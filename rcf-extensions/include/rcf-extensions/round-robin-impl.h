#pragma once

template <typename W, typename R>
RoundRobinScheduler<W, R>::RoundRobinScheduler(
    RCF::TcpEndpoint const& endpoint,
    worker_t&& worker,
    size_t num_threads_pre,
    size_t num_threads_post)
    : m_worker(std::move(worker)),
      m_worker_is_set_up(false),
      m_worker_last_release(std::chrono::system_clock::time_point::min()),
      m_worker_last_idle(std::chrono::system_clock::now()),
      m_teardown_period(0)
{
	m_stop_flag = false; // no other threads running

	RCF::init();

	m_server.reset(new RCF::RcfServer(endpoint));
	m_server->bind<rcf_interface_t>(*this);

	// Thread pool with fixed number of threads
	RCF::ThreadPoolPtr tpPtr(new RCF::ThreadPool(num_threads_pre));
	m_server->setThreadPool(tpPtr);

	// set up worker
	m_worker_thread.reset(
	    new RCF::Thread(boost::bind(&RoundRobinScheduler<W, R>::worker_main_thread, this)));

	// set up output threads
	m_threads_output.resize(num_threads_post);
	for (auto& t : m_threads_output) {
		t.reset(new RCF::Thread(boost::bind(&RoundRobinScheduler<W, R>::output_main_thread, this)));
	}
}

template <typename W, typename R>
RoundRobinScheduler<W, R>::~RoundRobinScheduler()
{
	if (!m_stop_flag) {
		shutdown();
	}
}

template <typename W, typename R>
void RoundRobinScheduler<W, R>::shutdown()
{
	auto log = log4cxx::Logger::getLogger(__func__);
	LOG4CXX_DEBUG(log, "Preparing to shut down!");
	m_stop_flag = true;
	LOG4CXX_DEBUG(log, "Notifying worker..");
	notify_worker();
	LOG4CXX_DEBUG(log, "Notifying output threads..");
	notify_output_all();
	LOG4CXX_DEBUG(log, "Joining worker thread..");
	m_worker_thread->join();
	{
		// tear worker down to be sure
		LOG4CXX_DEBUG(log, "Tearing down worker.");
		RCF::Lock input_lock(m_mutex_input_queue);
		m_worker.teardown();
		LOG4CXX_DEBUG(log, "Teardown finished");
	}
	LOG4CXX_DEBUG(log, "Joining output threads");
	for (auto& thread : m_threads_output) {
		thread->join();
	}
	LOG4CXX_DEBUG(log, "Notifying timeout thread");
	{
		RCF::Mutex mutex;
		RCF::Lock lock(mutex);
		m_cond_timeout.notify_all(lock);
	}

	LOG4CXX_DEBUG(log, "Resetting server");
	// destruct server prior to deinit
	m_server.reset();
	LOG4CXX_DEBUG(log, "RCF::deinit");
	RCF::deinit();
	LOG4CXX_DEBUG(log, "Shutdown finished");
}

template <typename W, typename R>
void RoundRobinScheduler<W, R>::start_server(std::chrono::seconds const& timeout)
{
	m_timeout = timeout;
	m_server->start();

	server_idle_timeout();
}


template <typename W, typename R>
void RoundRobinScheduler<W, R>::server_idle_timeout()
{
	using namespace std::chrono_literals;

	m_worker_last_idle = std::chrono::system_clock::now();

	RCF::Mutex mutex_timeout;
	RCF::Lock lock_timeout(mutex_timeout);
	bool timeout_reached;

	// set idle timeout routine
	do {
		if (m_timeout > 0s) {
			std::chrono::milliseconds duration_till_timeout;
			{
				// m_worker_last_idle protected by input queue mutex
				RCF::Lock input_lock(m_mutex_input_queue);
				if (m_worker_is_set_up) {
					duration_till_timeout = get_time_till_next_teardown();
				} else {
					duration_till_timeout = get_time_till_timeout();
				}
			}

			// sleep at least a second before checking for timeout again
			auto duration_sleep_ms = duration_till_timeout.count() + 1;
			m_cond_timeout.timed_wait(
			    lock_timeout, std::max(duration_sleep_ms, (decltype(duration_sleep_ms)) 1000));

			{
				RCF::Lock input_lock(m_mutex_input_queue);
				timeout_reached = !m_worker_is_set_up && (std::chrono::system_clock::now() -
				                                          m_worker_last_idle) > m_timeout;
			}
		} else {
			// there is no time-out - we just wait forever (i.e. till the user aborts)
			m_cond_timeout.wait(lock_timeout);
			// will never be reached because we wait for user input to abort
			timeout_reached = true;
		}
	} while (!timeout_reached && !m_stop_flag);

	{
		RCF::Lock input_lock(m_mutex_input_queue);
		m_stop_flag = true;
	}
	notify_worker();
	notify_output_all();
}

