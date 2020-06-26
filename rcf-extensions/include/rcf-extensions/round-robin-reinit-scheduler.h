#pragma once

#include <chrono>
#include <deque>
#include <iostream>
#include <list>
#include <map>
#include <memory>
#include <optional>
#include <utility>
#include <vector>
#include <boost/assert.hpp>
#include <RCF/RCF.hpp>

#include "rcf-extensions/common.h"
#include "rcf-extensions/detail/round-robin-scheduler/idle-timeout.h"
#include "rcf-extensions/detail/round-robin-scheduler/input-queue.h"
#include "rcf-extensions/detail/round-robin-scheduler/output-queue.h"
#include "rcf-extensions/detail/round-robin-scheduler/work-methods.h"
#include "rcf-extensions/detail/round-robin-scheduler/worker-thread-reinit.h"
#include "rcf-extensions/on-demand-upload.h"
#include "rcf-extensions/sequence-number.h"

/*
 * Wrap a worker-object in a RCF-server that uses round-robin scheduling to do
 * work of several producers.
 *
 * The worker has to satisfy the following interface:
 * ```
 * class MyWorker
 * {
 *  // Function executed when the server is about to begin execution
 *  // (should acquire all needed resources).
 *  void setup();
 *
 *  // Verify that the given string authorizes a given user (e.g. by treating
 *  // the string as a munge token and decoding it).
 *  //
 *  // The supplied parameter is the extracted user_data attribute from the
 *  // RCF-call.
 *  //
 *  // The return value is an optional of a CopyAssignable and
 *  // CopyConstructable UserIdentifierT-type.
 *  // If the optional does not contain a value the user is deemed not
 *  // authorized and the work will not be executed. Otherwise, the contained
 *  // UserIdentifierT-value will be used to assign the given work unit to a
 *  // user.
 *  //
 *  // This version needs to be thread-safe!
 *  std::optional<UserIdentifierT> verify_user(std::string const&);
 *
 *  // Function that does the acutal work. Both the supplied parameter and the
 *  // return value can be any type.
 *  // The return type has to be default-constructable!
 *  MyReturnType work(MyWorkParameters const& work);
 *
 * // Function that
 *  void perform_reinit(ReinitData const& reinit);
 *
 *  // function exectuted when the server is about to go idle
 *  // (should release all acuqired resources)
 *  void teardown();
 * };
 * ```

 * Using the define `RR_GENERATE(MyWorker, MyAlias)` below, we can generate a
 * RCFServer instance wrapping the worker-type and implementing the
 * `I_MyAlias`-RCF-interface.
 *
 * Clients can then execute work via:
 *
 * ```
 * MyWorkParameters parameters;
 * // (..) setup parameters
 *
 * RcfClient< I_MyAlias > client;
 * MyReturnType ret = client.submit_work(parameters);
 * ```
 * The work to be done should be completely defined in the
 * MyWorkParameters-type.
 *
 * Authentification can be achieved via specifying:
 * ```
 * std::string my_user_data = "HERE BE AUTH";
 * client.getClientStub().setRequestUserData(my_user_data);
 * ```
 * prior to calling submit_work. From this user-data a unique user identity is
 * derived on the server-side in order to achieve round-robin balancing.
 *
 * A fully working example can be found under `playground/round-robin-scheduler`.
 */

using namespace std::literals::chrono_literals;

namespace rcf_extensions {

template <typename Worker>
class RoundRobinReinitScheduler
{
public:
	using worker_t = Worker;
	using work_methods = detail::round_robin_scheduler::work_methods<Worker>;

	static_assert(
	    work_methods::reinit_detected::value, "Worker type is missing method to perform reinit!");

	// inference done in extra helper struct for RR_GENERATE macro
	using work_argument_t = typename work_methods::work_argument_t;
	using work_return_t = typename work_methods::work_return_t;
	using work_context_t = typename work_methods::work_context_t;
	using work_package_t = typename work_methods::work_package_t;

	using reinit_data_t = typename work_methods::reinit_data_t;

	RoundRobinReinitScheduler() = delete;
	RoundRobinReinitScheduler(const RoundRobinReinitScheduler&) = delete;
	RoundRobinReinitScheduler(RoundRobinReinitScheduler&&) = delete;

	RoundRobinReinitScheduler(
	    RCF::TcpEndpoint const& endpoint,
	    worker_t&& worker,
	    std::size_t num_threads_pre = 1,
	    std::size_t num_threads_post = 1,
	    std::size_t num_max_connections = 1 << 16);
	~RoundRobinReinitScheduler();

	/**
	 * Bind scheduler to rcf interface.
	 *
	 * Needs to be called after creating scheduler. _construct-helper takes
	 * care of it.
	 */
	template <typename RcfInterface>
	void bind_to_interface()
	{
		m_server->bind<RcfInterface>(*this);
	}

