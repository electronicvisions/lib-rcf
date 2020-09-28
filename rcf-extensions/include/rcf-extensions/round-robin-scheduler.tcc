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
    m_worker_thread{new worker_thread_t{std::move(worker), *m_input_queue, *m_output_queue}},
    m_idle_timeout{new idle_timeout_t{*m_worker_thread}}
{
	RCF::init();

	m_server.reset(new RCF::RcfServer(endpoint));

	// Thread pool with fixed number of threads
	RCF::ThreadPoolPtr tpPtr(new RCF::ThreadPool(num_threads_pre));
	m_server->setThreadPool(tpPtr);
}

template <typename W>
RoundRobinScheduler<W>::~RoundRobinScheduler()
{
	RCF_LOG_DEBUG(m_log, "Preparing to shut down!");

	// Delete in reverse order
	m_idle_timeout.reset();
	m_worker_thread.reset();
	m_output_queue.reset();
	m_input_queue.reset();

	RCF_LOG_DEBUG(m_log, "Resetting server");
	// destruct server prior to deinit
	m_server.reset();
	RCF_LOG_DEBUG(m_log, "RCF::deinit");
	RCF::deinit();
	RCF_LOG_DEBUG(m_log, "Shutdown finished");
}

template <typename W>
bool RoundRobinScheduler<W>::start_server(std::chrono::seconds const& timeout)
{
	m_worker_thread->start();
	m_server->start();
	return m_idle_timeout->wait_until_idle_for(timeout);
	// NOTE: Server needs to be destroyed to shutdown.
}

template <typename W>
typename RoundRobinScheduler<W>::work_return_t RoundRobinScheduler<W>::submit_work(
    work_argument_t work, SequenceNumber sequence_num)
{
	std::ignore = work; // captured in context object

	auto verified_user_data =
	    get_verified_user_data<work_return_t, work_argument_t, SequenceNumber>(*m_worker_thread);

	if (!verified_user_data) {
		// return early, exception already set
		return RoundRobinScheduler<W>::work_return_t();
	}

	m_input_queue->add_work(work_package_t{
	    detail::round_robin_scheduler::get_user_id(verified_user_data),
	    work_context_t{RCF::getCurrentRcfSession()}, sequence_num});

	// notify the worker thread of work
	m_worker_thread->notify();

	return RoundRobinScheduler<W>::work_return_t(); // not passed to client
}

} // namespace rcf_extensions