template <typename W, typename R>
typename RoundRobinScheduler<W, R>::work_return_t RoundRobinScheduler<W, R>::submit_work(
    work_argument_t work)
{
	{
		std::ignore = work; // captured in session object

		std::string user_data = RCF::getCurrentRcfSession().getRequestUserData();
		work_context_t context(RCF::getCurrentRcfSession());

		std::optional<user_id_t> user_id = m_worker.verify_user(user_data);

		if (!user_id) {
			context.commit(UserNotAuthorized());
			// dummy data not passed to client
			return RoundRobinScheduler<W, R>::work_return_t();
		}

		RCF::Lock lock_input_queue(m_mutex_input_queue);

		// check job count for user
		auto& backlog = m_user_to_input_queue[*user_id];
		if (backlog.size() == 0) {
			// no jobs for current user -> register user
			m_users.push_back(*user_id);
		}

		// select current user if we had no work before
		if (m_users.size() == 1) {
			m_it_current_user = m_users.cbegin();
		}

		// submit the job
		backlog.push_back(context);
	}
	// notify the worker thread of work
	notify_worker();

	return RoundRobinScheduler<W, R>::work_return_t(); // not passed to client
}

template <typename W, typename R>
void RoundRobinScheduler<W, R>::notify_worker()
{
	RCF::Lock lock(m_mutex_notify_worker);
	m_cond_worker.signal_one(lock);
}

template <typename W, typename R>
void RoundRobinScheduler<W, R>::notify_output()
{
	RCF::Lock lock(m_mutex_notify_output_queue);
	m_cond_output_queue.signal_one(lock);
}

template <typename W, typename R>
void RoundRobinScheduler<W, R>::notify_output_all()
{
	RCF::Lock lock(m_mutex_notify_output_queue);
	m_cond_output_queue.signal_all(lock);
}

template <typename W, typename R>
void RoundRobinScheduler<W, R>::worker_main_thread()
{
	RCF::Lock lock_input_queue(m_mutex_input_queue);
	while (true) {
		// teardown needs to be done periodically
		if (m_worker_is_set_up && is_teardown_needed(lock_input_queue)) {
			m_worker.teardown();
			m_worker_is_set_up = false;
		}

		// sleep worker if there is no work
		if (!is_work_left(lock_input_queue)) {
			if (m_worker_is_set_up) {
				// worker is still set up so we can only sleep until the next release
				m_cond_worker.timed_wait(lock_input_queue, get_time_till_next_teardown().count());
			} else {
				// note that the worker went idle without any resources
				m_worker_last_idle = std::chrono::system_clock::now();
				// no work to be done -> sleep until needed
				m_cond_worker.wait(lock_input_queue);
			}
		}
		if (m_stop_flag) {
			break;
		}
		if (m_users.size() == 0) {
			// no work to do -> do not advance
			continue;
		}
		if (!m_worker_is_set_up) {
			// worker is starting up -> perform setup
			m_worker.setup();
			m_worker_is_set_up = true;
			m_worker_last_release = std::chrono::system_clock::now();
		}
		work_context_t context = worker_retrieve_work(lock_input_queue);

		lock_input_queue.unlock();

		work_argument_t work = context.parameters().a1.get();
		work_return_t retval = m_worker.work(work);
		context.parameters().r.set(retval);

		push_to_output_queue(std::move(context));

		lock_input_queue.lock();
	}
}

