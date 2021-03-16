#include "rcf-extensions/detail/round-robin-scheduler/worker-thread-reinit.h"

#include <exception>

namespace rcf_extensions::detail::round_robin_scheduler {

template <typename W>
WorkerThreadReinit<W>::WorkerThreadReinit(
    worker_t&& worker,
    input_queue_t& input,
    output_queue_t& output,
    session_storage_t& session_storage) :
    WorkerThread<W>{std::move(worker), input, output}, m_session_storage{session_storage}
{
	wtr_t::wtr_t::m_log = log4cxx::Logger::getLogger("lib-rcf.WorkerThreadReinit");
}


template <typename W>
void WorkerThreadReinit<W>::main_thread(std::stop_token st)
{
	using namespace std::chrono_literals;
	auto lk = wtr_t::lock();
	RCF_LOG_TRACE(wtr_t::m_log, "Worker starting up.");

	while (!st.stop_requested()) {
		RCF_LOG_TRACE(wtr_t::m_log, "New loop.");
		// teardown needs to be done periodically
		if (wtr_t::is_set_up() && wtr_t::is_teardown_needed()) {
			RCF_LOG_TRACE(wtr_t::m_log, "Tearing down worker because of time constraints.");
			perform_teardown();
		}

		// sleep worker if there is no work
		if (wtr_t::m_input.is_empty()) {
			wtr_t::set_idle();
			if (wtr_t::m_is_set_up) {
				RCF_LOG_TRACE(wtr_t::m_log, "Sleeping while worker still set up.");
				while (!wtr_t::is_teardown_needed() && wtr_t::m_input.is_empty() &&
				       !st.stop_requested()) {
					// We need to active wait because otherwise there is a chance to miss new work
					wtr_t::m_cv.wait_for(lk, std::min(100ms, wtr_t::get_time_till_next_teardown()));
				}
				RCF_LOG_TRACE(wtr_t::m_log, "Woke up while worker still set up.");
			} else {
				// no work to be done -> sleep until needed
				RCF_LOG_TRACE(wtr_t::m_log, "Sleeping while worker NOT set up.");
				while (wtr_t::m_input.is_empty() && !st.stop_requested()) {
					// We need to active wait because otherwise there is a chance to miss new work
					wtr_t::m_cv.wait_for(lk, 100ms);
				}
				RCF_LOG_TRACE(wtr_t::m_log, "Woke up while worker NOT set up.");
			}
		}
		if (st.stop_requested()) {
			RCF_LOG_TRACE(wtr_t::m_log, "Shutdown requested..");
			break;
		}
		RCF_LOG_DEBUG(
		    wtr_t::m_log, "Total count session/jobs: " << m_session_storage.get_total_refcount()
		                                               << " / "
		                                               << wtr_t::m_input.get_total_job_count());

		if (wtr_t::m_input.is_empty()) {
			// no work to do -> do not advance
			continue;
		}
		wtr_t::reset_last_idle();

		work_package_t pkg =
		    wtr_t::m_input.retrieve_work(m_session_storage.get_heap_sorter_most_completed());

		if (pkg.sequence_num) {
			RCF_LOG_TRACE(
			    wtr_t::m_log,
			    "[" << pkg.session_id << "] Retrieved #" << *(pkg.sequence_num) << " to work on.");
		} else {
			RCF_LOG_TRACE(
			    wtr_t::m_log,
			    "[" << pkg.session_id << "] Retrieved out-of-order package to work on.");
		}

		if (check_invalidity(pkg)) {
			// exception is already set in function -> just continue
			continue;
		}

		if (needs_delay(pkg)) {
			requeue_work_package(std::move(pkg));
			continue;
		}

		wtr_t::ensure_worker_is_set_up();

		if (!ensure_session_via_reinit(pkg)) {
			// switching session failed
			requeue_work_package(std::move(pkg));
			continue;
		}

		typename wtr_t::work_context_t context{std::move(pkg.context)};

		RCF_LOG_DEBUG(wtr_t::m_log, "Executing: " << pkg);
		auto work = context.parameters().a1.get();

		wtr_t::set_busy();
		try {
			auto retval = wtr_t::m_worker.work(work);
			context.parameters().r.set(retval);

			wtr_t::m_output.push_back(std::move(context));
		} catch (std::exception& e) {
			RCF_LOG_ERROR(wtr_t::m_log, pkg << " encountered exception: " << e.what());
			pkg.context.commit(e);
			// After exception we need to tear the worker down.
			perform_teardown();
		}

		if (pkg.sequence_num) {
			m_session_storage.sequence_num_next(pkg.session_id);
		}
	}
	RCF_LOG_TRACE(wtr_t::m_log, "main_thread() left loop.");
	// We need to tear down the worker inside the main thread.
	// Background:
	// One application (sctrltp's hostarq-daemon) registers a parent-death
	// signal that gets issued when the parent thread (NOT parent process)
	// dies. Hence, if the worker was not torn down in the main thread, the
	// hostarq-daemon would receive a parent death signal which would trigger
	// its early termination which in turn would disrupt regular shutdown of
	// sctrltp::ARQStream (in hxcomm::ARQConnection).
	wtr_t::m_worker.teardown();
	wtr_t::m_running = false;
	RCF_LOG_TRACE(wtr_t::m_log, "main_thread() shut down.");
}

template <typename W>
bool WorkerThreadReinit<W>::is_different(session_id_t const& session_id)
{
	return !m_current_session_id || session_id != *m_current_session_id;
}

template <typename W>
bool WorkerThreadReinit<W>::ensure_session_via_reinit(work_package_t const& pkg)
{
	if (!m_current_session_id || pkg.session_id != *m_current_session_id) {
		auto log_trace = [this, &pkg]([[maybe_unused]] auto& session_id) {
			RCF_LOG_TRACE(
			    wtr_t::m_log, "Switching session from " << session_id << " to " << pkg.user_id
			                                            << "@" << pkg.session_id << ".");
		};
		if (m_current_session_id) {
			log_trace(*m_current_session_id);
			// request the reinit program for the old session if we have to resume
			m_session_storage.reinit_request(*m_current_session_id);
		} else {
			log_trace("no active session");
		}
		m_current_session_id = pkg.session_id;
	}

	if (m_session_storage.reinit_is_needed(*m_current_session_id)) {
		if (!perform_reinit()) {
			// reinit failed, clear session
			RCF_LOG_TRACE(wtr_t::m_log, "Resetting current session.");
			m_current_session_id = std::nullopt;
			return false;
		} else {
			return true;
		}
	} else {
		if (m_current_session_id) {
			RCF_LOG_TRACE(wtr_t::m_log, "No reinit needed for session " << *m_current_session_id);
		}
		return true; // switch successful
	}
}

template <typename W>
void WorkerThreadReinit<W>::requeue_work_package(work_package_t&& pkg)
{
	using namespace std::chrono_literals;
	RCF_LOG_TRACE(wtr_t::m_log, "[" << pkg.session_id << "] Requeueing #" << *(pkg.sequence_num));
	wtr_t::m_input.advance_user();
	std::jthread(
	    [this](work_package_t&& pkg) {
		    RCF_LOG_TRACE(
		        wtr_t::m_log, "[" << pkg.session_id << "] Requeueing #" << *(pkg.sequence_num));
		    wtr_t::m_input.add_work(
		        std::move(pkg), m_session_storage.get_heap_sorter_most_completed());
		    wtr_t::notify();
	    },
	    std::move(pkg))
	    .detach();
}

template <typename W>
bool WorkerThreadReinit<W>::needs_delay(work_package_t const& pkg)
{
	if (pkg.sequence_num.is_out_of_order()) {
		RCF_LOG_TRACE(wtr_t::m_log, "Package can be executed out-of-order -> no delay needed.");
		return false;
	}

	auto current = m_session_storage.sequence_num_get(pkg.session_id);

	// If the sequene number is larger than the current this package needs to be delayed.
	if (pkg.sequence_num > current) {
		RCF_LOG_TRACE(
		    wtr_t::m_log, "Sequence number is " << pkg.sequence_num << " but session "
		                                        << pkg.session_id << " currently is at " << current
		                                        << " -> delay.");
		return true;
	} else {
		return false;
	}
}

template <typename W>
bool WorkerThreadReinit<W>::check_invalidity(work_package_t& pkg)
{
	if (!m_session_storage.is_active(pkg.session_id)) {
		// inactive session despite work package present indicates a crashed client -> discard
		RCF_LOG_WARN(
		    wtr_t::m_log, "Session " << pkg.session_id << " inactive, discarding work package "
		                             << pkg.sequence_num);
		return true;
	}

	if (pkg.sequence_num.is_out_of_order()) {
		// No sequence number means out-of-order execution
		RCF_LOG_TRACE(wtr_t::m_log, "Work package marked for out-of-order execution -> valid.");
		return false;
	}

	auto current = m_session_storage.sequence_num_get(pkg.session_id);

	if (pkg.sequence_num < current) {
		auto const exception = InvalidSequenceNumber(pkg.sequence_num, current);
		RCF_LOG_TRACE(wtr_t::m_log, "Session: " << pkg.session_id << " " << exception.what());
		pkg.context.commit(exception);
		return true;
	} else {
		return false;
	}
}

template <typename W>
bool WorkerThreadReinit<W>::perform_reinit()
{
	// check if we need to perform reinit for the new session (i.e. it has a reinit program
	// requested)
	// Active wait in case we have no other work left (current use case: Synchronous PyNN)
	auto reinit_data = m_session_storage.reinit_get(
	    *m_current_session_id, wtr_t::m_input.is_empty() ? std::make_optional(20ms) : std::nullopt);
	if (reinit_data) {
		RCF_LOG_TRACE(wtr_t::m_log, "Performing reinit..");
		wtr_t::m_worker.perform_reinit(*reinit_data);
		return true;
	} else {
		RCF_LOG_WARN(
		    wtr_t::m_log, "Reinit data needed but not available for session: "
		                      << *m_current_session_id << ". Delaying execution..");
		return false;
	}
}

template <typename W>
void WorkerThreadReinit<W>::perform_teardown()
{
	RCF_LOG_TRACE(wtr_t::m_log, "Performing teardown.");
	base_t::perform_teardown();

	RCF_LOG_TRACE(
	    wtr_t::m_log, "Teardown performed, requesting potential reinit for current session.");
	if (m_current_session_id) {
		// request reinit for the current session
		m_session_storage.reinit_request(*m_current_session_id);
	}
}

} // namespace rcf_extensions::detail::round_robin_scheduler
