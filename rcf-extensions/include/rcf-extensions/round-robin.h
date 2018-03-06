#pragma once

#include <chrono>
#include <deque>
#include <iostream>
#include <list>
#include <map>
#include <utility>
#include <vector>
#include <boost/assert.hpp>
#include <RCF/RCF.hpp>

#include "log4cxx/logger.h"
#include "rcf-extensions/detail/round-robin.h"

/*
 * Wrap a worker-object in a RCF-server that uses round-robin scheduling to do
 * work of several producers.
 *
 * The worker has to satisfy the following interface:
 * ```
 * class MyWorker
 * {
 *  // Function exectuted when the server is about to begin execution
 *  // (should acuqire all needed resources).
 *  void setup();
 *
 *  // Verify that the given string authorizes a given user.
 *  // If false is returned, the work is not executed
 *  bool verify_user(std::string const&);
 *
 *  // Function that does the acutal work. Both the supplied parameter and the
 *  // return value can be any type.
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
 * prior to calling submit_work. This user-data is also used to identify unique
 * users in order to achieve round-robin balancing.
 *
 * A fully working example can be found under `playground/round-robin-scheduler`.
 */

namespace rcf_extensions {

class UserNotAuthorized : public std::exception
{
public:
	const char* what() const noexcept { return "User is not authorized."; }
};

template <typename Worker, typename RCFInterface>
class RoundRobinScheduler
{
public:
	typedef Worker worker_t;

	// inference done in extra helper struct for RR_GENERATE macro
	using work_t = typename detail::infer_work_method_traits<Worker>::work_t;
	using work_return_t = typename detail::infer_work_method_traits<Worker>::work_return_t;

	using rcf_interface_t = RCFInterface;

	RoundRobinScheduler(
		RCF::TcpEndpoint const& endpoint,
		worker_t&& worker,
		size_t num_threads_pre = 1,
		size_t num_threads_post = 1);
	~RoundRobinScheduler();

	// start server and shut down server after a given timeout of being idle
	void start_server(std::chrono::seconds const& timeout = 0);

	// stop the server gracefully by letting the last work unit complete
	// (this should be done as to not brick the FPGAs)
	void shutdown();

	RCF::RcfServer& get_server() { return *m_server; }
	worker_t& get_worker() { return m_worker; }

	work_return_t submit_work(work_t);

	// Set and retrieve the period after which the teardown()-method is called.
	// If there is still work left to do, setup() will be called immediately.
	void set_release_interval(std::chrono::seconds const& s) { m_teardown_period = s; }
	std::chrono::seconds get_release_interval() { return m_teardown_period; }

	// reset the counter governing the idle timeout
	void reset_idle_timeout();

private:
	typedef RCF::RemoteCallContext<work_return_t, work_t> work_context_t;

	// members
	std::unique_ptr<RCF::RcfServer> m_server;

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
	typedef std::map<std::string, std::deque<work_context_t> > user_to_inputqueue_t;
	user_to_inputqueue_t m_user_to_input_queue;

	std::deque<work_context_t> m_output_queue;
	std::vector<RCF::ThreadPtr> m_threads_output;
	RCF::Mutex m_mutex_output_queue;
	RCF::Condition m_cond_output_queue;

	typedef std::list<std::string> users_t;
	users_t m_users;
	users_t::const_iterator m_it_current_user;

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

#include "round-robin-impl.h"
}

// The only symbol that should be used externally:
// Given a worker-type and and a desired alias for the RCF-server-wrapper, the
// correct rcf-interface will be instantiated under the specified alias (with
// an "I_"-prefix).
//
#define RR_GENERATE(WORKERTYPE, DESIRED_ALIAS)                                                     \
	RCF_BEGIN(I_##DESIRED_ALIAS, "I_" #DESIRED_ALIAS)                                              \
	RCF_METHOD_R1(                                                                                 \
		typename rcf_extensions::detail::infer_work_method_traits<WORKERTYPE>::work_return_t,      \
		submit_work,                                                                               \
		typename rcf_extensions::detail::infer_work_method_traits<WORKERTYPE>::work_t)             \
	RCF_END(I_##DESIRED_ALIAS)                                                                     \
	typedef rcf_extensions::RoundRobinScheduler<WORKERTYPE, I_##DESIRED_ALIAS> DESIRED_ALIAS;
