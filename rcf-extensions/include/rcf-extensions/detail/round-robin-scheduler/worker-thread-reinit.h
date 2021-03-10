#pragma once

#include "rcf-extensions/common.h"
#include "rcf-extensions/detail/round-robin-scheduler/session-storage.h"
#include "rcf-extensions/detail/round-robin-scheduler/worker-thread.h"

#include <thread>

namespace rcf_extensions::detail::round_robin_scheduler {

/**
 * Worker thread that has exclusive access to the hardware resources and does the heavy lifting.
 *
 * Wraps the worker-object supplied from user side.
 *
 */
template <typename Worker>
class WorkerThreadReinit : public WorkerThread<Worker>
{
public:
	using base_t = WorkerThread<Worker>;

	using worker_t = typename base_t::worker_t;
	using input_queue_t = typename base_t::input_queue_t;
	using output_queue_t = typename base_t::output_queue_t;
	using session_storage_t = SessionStorage<worker_t>;

	using work_methods_t = typename base_t::work_methods_t;
	using work_package_t = typename work_methods_t::work_package_t;

	using session_id_t = typename work_methods_t::session_id_t;

	WorkerThreadReinit(
	    worker_t&& worker,
	    input_queue_t& input,
	    output_queue_t& output,
	    session_storage_t& reinit_storage);
	WorkerThreadReinit(WorkerThreadReinit const&) = delete;
	WorkerThreadReinit(WorkerThreadReinit&&) = delete;
	virtual ~WorkerThreadReinit() override = default;

protected:
	using wtr_t = WorkerThreadReinit; // helper typedef to access protected members of WorkerThread

	session_storage_t& m_session_storage;
	session_id_t m_current_session_id;

	virtual void main_thread(std::stop_token) override;

	/**
	 * Check if given session id is different from current session id.
	 */
	bool is_different(session_id_t const& session_id);

	/**
	 * Switch session to given id and ensure correct state via potential reinit.
	 *
	 * @param pkg Work package which session we should switch to.
	 *
	 * @return Indicate whether switch was successful or not. If not, the
	 * current work package should be pushed back to the input queue and the
	 * user switched.
	 */
	bool ensure_session_via_reinit(work_package_t const& pkg);

	/**
	 * Requeue the given work if it is not able to be executed right now.
	 */
	void requeue_work_package(work_package_t&& pkg);

	/**
	 * Check if the given work package needs a delay.
	 *
	 * @return true if package needs to be requeued and the user switched.
	 */
	bool needs_delay(work_package_t const& pkg);

	/**
	 * Check the validity of the work package and set the corresponding exception.
	 *
	 * @return true if package invalid, i.e., exception was set and the package
	 * should be discarded.
	 */
	bool check_invalidity(work_package_t& pkg);

	/**
	 * Perform reinit for the current session.
	 *
	 * @return true if reinit was successful.
	 */
	bool perform_reinit();

	/**
	 * Perform a worker teardown.
	 */
	void perform_teardown();
};

} // namespace rcf_extensions::detail::round_robin_scheduler

#include "rcf-extensions/detail/round-robin-scheduler/worker-thread-reinit.tcc"
