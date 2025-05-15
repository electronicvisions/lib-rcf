#include "rcf-extensions/detail/round-robin-scheduler/worker-thread.h"
#include "rcf-extensions/logging.h"
#include <atomic>

namespace rcf_extensions::detail::round_robin_scheduler {

template <typename W>
WorkerThread<W>::WorkerThread(worker_t&& worker, input_queue_t& input, output_queue_t& output) :
    m_log(log4cxx::Logger::getLogger("lib-rcf.WorkerThread")),
    m_is_set_up{false},
    m_running{false},
    m_is_idle{true},
    m_worker{std::move(worker)},
    m_input{input},
    m_output{output},
    m_last_release{std::chrono::system_clock::now()},
    m_last_idle{std::chrono::system_clock::now()}
{}

template <typename W>
WorkerThread<W>::~WorkerThread()
{
	using namespace std::chrono_literals;

	RCF_LOG_TRACE(m_log, "Shutting down..");
	m_thread.request_stop();
	notify();
	while (m_running) {
		notify();
		std::this_thread::sleep_for(100ms);
	}
	RCF_LOG_TRACE(m_log, "Joining main thread.");
	m_thread.join();
	RCF_LOG_TRACE(m_log, "Shut down.");
}

template <typename W>
void WorkerThread<W>::start()
{
	if (!m_running) {
		m_running = true;
		m_thread = std::jthread{[this](std::stop_token st) { main_thread(std::move(st)); }};
	}
}

template <typename W>
std::chrono::milliseconds WorkerThread<W>::get_time_till_next_teardown() const
{
	auto now = std::chrono::system_clock::now();
	return m_teardown_period -
	       std::chrono::duration_cast<std::chrono::milliseconds>(now - get_last_release());
}

template <typename W>
void WorkerThread<W>::notify()
{
	std::atomic_thread_fence(std::memory_order_release);
	RCF_LOG_TRACE(m_log, "Notifying..");
	m_cv.notify_one();
}

template <typename W>
void WorkerThread<W>::main_thread(std::stop_token st)
{
	auto lk = lock();
	RCF_LOG_TRACE(m_log, "Worker starting up.");

	while (!st.stop_requested()) {
		// teardown needs to be done periodically
		if (is_set_up() && is_teardown_needed()) {
			RCF_LOG_TRACE(m_log, "Tearing down worker because of time constraints.");
			perform_teardown();
		}

		// sleep worker if there is no work
		if (m_input.is_empty()) {
			set_idle();
			if (is_set_up()) {
				// worker is still set up so we can only sleep until the next release
				m_cv.wait_for(lk, get_time_till_next_teardown(), [this, st] {
					return st.stop_requested() || !m_input.is_empty();
				});
				RCF_LOG_TRACE(m_log, "Woke up while worker still set up.");
			} else {
				// no work to be done -> sleep until needed
				m_cv.wait(lk, [this, st] { return st.stop_requested() || !m_input.is_empty(); });
				RCF_LOG_TRACE(m_log, "Woke up while worker NOT set up.");
			}
		}
		if (st.stop_requested()) {
			RCF_LOG_TRACE(m_log, "Shutdown requested..");
			break;
		}
		if (m_input.is_empty()) {
			// no work to do -> do not advance
			continue;
		}

		ensure_worker_is_set_up();

		auto pkg = m_input.retrieve_work();

		auto const& work = pkg.context.parameters().a1.get();

		set_busy();
		try {
			// For workers with reinit functionality (which we test via availability of the perform_reinit
			// method), we expect two arguments, since in addition to the work, we get the session id. For
			// workers without reinit functionality, we expect one argument, the work.
			if constexpr (trait::has_method_perform_reinit<W>::value) {
				auto retval = m_worker.work(work, pkg.session_id);
				pkg.context.parameters().r.set(retval);
			} else {
				auto retval = m_worker.work(work);
				pkg.context.parameters().r.set(retval);
			}
			m_output.push_back(std::move(pkg.context));
		} catch (std::exception& e) {
			RCF_LOG_ERROR(m_log, pkg << " encountered exception: " << e.what());
			pkg.context.commit(e);
			// After exception we need to tear the worker down.
			perform_teardown();
		}
	}
	// We need to tear down the worker inside the main thread.
	// Background:
	// One application (sctrltp's hostarq-daemon) registers a parent-death
	// signal that gets issued when the parent thread (NOT parent process)
	// dies. Hence, if the worker was not torn down in the main thread, the
	// hostarq-daemon would receive a parent death signal which would trigger
	// its early termination which in turn would disrupt regular shutdown of
	// sctrltp::ARQStream (in hxcomm::ARQConnection).
	m_worker.teardown();
	m_running = false;
	RCF_LOG_TRACE(m_log, "main_thread() shut down.");
}

template <typename W>
bool WorkerThread<W>::is_teardown_needed()
{
	using namespace std::chrono_literals;

	// There are two conditions for tearing down the worker
	// 1. we have reached the end of the current period
	// 2. if there is no period we have to tear down the worker whenever there is no work
	if (m_teardown_period > 0s) {
		auto now = std::chrono::system_clock::now();
		return (now - get_last_release()) >= m_teardown_period;
	} else {
		return m_input.is_empty();
	}
}

template <typename W>
void WorkerThread<W>::reset_last_release()
{
	m_last_release = std::chrono::system_clock::now();
}

template <typename W>
void WorkerThread<W>::reset_last_idle()
{
	m_last_idle = std::chrono::system_clock::now();
}

template <typename W>
template <typename VisitorT>
auto WorkerThread<W>::visit_const(VisitorT visitor) const
{
	worker_t const& cref{m_worker};
	return visitor(cref);
}

template <typename W>
template <typename VisitorT>
auto WorkerThread<W>::visit(VisitorT visitor)
{
	worker_t& ref{m_worker};
	return visitor(ref);
}

template <typename W>
template <typename VisitorT>
auto WorkerThread<W>::visit_set_up_const(VisitorT visitor)
{
	if (!is_set_up()) {
		auto const lk = lock_guard();
		ensure_worker_is_set_up();
	}
	worker_t const& cref{m_worker};
	return visitor(cref);
}

template <typename W>
bool WorkerThread<W>::ensure_worker_is_set_up()
{
	if (!is_set_up()) {
		m_worker.setup();
		if (m_input.get_period_per_user() < m_teardown_period) {
			// in case we tear down less often than switch users, we reset the
			// timeout because otherwise the user could switch immediately
			m_input.reset_timeout_user_switch();
		}
		m_is_set_up = true;
		reset_last_release();
		set_idle();
		return false;
	} else {
		return true;
	}
}

template <typename W>
void WorkerThread<W>::perform_teardown()
{
	// up until now we were not idle
	set_idle();
	m_worker.teardown();
	m_is_set_up = false;
}

template <typename W>
void WorkerThread<W>::set_idle()
{
	reset_last_idle();
	m_is_idle = true;
}

template <typename W>
void WorkerThread<W>::set_busy()
{
	m_is_idle = false;
}

} // namespace rcf_extensions::detail::round_robin_scheduler
