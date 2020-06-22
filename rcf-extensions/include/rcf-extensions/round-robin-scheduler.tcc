#pragma once

#include "rcf-extensions/common.h"
#include "rcf-extensions/logging.h"
#include "rcf-extensions/round-robin-scheduler.h"

namespace rcf_extensions {

template <typename W>
RoundRobinScheduler<W>::RoundRobinScheduler(
    RCF::TcpEndpoint const& endpoint,
    worker_t&& worker,
    size_t num_threads_pre,
    size_t num_threads_post) :
    m_log(log4cxx::Logger::getLogger("lib-rcf.RoundRobinScheduler")),
    m_input_queue{new input_queue_t},
    m_output_queue{new output_queue_t{num_threads_post}},
    m_worker(std::move(worker)),
    m_worker_is_set_up(false),
    m_worker_last_release(std::chrono::system_clock::time_point::min()),
    m_worker_last_idle(std::chrono::system_clock::now()),
    m_teardown_period(0)
{
	m_stop_flag = false; // no other threads running

	RCF::init();

	m_server.reset(new RCF::RcfServer(endpoint));

	// Thread pool with fixed number of threads
	RCF::ThreadPoolPtr tpPtr(new RCF::ThreadPool(num_threads_pre));
	m_server->setThreadPool(tpPtr);

	// set up worker
	m_worker_thread.reset(
	    new RCF::Thread(std::bind(&RoundRobinScheduler<W>::worker_main_thread, this)));
}

template <typename W>
RoundRobinScheduler<W>::~RoundRobinScheduler()
{
	if (!m_stop_flag) {
		RCF_LOG_DEBUG(m_log, "Preparing to shut down!");
		m_stop_flag = true;
		RCF_LOG_DEBUG(m_log, "Notifying worker..");
		notify_worker();
		RCF_LOG_DEBUG(m_log, "Joining worker thread..");
		m_worker_thread->join();
		{
			// tear worker down to be sure
			RCF_LOG_DEBUG(m_log, "Tearing down worker.");
			auto const lk = m_input_queue->lock_guard();
			m_worker.teardown();
			RCF_LOG_DEBUG(m_log, "Teardown finished");
		}
		m_output_queue.reset();
		LOG4CXX_DEBUG(m_log, "Notifying timeout thread");
		{
			RCF::Mutex mutex;
			RCF::Lock lock(mutex);
			m_cond_timeout.notify_all();
		}

		m_input_queue.reset();

		RCF_LOG_DEBUG(m_log, "Resetting server");
		// destruct server prior to deinit
		m_server.reset();
		RCF_LOG_DEBUG(m_log, "RCF::deinit");
		RCF::deinit();
		RCF_LOG_DEBUG(m_log, "Shutdown finished");
	}
}

template <typename W>
void RoundRobinScheduler<W>::start_server(std::chrono::seconds const& timeout)
{
	m_timeout = timeout;
	m_server->start();

	server_idle_timeout();
}


template <typename W>
void RoundRobinScheduler<W>::server_idle_timeout()
{
	using namespace std::chrono_literals;

	m_worker_last_idle = std::chrono::system_clock::now();

	RCF::Mutex mutex_timeout;
	RCF::Lock lock_timeout(mutex_timeout);
	bool timeout_reached = false;

	// set idle timeout routine
	do {
		if (m_timeout > 0s) {
			std::chrono::milliseconds duration_till_timeout;
			{
				// m_worker_last_idle protected by input queue mutex
				auto input_lock = m_input_queue->lock_guard();
				if (m_worker_is_set_up) {
					duration_till_timeout = get_time_till_next_teardown();
				} else {
					duration_till_timeout = get_time_till_timeout();
				}
			}

			// sleep at least a second before checking for timeout again
			std::chrono::milliseconds duration_sleep_min = 1s;
			m_cond_timeout.wait_for(
			    lock_timeout, std::max(duration_till_timeout, duration_sleep_min));

			{
				auto input_lock = m_input_queue->lock_guard();
				timeout_reached = !m_worker_is_set_up && (std::chrono::system_clock::now() -
				                                          m_worker_last_idle) > m_timeout;
			}
		} else {
			// there is no time-out - we just wait forever (i.e. till the user aborts)
			m_cond_timeout.wait(lock_timeout);
			// will never be reached because we wait for user input to abort
			timeout_reached = true;
		}
	} while (!timeout_reached && !m_stop_flag);
}

template <typename W>
typename RoundRobinScheduler<W>::work_return_t RoundRobinScheduler<W>::submit_work(
    work_argument_t work, SequenceNumber sequence_num)
{
	std::ignore = work; // captured in session object

	std::string user_data = RCF::getCurrentRcfSession().getRequestUserData();

	std::optional<user_id_t> user_id = m_worker.verify_user(user_data);

	if (!user_id) {
		work_context_t context{RCF::getCurrentRcfSession()};
		context.commit(UserNotAuthorized());
		// dummy data not passed to client
		return RoundRobinScheduler<W>::work_return_t();
	}

	m_input_queue->add_work(
	    work_package_t{*user_id, work_context_t{RCF::getCurrentRcfSession()}, sequence_num});

	// notify the worker thread of work
	notify_worker();

	return RoundRobinScheduler<W>::work_return_t(); // not passed to client
}

template <typename W>
void RoundRobinScheduler<W>::notify_worker()
{
	RCF::Lock lock(m_mutex_notify_worker);
	m_cond_worker.notify_one();
}

template <typename W>
void RoundRobinScheduler<W>::worker_main_thread()
{
	using namespace std::chrono_literals;

	auto lock_input_queue = m_input_queue->lock();
	while (true) {
		// teardown needs to be done periodically
		if (m_worker_is_set_up && is_teardown_needed(lock_input_queue)) {
			m_worker.teardown();
			m_worker_last_idle = std::chrono::system_clock::now();
			m_worker_is_set_up = false;
		}

		lock_input_queue.unlock();
		// sleep worker if there is no work
		if (m_input_queue->is_empty()) {
			lock_input_queue.lock();
			if (m_worker_is_set_up) {
				// worker is still set up so we can only sleep until the next release
				m_cond_worker.wait_for(
				    lock_input_queue, std::min(get_time_till_next_teardown(), 1000ms));
			} else {
				// no work to be done -> sleep until needed
				m_cond_worker.wait(lock_input_queue);
			}
			lock_input_queue.unlock();
		}
		if (m_stop_flag) {
			break;
		}
		if (m_input_queue->is_empty()) {
			lock_input_queue.lock();
			// no work to do -> do not advance
			continue;
		}
		if (!m_worker_is_set_up) {
			// worker is starting up -> perform setup
			m_worker.setup();
			m_worker_is_set_up = true;
			m_worker_last_release = std::chrono::system_clock::now();
		}
		work_context_t context = std::move(m_input_queue->retrieve_work().context);

		work_argument_t work = context.parameters().a1.get();
		work_return_t retval = m_worker.work(work);
		context.parameters().r.set(retval);

		m_output_queue->push_back(std::move(context));

		lock_input_queue.lock();
	}
}

template <typename W>
bool RoundRobinScheduler<W>::is_teardown_needed(RCF::Lock& lock_input_queue)
{
	using namespace std::chrono_literals;

	// lock argument is merely to indicate, that this method should only be
	// called when the input queue is locked
	BOOST_ASSERT(lock_input_queue.owns_lock());

	// there are two conditions for tearing down the worker
	// 1. we have reached the end of the current period
	// 2. if there is no period we have to tear down the worker whenever there is no work
	if (m_teardown_period > 0s &&
	    m_worker_last_release > std::chrono::system_clock::time_point::min()) {
		auto now = std::chrono::system_clock::now();
		return (now - m_worker_last_release) >= m_teardown_period;
	} else {
		// NOTE: This code is only temporary until a dedicated worker thread is introduced.
		lock_input_queue.unlock();
		auto retval = m_input_queue->is_empty();
		lock_input_queue.lock();
		return retval;
	}
}

template <typename W>
std::chrono::milliseconds RoundRobinScheduler<W>::get_time_till_next_teardown()
{
	using namespace std::chrono_literals;
	if (m_teardown_period > 0ms) {
		auto now = std::chrono::system_clock::now();
		return m_teardown_period -
		       std::chrono::duration_cast<std::chrono::milliseconds>(now - m_worker_last_release);
	} else {
		return std::chrono::milliseconds::max();
	}
}

template <typename W>
std::chrono::milliseconds RoundRobinScheduler<W>::get_time_till_timeout()
{
	using namespace std::chrono_literals;
	if (m_timeout > 0ms) {
		auto now = std::chrono::system_clock::now();
		return m_timeout -
		       std::chrono::duration_cast<std::chrono::milliseconds>(now - m_worker_last_idle);
	} else {
		return std::chrono::milliseconds::max();
	}
}

} // namespace rcf_extensions
