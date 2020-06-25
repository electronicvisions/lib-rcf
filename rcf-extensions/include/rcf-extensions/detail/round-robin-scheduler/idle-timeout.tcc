
#include "rcf-extensions/detail/round-robin-scheduler/idle-timeout.h"

namespace rcf_extensions::detail::round_robin_scheduler {

template <typename W>
IdleTimeout<W>::~IdleTimeout()
{
	RCF_LOG_TRACE(m_log, "Shutting down..")
	m_stop_flag = true;
	// busy wait for other thread to exit
	while (m_num_threads_idling > 0) {
		RCF_LOG_TRACE(m_log, "Other thread still idling, waiting..");
		notify();
		std::this_thread::sleep_for(10ms);
	}
	RCF_LOG_TRACE(m_log, "Shut down.");
}

template <typename W>
bool IdleTimeout<W>::wait_until_idle_for(std::chrono::seconds timeout)
{
	using namespace std::chrono_literals;

	++m_num_threads_idling;

	m_timeout = timeout;
	bool timeout_reached;

	if (m_timeout > 0s) {
		RCF_LOG_INFO(
		    m_log, "Running until idle for "
		               << std::chrono::duration_cast<std::chrono::seconds>(m_timeout).count()
		               << "s.");
	} else {
		RCF_LOG_INFO(m_log, "Running indefinitely.");
	}

	auto lk = lock();

	// set idle timeout routine
	do {
		if (m_timeout > 0s) {
			// sleep at least a second before checking for timeout again
			m_cv.wait_for(lk, std::max(get_duration_till_timeout(), 1000ms));

			timeout_reached = is_timeout_reached();
		} else {
			// there is no time-out - we just wait forever (i.e. till the user aborts)
			m_cv.wait(lk);
			// will never be reached because we wait for user input to abort
			timeout_reached = true;
		}
	} while (!timeout_reached && !m_stop_flag);

	--m_num_threads_idling;
	if (timeout_reached) {
		RCF_LOG_INFO(m_log, "Timeout reached.");
	} else {
		RCF_LOG_TRACE(m_log, "Shutting down before timeout was reached.");
	}
	return timeout_reached;
}

template <typename W>
std::chrono::milliseconds IdleTimeout<W>::get_duration_till_timeout()
{
	auto now = std::chrono::system_clock::now();
	return m_timeout - std::chrono::duration_cast<std::chrono::milliseconds>(
	                       now - m_worker_thread.get_last_idle());
}

template <typename W>
bool IdleTimeout<W>::is_timeout_reached()
{
	return std::chrono::system_clock::now() - m_worker_thread.get_last_idle() > m_timeout;
}

} // namespace rcf_extensions::detail::round_robin_scheduler
