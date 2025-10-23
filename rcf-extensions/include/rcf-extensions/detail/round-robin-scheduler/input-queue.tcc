#include "rcf-extensions/detail/round-robin-scheduler/input-queue.h"
#include "rcf-extensions/logging.h"

#include <algorithm>

namespace rcf_extensions::detail::round_robin_scheduler {

template <typename W>
InputQueue<W>::InputQueue() :
    m_log{log4cxx::Logger::getLogger("lib-rcf.InputQueue")},
    m_last_user_switch{std::chrono::system_clock::now()},
    m_period_per_user{0}
{}

template <typename W>
InputQueue<W>::~InputQueue()
{
	RCF_LOG_TRACE(m_log, "Shutting down..");
	std::lock_guard const lk{m_mutex};
	if (!is_empty_while_locked()) {
		RCF_LOG_ERROR(m_log, "Work left in input queue on shutdown, this should not happen!");
	}
	RCF_LOG_TRACE(m_log, "Shut down.");
}

template <typename W>
template <typename SorterT>
void InputQueue<W>::add_work(work_package_t&& pkg, SorterT const& sorter)
{
	std::unique_lock lk{m_mutex};

	// check job count for user and add new user queue if no previous jobs exist
	RCF_LOG_TRACE(m_log, "Adding new work for user " << pkg.user_id);
	user_id_t const user_id{pkg.user_id};
	if (!m_user_to_input_queue.contains(user_id)) {
		RCF_LOG_TRACE(m_log, "User " << user_id << " had no work queued up until now.");
		// no jobs for current user -> register user
		m_user_list.push_back(user_id);
		m_user_to_input_queue.insert(std::make_pair(user_id, queue_t()));
		m_user_to_mutex.insert(std::make_pair(user_id, std::make_unique<std::mutex>()));
	}

	// select current user if we had no work before
	if (m_user_list.size() == 1) {
		RCF_LOG_TRACE(m_log, "There is only one user.");
		m_it_current_user = m_user_list.cbegin();
		reset_last_user_switch_while_locked();
	}

	std::lock_guard user_lk(*m_user_to_mutex[user_id]);
	queue_t& user_queue = m_user_to_input_queue.at(user_id);
	lk.unlock();

	ensure_heap_while_locked(user_queue, sorter);

	// store the job
	user_queue.push_back(std::move(pkg));
	std::push_heap(user_queue.begin(), user_queue.end(), sorter);

	RCF_LOG_TRACE(
	    m_log,
	    "Number of jobs left for user " << user_id << " after adding: " << user_queue.size());
}

template <typename W>
template <typename SorterT>
typename InputQueue<W>::work_package_t InputQueue<W>::retrieve_work(SorterT const& sorter)
{
	std::unique_lock lk{m_mutex};

	// there should always be work to retrieve, because otherwise we do not
	// leave the while loop in worker_main_thread()
	if (is_empty_while_locked()) {
		throw std::runtime_error("Tried to retrieve work from empty queue.");
	}

	if (m_user_to_input_queue[*m_it_current_user].size() == 0 || is_time_to_switch_user()) {
		advance_user_while_locked();
	}

	user_id_t current_user_id = *m_it_current_user;

	if (m_log->isEnabledFor(log4cxx::Level::getDebug())) {
		std::stringstream ss;
		ss << "Current users:";
		for (auto const& user : m_user_list) {
			if (user == current_user_id) {
				ss << " [" << user << "]";
			} else {
				ss << " " << user;
			}
		}
		ss << ".";
		RCF_LOG_DEBUG(m_log, ss.str());
	}

	std::lock_guard lk_user(*m_user_to_mutex[current_user_id]);
	queue_t& queue = m_user_to_input_queue[current_user_id];
	lk.unlock();

	// retrieve next job for current user
	BOOST_ASSERT(queue.size() > 0);
	std::pop_heap(queue.begin(), queue.end(), sorter);
	work_package_t pkg = std::move(queue.back());
	queue.pop_back();

	RCF_LOG_DEBUG(
	    m_log, "Number of jobs left for user " << current_user_id << ": " << queue.size());

	return pkg;
}

template <typename W>
bool InputQueue<W>::is_empty() const
{
	std::lock_guard const lk{m_mutex};
	return is_empty_while_locked();
}

template <typename W>
bool InputQueue<W>::is_empty_while_locked() const
{
	// The current user might have a queue size of zero as their queue only gets deleted once we
	// switch FROM the users
	// -> ergo, if we have more than one user there is work left to do
	if (m_user_list.size() == 0) {
		return true;
	} else if (m_user_list.size() == 1) {
		// acquire lock on user queue to check how many jobs there are
		std::lock_guard user_lk{*m_user_to_mutex[*m_it_current_user]};
		return m_user_to_input_queue.at(*m_it_current_user).size() == 0;
	} else {
		return false;
	}
}

template <typename W>
void InputQueue<W>::advance_user()
{
	std::lock_guard const lk{m_mutex};
	advance_user_while_locked();
}

template <typename W>
void InputQueue<W>::set_period_per_user(std::chrono::milliseconds period)
{
	m_period_per_user = period;
}

template <typename W>
std::chrono::milliseconds InputQueue<W>::get_period_per_user() const
{
	return m_period_per_user;
}

template <typename W>
void InputQueue<W>::advance_user_while_locked()
{
	auto const citer_previous_user = m_it_current_user++;

	// wrap user iterator
	if (m_it_current_user == m_user_list.cend()) {
		if (m_user_list.size() > 0) {
			RCF_LOG_TRACE(m_log, "User iterator wrapping back to " << *m_user_list.cbegin() << ".");
			m_it_current_user = m_user_list.cbegin();
		} else {
			RCF_LOG_ERROR(m_log, "No users left.");
			throw std::runtime_error("No user left.");
		}
	}

	RCF_LOG_TRACE(
	    m_log,
	    "Advancing from user " << (*citer_previous_user) << " to " << (*m_it_current_user) << ".");

	// note when we switched users
	reset_last_user_switch_while_locked();

	// check if the old user has no jobs left
	std::unique_lock user_lk{*m_user_to_mutex[*citer_previous_user]};
	if (m_user_to_input_queue[*citer_previous_user].size() == 0) {
		RCF_LOG_DEBUG(
		    m_log, "No jobs left for " << (*citer_previous_user) << ".. removing from queue.");
		m_user_to_input_queue.erase(*citer_previous_user);
		user_lk.unlock();
		m_user_to_mutex.erase(*citer_previous_user);
		RCF_LOG_TRACE(
		    m_log, "No pending jobs left for user " << (*citer_previous_user)
		                                            << " -> removing from active users-list.");
		m_user_list.erase(citer_previous_user); // invalidates citer_previous_user
	}
}

template <typename W>
void InputQueue<W>::reset_timeout_user_switch()
{
	std::lock_guard const lk{m_mutex};
	reset_last_user_switch_while_locked();
}

template <typename W>
void InputQueue<W>::reset_last_user_switch_while_locked()
{
	RCF_LOG_TRACE(m_log, "Resetting last user switched.");
	m_last_user_switch = std::chrono::system_clock::now();
}

template <typename W>
bool InputQueue<W>::is_time_to_switch_user()
{
	using namespace std::chrono_literals;

	auto duration_current_user = (std::chrono::system_clock::now() - m_last_user_switch);
	RCF_LOG_TRACE(
	    m_log,
	    "Current user "
	        << *m_it_current_user << " active for "
	        << std::chrono::duration_cast<std::chrono::milliseconds>(duration_current_user).count()
	        << "ms. [Max time: "
	        << std::chrono::duration_cast<std::chrono::milliseconds>(m_period_per_user).count()
	        << "ms].");
	return (m_period_per_user == 0ms) || duration_current_user > m_period_per_user;
}

template <typename W>
template <typename SorterT>
void InputQueue<W>::ensure_heap_while_locked(queue_t& queue, SorterT const& sorter)
{
	if (!std::is_heap(queue.begin(), queue.end(), sorter)) {
		std::make_heap(queue.begin(), queue.end(), sorter);
	}
}

template <typename W>
std::size_t InputQueue<W>::get_total_job_count() const
{
	std::lock_guard const lk{m_mutex};
	return get_total_job_count_while_locked();
}

template <typename W>
std::size_t InputQueue<W>::get_total_job_count_while_locked() const
{
	std::size_t count = 0;
	for (auto& queue : m_user_to_input_queue) {
		count += queue.second.size();
	}
	return count;
}

} // namespace rcf_extensions::detail::round_robin_scheduler
