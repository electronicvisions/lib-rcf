#pragma once

#include <chrono>
#include <deque>
#include <iostream>
#include <list>
#include <map>
#include <optional>
#include <utility>
#include <vector>
#include <boost/assert.hpp>
#include <RCF/RCF.hpp>
#include <log4cxx/logger.h>

#include "rcf-extensions/common.h"
#include "rcf-extensions/detail/round-robin-scheduler/idle-timeout.h"
#include "rcf-extensions/detail/round-robin-scheduler/input-queue.h"
#include "rcf-extensions/detail/round-robin-scheduler/output-queue.h"
#include "rcf-extensions/detail/round-robin-scheduler/work-methods.h"
#include "rcf-extensions/detail/round-robin-scheduler/worker-thread.h"
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
class RoundRobinScheduler
{
public:
	using worker_t = Worker;
	using work_methods = detail::round_robin_scheduler::work_methods<Worker>;

	// inference done in extra helper struct for RR_GENERATE macro
	using work_argument_t = typename work_methods::work_argument_t;
	using work_return_t = typename work_methods::work_return_t;
	using work_context_t = typename work_methods::work_context_t;
	using work_package_t = typename work_methods::work_package_t;

	RoundRobinScheduler() = delete;
	RoundRobinScheduler(const RoundRobinScheduler&) = delete;
	RoundRobinScheduler(RoundRobinScheduler&&) = delete; // mutexes cannot be moved

	RoundRobinScheduler(
	    RCF::TcpEndpoint const& endpoint,
	    worker_t&& worker,
	    size_t num_threads_pre = 1,
	    size_t num_threads_post = 1);
	~RoundRobinScheduler();

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
	 * Start server and shut down server after a given timeout of being idle
	 */
	void start_server(std::chrono::seconds const& timeout = 0s);

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

	work_return_t submit_work(work_argument_t, SequenceNumber);

	/**
	 * Set interval after which the the worker has to be teared down at least once.
	 *
	 * @param s Release interval.
	 */
	void set_release_interval(std::chrono::seconds const& s)
	{
		m_worker_thread->set_release_interval(s);
	}

	/**
	 * Get interval after which the the worker has to be teared down at least once.
	 *
	 * @return Release interval.
	 */
	std::chrono::seconds get_release_interval() const
	{
		return m_worker_thread->get_release_interval();
	}

	/**
	 * Set time period after which the user is forcibly switched even if there
	 * are jobs remaining.
	 *
	 * @param period Time after which the user is forcibly switched. If it is
	 * 0ms the user will not be switched.
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

	/**
	 * Reset the counter governing the idle timeout.
	 */
	void reset_idle_timeout()
	{
		m_worker_thread->reset_last_idle();
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

	using input_queue_t = rcf_extensions::detail::round_robin_scheduler::InputQueue<worker_t>;
	std::unique_ptr<input_queue_t> m_input_queue;

	using output_queue_t = detail::round_robin_scheduler::OutputQueue<worker_t>;
	std::unique_ptr<output_queue_t> m_output_queue;

	using worker_thread_t = detail::round_robin_scheduler::WorkerThread<worker_t>;
	std::unique_ptr<worker_thread_t> m_worker_thread;

	using idle_timeout_t = detail::round_robin_scheduler::IdleTimeout<worker_thread_t>;
	std::unique_ptr<idle_timeout_t> m_idle_timeout;

	bool m_stop_flag;
};

} // namespace rcf_extensions

#include "rcf-extensions/round-robin-scheduler.tcc"

// The only symbol that should be used externally:
// Given a worker-type and and a desired alias for the RCF-server-wrapper, the
// correct rcf-interface will be instantiated under the specified alias (with
// an "I_"-prefix).
//
// The marcro also creates a construction helper `<specified-alias>_construct`
// that automatically forwards the correct RCF-Interface to the constructor.
//
#define RR_GENERATE(WORKER_TYPE, ALIAS_SCHEDULER)                                                  \
	RCF_BEGIN(I_##ALIAS_SCHEDULER, "I_" #ALIAS_SCHEDULER)                                          \
	RCF_METHOD_R2(                                                                                 \
	    typename rcf_extensions::detail::round_robin_scheduler::work_methods<                      \
	        WORKER_TYPE>::work_return_t,                                                           \
	    submit_work,                                                                               \
	    typename rcf_extensions::detail::round_robin_scheduler::work_methods<                      \
	        WORKER_TYPE>::work_argument_t,                                                         \
	    ::rcf_extensions::SequenceNumber)                                                          \
	RCF_END(I_##ALIAS_SCHEDULER)                                                                   \
                                                                                                   \
	using ALIAS_SCHEDULER##_t = rcf_extensions::RoundRobinScheduler<WORKER_TYPE>;                  \
	using ALIAS_SCHEDULER##_client_t = RcfClient<I_##ALIAS_SCHEDULER>;                             \
	using ALIAS_SCHEDULER##_rcf_interface_t = I_##ALIAS_SCHEDULER;                                 \
                                                                                                   \
	template <typename... Args>                                                                    \
	std::unique_ptr<ALIAS_SCHEDULER##_t> ALIAS_SCHEDULER##_construct(Args&&... args)               \
	{                                                                                              \
		auto scheduler = std::make_unique<ALIAS_SCHEDULER##_t>(std::forward<Args>(args)...);       \
		scheduler->template bind_to_interface<ALIAS_SCHEDULER##_rcf_interface_t>();                \
		return scheduler;                                                                          \
	}