	/**
	 * Notify server about new reinit program.
	 */
	bool reinit_notify();

	/**
	 * Upload new reinit program.
	 */
	void reinit_upload(reinit_data_t);

	/**
	 * Start server and shut down server after a given timeout of being idle.
	 *
	 * @return Whether or not the server shutdown due to idle timeout. If false
	 * the server is already in the process of shutting down.
	 */
	bool start_server(std::chrono::seconds const& timeout = 0s);

	/**
	 * Indicate whether scheduler has work left.
	 */
	bool has_work_left() const
	{
		return !m_input_queue->is_empty();
	}

	RCF::RcfServer& get_server()
	{
		return *m_server;
	}

	/**
	 * Submit work to be executed on the worker.
	 *
	 * If several units of work get submitted and control of the worker is
	 * relinquished in-between runs, the scheduler checks for and applies any
	 * provided reinit data, if it is available.
	 *
	 * @param work_argument_t The work to be performed.
	 * @param sequence_num The sequence number of the given work. Can be zero
	 * if the order of execution doe not matter or couting upwards from 1. In
	 * the latter case the units of work will be executed in ascending order.
	 * Failing to supply a number will cause the scheduler to stall whereas
	 * supplying a number twice will lead to an exception.
	 *
	 * @param work_argument_t The work to be performed.
	 *
	 * @param sequence_num The sequence number of the given work (or
	 * SequenceNumber::out_of_order() to indicate that sequential order does
	 * not matter).
	 * In case of in-order execution, failing to supply a number in the
	 * sequence will cause the scheduler to stall whereas supplying a number
	 * twice will lead to an exception.
	 *
	 * @param enforce_reinit If set to true the scheduler will not perfrom work
	 * for the current session until a reinit program is available. This also
	 * means that the reinit program will run for the first work unit in the
	 * sequence.
	 *
	 * @return Return value of the worker after work unit was completed.
	 */
	work_return_t submit_work(work_argument_t, SequenceNumber sequence_num, bool enforce_reinit);

	/**
	 * Set interval after which the the worker has to be teared down at least once.
	 */
	void set_release_interval(std::chrono::seconds const& s)
	{
		m_worker_thread->set_release_interval(s);
	}

	/**
	 * Manually reset idle timeout.
	 */
	void reset_idle_timeout()
	{
		m_worker_thread->reset_last_idle();
	}

	/**
	 * Set the time period after which the current user is forcibly switched.
	 *
	 * @param period Time after which the user is forcibly switched.
	 */
	void set_period_per_user(std::chrono::milliseconds period)
	{
		m_input_queue->set_period_per_user(period);
	}

	/**
	 * Get the time period after which the user is forcibly switched even if
	 * there are jobs remaining for the current user.
	 */
	std::chrono::milliseconds get_period_per_user() const
	{
		return m_input_queue->get_period_per_user();
	}

protected:
	/**
	 * Apply const visitor to worker object.
	 *
	 * This is useful to extend the RCF-interface if there are some read-only
	 * operations to be facilitated on the worker.
	 */
	template <typename VisitorT>
	auto visit_worker_const(VisitorT visit) const
	{
		return m_worker_thread->visit_const(std::forward<VisitorT>(visit));
	}

	/**
	 * RcfServer instance that can be custom-bound to extended RCF-interfaces
	 * in derived classes.
	 */
	std::unique_ptr<RCF::RcfServer> m_server;

private:
	log4cxx::Logger* m_log;

	using input_queue_t = detail::round_robin_scheduler::InputQueue<worker_t>;
	std::unique_ptr<input_queue_t> m_input_queue;

	using output_queue_t = detail::round_robin_scheduler::OutputQueue<worker_t>;
	std::unique_ptr<output_queue_t> m_output_queue;

	using session_storage_t = detail::round_robin_scheduler::SessionStorage<worker_t>;
	std::unique_ptr<session_storage_t> m_session_storage;

	using worker_thread_t = detail::round_robin_scheduler::WorkerThreadReinit<worker_t>;
	std::unique_ptr<worker_thread_t> m_worker_thread;

	using idle_timeout_t = detail::round_robin_scheduler::IdleTimeout<worker_thread_t>;
	std::unique_ptr<idle_timeout_t> m_idle_timeout;

	void handle_submission(SequenceNumber, bool);
};

} // namespace rcf_extensions

#include "rcf-extensions/round-robin-reinit-scheduler.tcc"


