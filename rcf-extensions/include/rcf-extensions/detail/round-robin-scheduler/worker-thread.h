#pragma once

#include "rcf-extensions/detail/round-robin-scheduler/input-queue.h"
#include "rcf-extensions/detail/round-robin-scheduler/output-queue.h"
#include "rcf-extensions/detail/round-robin-scheduler/work-methods.h"

#include <RCF/RCF.hpp>

#include <chrono>
#include <condition_variable>
#include <thread>

namespace log4cxx {

class Logger;

} // namespace log4cxx


namespace rcf_extensions::detail::round_robin_scheduler {

/**
 * Worker thread that has exclusive access to the hardware resources and does the heavy lifting.
 *
 * Wraps the worker-object supplied from user side.
 *
 */
template <typename Worker>
class WorkerThread
{
public:
	using worker_t = Worker;
	using input_queue_t = InputQueue<worker_t>;
	using output_queue_t = OutputQueue<worker_t>;

	using work_methods_t = ::rcf_extensions::detail::round_robin_scheduler::work_methods<worker_t>;

	using optional_verified_user_data_t = typename work_methods_t::optional_verified_user_data_t;

	using work_argument_t = typename work_methods_t::work_argument_t;
	using work_return_t = typename work_methods_t::work_return_t;
	using work_context_t = typename work_methods_t::work_context_t;

	WorkerThread(worker_t&& worker, input_queue_t& input, output_queue_t& output);
	WorkerThread(WorkerThread&&) = delete;
	WorkerThread(WorkerThread const&) = delete;
	virtual ~WorkerThread();

	void set_release_interval(std::chrono::seconds const& s)
	{
		m_teardown_period = s;
	}

	bool is_set_up() const
	{
		return m_is_set_up;
	}

	bool is_idle() const
	{
		return m_is_idle;
	}

	/**
	 * Notify worker about new work to be performed.
	 */
	void notify()
	{
		RCF_LOG_TRACE(m_log, "Notifying..");
		m_cv.notify_one();
	}

	auto get_last_idle() const
	{
		if (m_is_idle) {
			return m_last_idle;
		} else {
			return std::chrono::system_clock::now();
		}
	}

	auto get_last_release() const
	{
		return m_last_release;
	}

	optional_verified_user_data_t verify_user(std::string const& data)
	{
		return m_worker.verify_user(data);
	}

	std::chrono::milliseconds get_time_till_next_teardown() const;

	/**
	 * Start the worker thread.
	 */
	void start();

	/**
	 * Manually reset last idle timer.
	 */
	void reset_last_idle();

	/**
	 * Apply a visitor to the const worker object and return the result.
	 *
	 * Another way of phrasing would be to apply a lens to the worker object.
	 */
	template <typename VisitorT>
	auto visit_const(VisitorT) const;

	/**
	 * Apply a visitor to the const worker object and return the result.
	 * The worker is guaranteed to be set up while being visited.
	 *
	 * Another way of phrasing would be to apply a lens to the worker object.
	 */
	template <typename VisitorT>
	auto visit_set_up_const(VisitorT);

protected:
	log4cxx::Logger* m_log;

	/**
	 * If the worker is set up it holds resources required to perform its task (e.g. slurm
	 * allocations).
	 */
	bool m_is_set_up;
	/**
	 * State of the execution thread (internally used).
	 */
	bool m_running;
	/**
	 * The worker is idle if it is not performing its task.
	 */
	bool m_is_idle;

	worker_t m_worker;
	input_queue_t& m_input;
	output_queue_t& m_output;

	std::jthread m_thread;
	mutable std::mutex m_mutex;
	std::condition_variable_any m_cv;

	std::chrono::system_clock::time_point m_last_release;
	std::chrono::system_clock::time_point m_last_idle;
	std::chrono::seconds m_teardown_period;

	virtual void main_thread(std::stop_token);

	auto lock() const
	{
		return std::unique_lock<std::mutex>(m_mutex);
	}

	auto lock_guard() const
	{
		return std::lock_guard<std::mutex>(m_mutex);
	}

	/**
	 * Check if a teardown is needed.
	 */
	bool is_teardown_needed();

	/**
	 * Reset the time the worker was last released.
	 */
	void reset_last_release();

	/**
	 * Make sure that the worke is set up and return if we had to set up the
	 * data and need perform a reinit if available.
	 *
	 * @return Whether or not the worker was already up.
	 */
	bool ensure_worker_is_set_up();

	/**
	 * Perform a worker teardown.
	 */
	void perform_teardown();

	void set_idle();
	void set_busy();
};

} // namespace rcf_extensions::detail::round_robin_scheduler

#include "rcf-extensions/detail/round-robin-scheduler/worker-thread.tcc"
