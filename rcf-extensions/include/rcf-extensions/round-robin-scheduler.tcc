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
    m_worker_thread{new worker_thread_t{std::move(worker), *m_input_queue, *m_output_queue}}
{
	m_stop_flag = false; // no other threads running

	RCF::init();

	m_server.reset(new RCF::RcfServer(endpoint));

	// Thread pool with fixed number of threads
	RCF::ThreadPoolPtr tpPtr(new RCF::ThreadPool(num_threads_pre));
	m_server->setThreadPool(tpPtr);
}

template <typename W>
RoundRobinScheduler<W>::~RoundRobinScheduler()
{
	if (!m_stop_flag) {
		RCF_LOG_DEBUG(m_log, "Preparing to shut down!");
		m_stop_flag = true;
		m_worker_thread.reset();
		m_output_queue.reset();
		LOG4CXX_DEBUG(m_log, "Notifying timeout thread");
		m_cv_timeout.notify_all();

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
	m_worker_thread->start();
	m_server->start();

	server_idle_timeout();
}


template <typename W>
void RoundRobinScheduler<W>::server_idle_timeout()
{
	using namespace std::chrono_literals;

	RCF::Mutex mutex_timeout;
	RCF::Lock lock_timeout(mutex_timeout);
	bool timeout_reached = false;

	// set idle timeout routine
	do {
		if (m_timeout > 0s) {
			std::chrono::milliseconds duration_till_timeout;

			if (m_worker_thread->is_set_up()) {
				duration_till_timeout = m_worker_thread->get_time_till_next_teardown();
			} else {
				duration_till_timeout = get_time_till_timeout();
			}

			// sleep at least a second before checking for timeout again
			m_cv_timeout.wait_for(lock_timeout, std::max(duration_till_timeout, 1000ms));

			timeout_reached = is_timeout_reached();
		} else {
			// there is no time-out - we just wait forever (i.e. till the user aborts)
			m_cv_timeout.wait(lock_timeout);
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

	auto verified_user_id = m_worker_thread->verify_user(user_data);

	if (!verified_user_id) {
		work_context_t context(RCF::getCurrentRcfSession());
		context.commit(UserNotAuthorized());
		// dummy data not passed to client
		return RoundRobinScheduler<W>::work_return_t();
	}

	m_input_queue->add_work(work_package_t{
	    *verified_user_id, work_context_t{RCF::getCurrentRcfSession()}, sequence_num});

	// notify the worker thread of work
	m_worker_thread->notify();

	return RoundRobinScheduler<W>::work_return_t(); // not passed to client
}

template <typename W>
std::chrono::milliseconds RoundRobinScheduler<W>::get_time_till_timeout()
{
	auto now = std::chrono::system_clock::now();
	return m_timeout - std::chrono::duration_cast<std::chrono::milliseconds>(
	                       now - m_worker_thread->get_last_idle());
}

template <typename W>
bool RoundRobinScheduler<W>::is_timeout_reached() const
{
	return !m_worker_thread->is_set_up() &&
	       (std::chrono::system_clock::now() - m_worker_thread->get_last_idle()) > m_timeout;
}

} // namespace rcf_extensions