/**
 * The only symbols that should be used externally:
 * Given a worker-type and and a desired alias for the RCF-server-wrapper, the
 * correct rcf-interface will be instantiated under the specified alias (with
 * an "I_"-prefix).
 *
 * The RRWR_GENERATE (RoundRobin With Reinit-)macro also creates a
 * construction helper `<specified-alias>_construct` that automatically
 * forwards the correct RCF-Interface to the constructor. Due to the fact that
 * the scheduler uses threads internally synchronised via std::mutexes, it is
 * inherently not move-able and hence has to be constructed on the heap.
 *
 * Furthermore, `<specified-alias>_reinit_uploader_t` (along with its
 * `<specified-alias>_construct reinit_uploader`-helper) is created.
 *
 * If more control is needed the RCF interface can be created from the worker
 * definition with RRWR_GENERATE_INTERFACE, whereas the utility funcionality is
 * created via RRWR_GENERATE_UTILITIES. Please note that in order to allow for
 * more flexibility, the interface generator-macros only generate the required
 * part of the scheduler to work. The interface can be extended in user-code
 * and then _has_ to be closed via RCF_END(<interface-name>)!
 */

#define RRWR_GENERATE_INTERFACE(WORKER_TYPE, RCF_INTERFACE)                                        \
	RRWR_GENERATE_INTERFACE_EXPLICIT_TYPES(                                                        \
	    RCF_INTERFACE,                                                                             \
	    typename rcf_extensions::detail::round_robin_scheduler::work_methods<                      \
	        WORKER_TYPE>::work_return_t,                                                           \
	    typename rcf_extensions::detail::round_robin_scheduler::work_methods<                      \
	        WORKER_TYPE>::work_argument_t,                                                         \
	    typename rcf_extensions::detail::round_robin_scheduler::work_methods<                      \
	        WORKER_TYPE>::reinit_data_t)

#define RRWR_GENERATE_INTERFACE_EXPLICIT_TYPES(                                                    \
    RCF_INTERFACE, WORK_RETURN_TYPE, WORK_ARGUMENT_TYPE, REINIT_DATA_TYPE)                         \
	RCF_BEGIN(RCF_INTERFACE, #RCF_INTERFACE)                                                       \
	RCF_METHOD_R3(                                                                                 \
	    WORK_RETURN_TYPE, submit_work, WORK_ARGUMENT_TYPE, ::rcf_extensions::SequenceNumber, bool) \
	RCF_METHOD_R0(bool, reinit_notify)                                                             \
	RCF_METHOD_V1(void, reinit_upload, REINIT_DATA_TYPE)


#define RRWR_GENERATE_UTILITIES(WORKER_TYPE, ALIAS_SCHEDULER, RCF_INTERFACE)                       \
	using ALIAS_SCHEDULER##_t = rcf_extensions::RoundRobinReinitScheduler<WORKER_TYPE>;            \
	using ALIAS_SCHEDULER##_client_t = RcfClient<RCF_INTERFACE>;                                   \
	using ALIAS_SCHEDULER##_rcf_interface_t = RCF_INTERFACE;                                       \
                                                                                                   \
	template <typename... Args>                                                                    \
	std::unique_ptr<ALIAS_SCHEDULER##_t> ALIAS_SCHEDULER##_construct(Args&&... args)               \
	{                                                                                              \
		auto scheduler = std::make_unique<ALIAS_SCHEDULER##_t>(std::forward<Args>(args)...);       \
		scheduler->template bind_to_interface<ALIAS_SCHEDULER##_rcf_interface_t>();                \
		return scheduler;                                                                          \
	}                                                                                              \
                                                                                                   \
	using ALIAS_SCHEDULER##_reinit_uploader_t = rcf_extensions::OnDemandUpload<                    \
	    ALIAS_SCHEDULER##_client_t, typename rcf_extensions::detail::round_robin_scheduler::       \
	                                    work_methods<WORKER_TYPE>::reinit_data_t>;                 \
                                                                                                   \
	inline ALIAS_SCHEDULER##_reinit_uploader_t ALIAS_SCHEDULER##_construct_reinit_uploader(        \
	    typename ALIAS_SCHEDULER##_reinit_uploader_t::f_create_client_shared_ptr_t&& func_create)  \
	{                                                                                              \
		return ALIAS_SCHEDULER##_reinit_uploader_t(                                                \
		    std::move(func_create), &ALIAS_SCHEDULER##_client_t::reinit_notify,                    \
		    &ALIAS_SCHEDULER##_client_t::reinit_upload);                                           \
	}

#define RRWR_GENERATE(WORKER_TYPE, ALIAS_SCHEDULER)                                                \
	RRWR_GENERATE_INTERFACE(WORKER_TYPE, I_##ALIAS_SCHEDULER)                                      \
	RCF_END(I_##ALIAS_SCHEDULER)                                                                   \
	RRWR_GENERATE_UTILITIES(WORKER_TYPE, ALIAS_SCHEDULER, I_##ALIAS_SCHEDULER)
