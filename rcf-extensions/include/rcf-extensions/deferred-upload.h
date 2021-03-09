#pragma once

#include <future>
#include <mutex>
#include <thread>

#include "rcf-extensions/logging.h"

#include <RCF/RCF.hpp>

namespace rcf_extensions {

/**
 * Helper class that can be created in `bool()`-RCF-functions to defer
 * execution of the call. This is useful in the server-side implementation of
 * @see OnDemandUpload to easily manage possible uploads received by clients.
 *
 * Afterwards, signal() can be called to indicate that the upload should be performed.
 * Otherwise, abort() can be called to indicate that the upload should _not_ be
 * performed.
 */
class DeferredUpload
{
public:
	using rcf_context_t = RCF::RemoteCallContext<bool>;

	DeferredUpload() :
	    m_log(log4cxx::Logger::getLogger("DeferredUpload")),
	    m_lock(m_mutex_deferred),
	    m_return_value_to_client(true),
	    m_thread_finished(false)
	{
		RCF_LOG_TRACE(m_log, "Setting up DeferredUpload");

		rcf_context_t context(RCF::getCurrentRcfSession());
		std::promise<bool> promise;
		m_future_thread_finished = promise.get_future();

		m_thread = std::thread([this, context(context), promise(std::move(promise))]() mutable {
			RCF_LOG_TRACE(m_log, "Started thread to hold asynchronous notification-call.");
			promise.set_value_at_thread_exit(true);
			std::lock_guard<std::mutex> const lk(m_mutex_deferred);
			RCF_LOG_TRACE(
			    m_log, "Resuming pending asynchronous call so that upload gets performed.");
			context.parameters().r.set(m_return_value_to_client);
			context.commit();
			RCF_LOG_TRACE(m_log, "Pending asynchronous call committed.");
		});
	}

	~DeferredUpload()
	{
		abort();
		m_thread.join();
	}

	/**
	 * Indicate whether the response has been properly communicated back to the
	 * client.
	 */
	bool is_done()
	{
		// Since we can only call .get() on the future once, we need to cache its result.
		if (m_thread_finished) {
			return true;
		} else if (m_future_thread_finished.valid()) {
			RCF_LOG_TRACE(m_log, "Pending thread finished execution, storing result.");
			m_thread_finished = m_future_thread_finished.get();
			return m_thread_finished;
		} else {
			return false;
		}
	}

	/**
	 * Request the upload to be performed.
	 */
	void request()
	{
		if (m_lock) {
			RCF_LOG_TRACE(m_log, "Requesting upload to be performed");
			m_return_value_to_client = true;
			m_lock.unlock();
		} else {
			RCF_LOG_TRACE(m_log, "No upload pending -> not requested.");
		}
	}

	/**
	 * Indicate whether the upload was requested.
	 */
	bool was_requested()
	{
		return !m_lock.owns_lock();
	}

	/**
	 * Signal the upload to NOT be performed.
	 */
	void abort()
	{
		if (m_lock) {
			m_return_value_to_client = false;
			m_lock.unlock();
		}
	}

private:
	log4cxx::Logger* m_log;

	// mutex that gets held until the deferred thread should resume
	std::mutex m_mutex_deferred;
	std::unique_lock<std::mutex> m_lock;

	bool m_return_value_to_client;

	std::thread m_thread;
	std::future<bool> m_future_thread_finished;
	bool m_thread_finished;
};

} // namespace rcf_extensions