template <typename W, typename R>
typename RoundRobinScheduler<W, R>::work_context_t RoundRobinScheduler<W, R>::worker_retrieve_work(
    RCF::Lock const& lock_input_queue)
{
	// lock argument is merely to indicate, that this method should only be
	// called when the input queue is locked
	BOOST_ASSERT(lock_input_queue.locked());

	// there should always be work to retrieve, because otherwise we do not
	// leave the while loop in worker_main_thread()
	BOOST_ASSERT(is_work_left(lock_input_queue));

	// retrieve next job for current user
	work_context_t context = m_user_to_input_queue[*m_it_current_user].front();
	m_user_to_input_queue[*m_it_current_user].pop_front();

	{
		// advance to next user but check if work queue for current user is empty
		users_citer_t it_old_user = m_it_current_user++;
		if (m_user_to_input_queue[*it_old_user].size() == 0) {
			m_users.remove(*it_old_user); // invalidates it_old_user
		}
	}
	// wrap user iterator
	if (m_it_current_user == m_users.cend()) {
		m_it_current_user = m_users.cbegin();
	}

	return context;
}

template <typename W, typename R>
void RoundRobinScheduler<W, R>::output_main_thread()
{
	RCF::Lock lock_output_queue(m_mutex_output_queue);
	while (true) {
		while ((m_output_queue.size() == 0) && !m_stop_flag) {
			m_cond_output_queue.wait(lock_output_queue);
		}
		if (m_stop_flag) {
			break;
		}
		// wrap in brackets to make sure that context gets deleted after we
		// have committed
		{
			// retrieve the oldest output to deliver
			work_context_t context = m_output_queue.front();
			m_output_queue.pop_front();
			lock_output_queue.unlock();

			// send the result to the caller
			context.commit();

			lock_output_queue.lock();
		}
	}
}

template <typename W, typename R>
void RoundRobinScheduler<W, R>::push_to_output_queue(work_context_t&& work)
{
	RCF::Lock lock(m_mutex_output_queue);
	m_output_queue.push_back(std::move(work));
	notify_output();
}

template <typename W, typename R>
bool RoundRobinScheduler<W, R>::is_teardown_needed(RCF::Lock const& lock_input_queue)
{
	using namespace std::chrono_literals;

	// lock argument is merely to indicate, that this method should only be
	// called when the input queue is locked
	BOOST_ASSERT(lock_input_queue.locked());

	// there are two conditions for tearing down the worker
	// 1. we have reached the end of the current period
	// 2. if there is no period we have to tear down the worker whenever there is no work
	if (m_teardown_period > 0s &&
	    m_worker_last_release > std::chrono::system_clock::time_point::min()) {
		auto now = std::chrono::system_clock::now();
		return (now - m_worker_last_release) >= m_teardown_period;
	} else {
		return !is_work_left(lock_input_queue);
	}
}

template <typename W, typename R>
std::chrono::milliseconds RoundRobinScheduler<W, R>::get_time_till_next_teardown()
{
	auto now = std::chrono::system_clock::now();
	return m_teardown_period -
	       std::chrono::duration_cast<std::chrono::milliseconds>(now - m_worker_last_release);
}

template <typename W, typename R>
std::chrono::milliseconds RoundRobinScheduler<W, R>::get_time_till_timeout()
{
	auto now = std::chrono::system_clock::now();
	return m_timeout -
	       std::chrono::duration_cast<std::chrono::milliseconds>(now - m_worker_last_idle);
}

template <typename W, typename R>
bool RoundRobinScheduler<W, R>::is_work_left(RCF::Lock const& lock_input_queue)
{
	BOOST_ASSERT(lock_input_queue.locked());
	return m_users.size() > 0;
}

template <typename W, typename R>
void RoundRobinScheduler<W, R>::reset_idle_timeout()
{
	m_worker_last_idle = std::chrono::system_clock::now();
}
