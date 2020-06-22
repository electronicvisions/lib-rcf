#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>

#include "rcf-extensions/detail/round-robin-scheduler/work-methods.h"

#include <RCF/RCF.hpp>

namespace log4cxx {

class Logger;

} // namespace log4cxx

namespace rcf_extensions::detail::round_robin_scheduler {

/**
 * Helper functions to store and retieve incoming work packages.
 *
 */
template <typename Worker>
class OutputQueue
{
public:
	using worker_t = Worker;

	using work_argument_t = typename work_methods<worker_t>::work_argument_t;
	using work_return_t = typename work_methods<worker_t>::work_return_t;
	using work_context_t = typename work_methods<worker_t>::work_context_t;

	using user_id_t = typename work_methods<worker_t>::user_id_t;

	OutputQueue(size_t num_threads);
	OutputQueue(OutputQueue&&) = delete;
	OutputQueue(OutputQueue const&) = delete;
	~OutputQueue();

	void push_back(work_context_t&& context);

private:
	log4cxx::Logger* m_log;

	mutable std::mutex m_mutex;
	std::atomic<int> m_thread_count;

	std::deque<work_context_t> m_queue;
	std::vector<std::jthread> m_threads;
	std::condition_variable m_cv;

	void output_thread(std::stop_token);

	auto lock() const
	{
		return std::unique_lock<std::mutex>(m_mutex);
	}

	auto lock_guard() const
	{
		return std::lock_guard<std::mutex>(m_mutex);
	}

	void notify()
	{
		m_cv.notify_all();
	}
};

} // namespace rcf_extensions::detail::round_robin_scheduler

#include "rcf-extensions/detail/round-robin-scheduler/output-queue.tcc"
