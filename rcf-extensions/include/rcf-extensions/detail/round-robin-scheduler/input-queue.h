#pragma once

#include <chrono>
#include <deque>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "rcf-extensions/detail/round-robin-scheduler/work-methods.h"

#include <RCF/RCF.hpp>

namespace log4cxx {

class Logger;

} // namespace log4cxx

namespace rcf_extensions::detail::round_robin_scheduler {

/**
 * Helper functions to store and retieve incoming work packages.
 *
 * If the period per user is 0s (default) then we switch after every user.
 */
template <typename Worker>
class InputQueue
{
public:
	using worker_t = Worker;

	using work_argument_t = typename work_methods<worker_t>::work_argument_t;
	using work_return_t = typename work_methods<worker_t>::work_return_t;
	using work_package_t = typename work_methods<worker_t>::work_package_t;

	using user_id_t = typename work_methods<worker_t>::user_id_t;

	InputQueue();
	InputQueue(InputQueue const&) = delete;
	InputQueue(InputQueue&&) = delete;
	~InputQueue();

	auto lock() const
	{
		return std::unique_lock<std::mutex>(m_mutex);
	}

	auto lock_guard() const
	{
		return std::lock_guard<std::mutex>(m_mutex);
	}

	/**
	 * Add the given work package to the queue.
	 *
	 * @tparam SorterT struct that is used to sort work packages within a user's queue.
	 * @param SorterT custom sorter that might rely on runtime information in-between calls.
	 * @param work_package_t The package to add.
	 */
	template <typename SorterT = SortDescendingBySequenceNum>
	void add_work(work_package_t&&, SorterT const& = SorterT{});

	/**
	 * Retrieve the next work package in line from the queue. The work package
	 * is removed from the queue in the process.
	 *
	 * @tparam SorterT struct that is used to sort work packages within a user's queue.
	 * @param SorterT custom sorter that might rely on runtime information inbetween calls.
	 * @return The work package retrieved from the queue.
	 * @throws std::runtime_error if the queue is empty.
	 */
	template <typename SorterT = SortDescendingBySequenceNum>
	work_package_t retrieve_work(SorterT const& = SorterT{});

	/**
	 * @return Whether the queue is empty.
	 */
	bool is_empty() const;

	/**
	 * Advance to the next user in queue in round-robin fashion. Calling
	 * retrieve_work() will return work packages from that users queue.
	 *
	 * @throws std::runtime_error if the queue is empty.
	 */
	void advance_user();

	/**
	 * Set time period after which the user is forcibly switched even if there
	 * are jobs remaining.
	 *
	 * @param period Time after which the user is forcibly switched. If it is
	 * 0ms the user will not be switched.
	 */
	void set_period_per_user(std::chrono::milliseconds period)
	{
		m_period_per_user = period;
	}

	/**
	 * Get the time period after which the user is forcibly switched even if
	 * there are jobs remaining for the current user.
	 */
	std::chrono::milliseconds get_period_per_user() const
	{
		return m_period_per_user;
	}

	/**
	 * Get the total amount of jobs stored in input queue.
	 *
	 * @return Number of jobs currently stored in input queue.
	 */
	std::size_t get_total_job_count() const;

	/**
	 * Reset the timeout used for determining when to switch user.
	 *
	 * This only has an effect if get_period_per_user is larger than zero.
	 */
	void reset_timeout_user_switch();

private:
	mutable std::mutex m_mutex;

	log4cxx::Logger* m_log;

	using queue_t = std::deque<work_package_t>;
	using user_to_queue_t = std::unordered_map<user_id_t, queue_t>;
	user_to_queue_t m_user_to_input_queue;
	using user_to_mutex_t = std::unordered_map<user_id_t, std::unique_ptr<std::mutex>>;
	user_to_mutex_t m_user_to_mutex;

	using user_list_t = typename std::list<user_id_t>;
	using user_list_citer_t = typename user_list_t::const_iterator;
	user_list_t m_user_list;
	user_list_citer_t m_it_current_user;
	std::chrono::system_clock::time_point m_last_user_switch;

	std::chrono::milliseconds m_period_per_user;

	bool is_empty_while_locked() const;

	void advance_user_while_locked();

	void reset_last_user_switch_while_locked();

	bool is_time_to_switch_user();

	template <typename SorterT>
	void ensure_heap_while_locked(queue_t&, SorterT const&);

	std::size_t get_total_job_count_while_locked() const;
};

} // namespace rcf_extensions::detail::round_robin_scheduler

#include "rcf-extensions/detail/round-robin-scheduler/input-queue.tcc"
