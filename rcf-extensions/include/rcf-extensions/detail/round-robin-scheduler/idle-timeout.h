#pragma once

#include "rcf-extensions/logging.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>

namespace rcf_extensions::detail::round_robin_scheduler {

using namespace std::chrono_literals;

template <typename WorkerThread>
class IdleTimeout
{
public:
	using worker_thread_t = WorkerThread;

	IdleTimeout(worker_thread_t& worker_thread) :
	    m_log(log4cxx::Logger::getLogger("lib-rcf.IdleTimeout")),
	    m_stop_flag(false),
	    m_worker_thread(worker_thread),
	    m_timeout(0),
	    m_num_threads_idling(0){};
	IdleTimeout(IdleTimeout&&) = delete;
	IdleTimeout(IdleTimeout const&) = delete;

	~IdleTimeout();

	/**
	 * Block current thread and only return after worker_thread was idle for
	 * the specified duration.
	 *
	 * @return Whether or not the function returned due to idle timeout. The
	 * only reason to return false is if we are in shutdown-phase and
	 * IdleTimeout is being destroyed.
	 */
	bool wait_until_idle_for(std::chrono::seconds);

	std::chrono::milliseconds get_duration_till_timeout();

	bool is_timeout_reached();

private:
	mutable std::mutex m_mutex;
	log4cxx::Logger* m_log;

	bool m_stop_flag;
	worker_thread_t& m_worker_thread;
	std::chrono::seconds m_timeout;

	std::condition_variable m_cv;

	std::atomic<int> m_num_threads_idling;

	void notify()
	{
		m_cv.notify_all();
	}

	auto lock() const
	{
		return std::unique_lock<std::mutex>(m_mutex);
	}

	auto lock_guard() const
	{
		return std::lock_guard<std::mutex>(m_mutex);
	}
};

} // namespace rcf_extensions::detail::round_robin_scheduler

#include "rcf-extensions/detail/round-robin-scheduler/idle-timeout.tcc"
