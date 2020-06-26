#pragma once

#include "rcf-extensions/common.h"
#include "rcf-extensions/logging.h"
#include "rcf-extensions/round-robin-reinit-scheduler.h"

namespace rcf_extensions {

template <typename W>
RoundRobinReinitScheduler<W>::RoundRobinReinitScheduler(
    RCF::TcpEndpoint const& endpoint,
    worker_t&& worker,
    std::size_t num_threads_pre,
    std::size_t num_threads_post,
    std::size_t num_max_connections) :
    m_log{log4cxx::Logger::getLogger("lib-rcf.RoundRobinReinitScheduler")},
    m_input_queue{new input_queue_t},
    m_output_queue{new output_queue_t{num_threads_post}},
    m_session_storage{new session_storage_t},
    m_worker_thread{new worker_thread_t{std::move(worker), *m_input_queue, *m_output_queue,
                                        *m_session_storage}},
    m_idle_timeout{new idle_timeout_t{*m_worker_thread}}
{
	using namespace std::chrono_literals;

	// per default we allow up to 10 seconds per user (tbd in the future)
	m_input_queue->set_period_per_user(10s);

	RCF::init();
	m_server = std::make_unique<RCF::RcfServer>(endpoint);
	m_server->getServerTransport().setConnectionLimit(num_max_connections);
	// Thread pool with fixed number of threads
	RCF::ThreadPoolPtr tpPtr(new RCF::ThreadPool(num_threads_pre));
	m_server->setThreadPool(tpPtr);
}

template <typename W>
RoundRobinReinitScheduler<W>::~RoundRobinReinitScheduler()
{
	RCF_LOG_DEBUG(m_log, "Preparing to shut down!");
	{
		if (!m_input_queue->is_empty()) {
			RCF_LOG_ERROR(m_log, "Work left in input queue on shutdown, this should not happen!");
		}
	}

	// Delete in reverse order
	m_idle_timeout.reset();
	m_worker_thread.reset();
	m_session_storage.reset();
	m_output_queue.reset();
	m_input_queue.reset();

	// destruct server prior to deinit
	RCF_LOG_DEBUG(m_log, "Deleting RcfServer..");
	m_server.reset();
	RCF_LOG_DEBUG(m_log, "Deleted RcfServer.");

	RCF_LOG_DEBUG(m_log, "RCF::deinit");
	RCF::deinit();
	RCF_LOG_DEBUG(m_log, "Shutdown finished");
}

template <typename W>
bool RoundRobinReinitScheduler<W>::reinit_notify()
{
	auto verified_user_session_id = get_verified_user_data<bool>(*m_worker_thread);
	if (verified_user_session_id) {
		m_session_storage->reinit_handle_notify(verified_user_session_id->second);
		RCF_LOG_TRACE(
		    m_log, "[" << verified_user_session_id->second
		               << "] Reinit program notification successfully processed.");
	}
	return true;
}

template <typename W>
void RoundRobinReinitScheduler<W>::reinit_upload(reinit_data_t reinit_data)
{
	auto verified_user_session_id = get_verified_user_data<void, reinit_data_t>(*m_worker_thread);
	if (verified_user_session_id) {
		m_session_storage->reinit_store(verified_user_session_id->second, std::move(reinit_data));
		RCF_LOG_TRACE(
		    m_log,
		    "[" << verified_user_session_id->second << "] Reinit program successfully uploaded.");
	}
}

template <typename W>
bool RoundRobinReinitScheduler<W>::start_server(std::chrono::seconds const& timeout)
{
	m_worker_thread->start();
	m_server->start();
	return m_idle_timeout->wait_until_idle_for(timeout);
	// NOTE: Server needs to be destroyed to shutdown.
}

template <typename W>
typename RoundRobinReinitScheduler<W>::work_return_t RoundRobinReinitScheduler<W>::submit_work(
    work_argument_t work, SequenceNumber sequence_num, bool enforce_reinit)
{
	std::ignore = work;

	RCF_LOG_TRACE(m_log, "Handling new submission..");

	auto verified_user_session_id =
	    get_verified_user_data<work_return_t, work_argument_t, SequenceNumber, bool>(
	        *m_worker_thread);

	if (!verified_user_session_id) {
		return work_return_t{}; // result submitted to client asynchronously
	}

	auto user_id = detail::round_robin_scheduler::get_user_id(verified_user_session_id);
	auto session_id = detail::round_robin_scheduler::get_session_id(verified_user_session_id);

	RCF_LOG_TRACE(m_log, "[" << session_id << "] Handling submission " << sequence_num);

	m_session_storage->ensure_registered(session_id);

	if (enforce_reinit) {
		m_session_storage->reinit_set_needed(session_id);
	}

	m_input_queue->add_work(
	    work_package_t{user_id, session_id, work_context_t(RCF::getCurrentRcfSession()),
	                   sequence_num},
	    m_session_storage->get_heap_sorter_most_completed());
	RCF_LOG_TRACE(m_log, "[" << session_id << "] Submission " << sequence_num << " handled.");
	// notify the worker thread of work
	m_worker_thread->notify();
	return work_return_t{}; // result submitted to client asynchronously
}

} // namespace rcf_extensions
