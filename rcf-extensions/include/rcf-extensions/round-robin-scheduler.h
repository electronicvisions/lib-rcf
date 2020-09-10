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

#include "rcf-extensions/detail/round-robin-scheduler/work-methods.h"

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
	using user_id_t = typename work_methods::user_id_t;

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

	RCF::RcfServer& get_server()
	{
		return *m_server;
	}
	worker_t& get_worker()
	{
		return m_worker;
	}

	work_return_t submit_work(work_argument_t);

	// Set and retrieve the period after which the teardown()-method is called.
	// If there is still work left to do, setup() will be called immediately.
	void set_release_interval(std::chrono::seconds const& s)
	{
		m_teardown_period = s;
	}
	std::chrono::seconds get_release_interval()
	{
		return m_teardown_period;
	}

	// reset the counter governing the idle timeout
	void reset_idle_timeout();

private:
	typedef RCF::RemoteCallContext<work_return_t, work_argument_t> work_context_t;

	// members
	std::unique_ptr<RCF::RcfServer> m_server;

	log4cxx::Logger* m_log;

	RCF::Condition m_cond_worker;
	RCF::Condition m_cond_timeout;
	RCF::ThreadPtr m_worker_thread;
	worker_t m_worker;
	// variable indicating worker doing work or not
	bool m_worker_is_set_up;
	std::chrono::system_clock::time_point m_worker_last_release;
	std::chrono::system_clock::time_point m_worker_last_idle; // protected by m_mutex_input_queue
	std::chrono::seconds m_teardown_period;
	std::chrono::seconds m_timeout;
	RCF::Mutex m_mutex_notify_worker;
	RCF::Mutex m_mutex_notify_output_queue;

	// locked via m_mutex_input_queue
	bool m_stop_flag;

	RCF::Mutex m_mutex_input_queue;
	typedef std::map<user_id_t, std::deque<work_context_t> > user_to_inputqueue_t;
	user_to_inputqueue_t m_user_to_input_queue;

	std::deque<work_context_t> m_output_queue;
	std::vector<RCF::ThreadPtr> m_threads_output;
	RCF::Mutex m_mutex_output_queue;
	RCF::Condition m_cond_output_queue;

	using users_t = typename std::list<user_id_t>;
	using users_citer_t = typename users_t::const_iterator;
	users_t m_users;
	users_citer_t m_it_current_user;

	// methods
	void worker_main_thread();
	work_context_t worker_retrieve_work(RCF::Lock const& lock_input_queue);
	void worker_perform_work(work_context_t&);
	bool is_teardown_needed(RCF::Lock const& lock_input_queue);
	bool is_work_left(RCF::Lock const& lock_input_queue);
	std::chrono::milliseconds get_time_till_next_teardown();
	std::chrono::milliseconds get_time_till_timeout();

	void notify_worker();

	void notify_output();
	void notify_output_all();

	void server_idle_timeout();

	void output_main_thread();
	void push_to_output_queue(work_context_t&&);
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
	RCF_METHOD_R1(                                                                                 \
	    typename rcf_extensions::detail::round_robin_scheduler::work_methods<                      \
	        WORKER_TYPE>::work_return_t,                                                           \
	    submit_work,                                                                               \
	    typename rcf_extensions::detail::round_robin_scheduler::work_methods<                      \
	        WORKER_TYPE>::work_argument_t)                                                         \
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
