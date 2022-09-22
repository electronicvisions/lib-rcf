#pragma once

#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <thread>
#include <unordered_set>

#include <RCF/RCF.hpp>

#include "rcf-extensions/logging.h"

namespace rcf_extensions {

/**
 * Helper class for uploading large data blobs to the server on demand, i.e.
 * when the server needs them.
 *
 * This is useful when the client has more-or-less rapidly changing data blobs
 * of which the server only needs some at specific time instances.

 * A primary example - and the motivation for this implementation - is the
 * re-initialization playback program that is needed to put the chip into a
 * known state after some other user used it via quiggeldy. The initialization
 * routine might depend on current parameters of the experiment and should not
 * be transferred for every single experiment run the user performs.
 *
 * The RCF interface has two methods:
 * * A `bool notify()`-method the client calls to let the server know there is
 *   a new version of the data to be uploaded. The server then defers the call
 *   (@see DeferredUpload) until it needs the data upon which `true` is
 *   returned (or `false` if it does not need the data anymore).
 * * An `upload(data)`-method that actually sends the data to the server.
 *
 * Data is offered to be uploaded in the loop to ensure the server side
 * receives the reinit.
 *
 * @tparam RcfClientT The `RcfClient<INTERFACE>` from the interface in use.
 * @tparam UploadDataT The data type that is being uploaded.
 *
 */
template <typename RcfClientT, typename UploadDataT>
class OnDemandUpload
{
public:
	using client_t = RcfClientT;
	using client_shared_ptr_t = std::shared_ptr<client_t>;

	using upload_data_t = UploadDataT;
	using upload_data_shared_ptr_t = std::shared_ptr<upload_data_t>;

	using f_create_client_shared_ptr_t = std::function<client_shared_ptr_t()>;
	using f_notify_t = RCF::FutureConverter<RCF::Void> (client_t::*)(std::size_t);
	using f_pending_t = RCF::FutureConverter<bool> (client_t::*)(std::size_t);
	using f_upload_t = RCF::FutureConverter<RCF::Void> (client_t::*)(UploadDataT, std::size_t);

	/**
	 * Create a new OnDemandUpload-instance by providing the required methods.
	 *
	 * @param func_create A lambda that creates a `shared_ptr` to the RcfClient
	 * in use. It should also set other required things like a
	 * `getClientStub().setRemoteCallTimeout(timeout)` that is long enough for
	 * the usage scenario in question or
	 * `getClientStub().setRequestUserData(user_data)` if needed.
	 *
	 * @param func_notify Member-method pointer of
	 * `OnDemandUpload::client_t` that performs the notification, returns immediately.
	 *
	 * @param func_pending Member-method pointer of `OnDemandUpload::client_t`
	 * that blocks on the server side until the upload is required, return
	 * value indicates if upload should be performed.
	 *
	 * @param func_upload Member-method pointer of
	 * `OnDemandUpload::client_t` that performs the data upload.
	 */
	OnDemandUpload(
	    f_create_client_shared_ptr_t&& func_create,
	    f_notify_t func_notify,
	    f_pending_t func_pending,
	    f_upload_t func_upload);

	OnDemandUpload(OnDemandUpload&&) = delete;
	OnDemandUpload(OnDemandUpload const&) = delete;

	~OnDemandUpload();

	/**
	 * Refresh a given upload, making sure that it is still available to the server.
	 */
	void refresh();

	/**
	 * Notify and - if needed - upload the given data to the server.
	 */
	void upload(upload_data_t&& data);

	/**
	 * Notify and - if needed - upload the given data to the server.
	 */
	void upload(upload_data_t const& data);

	/**
	 * Block until the uploaded data is transferred upstream.
	 */
	void wait();

	/**
	 * Check if this instance currently holds data to be uploaded or did
	 * already upload data.
	 */
	bool holds_data();

	/**
	 * Abort upload.
	 */
	void abort();

	/**
	 * Update routine used to create new clients.
	 */
	void update_function_create_client(f_create_client_shared_ptr_t&& func);

private:
	/**
	 * Notify and - if needed - upload the given data to the server.
	 *
	 * Make sure to not modify the data pointed to by the `upload_data_ptr`
	 *
	 * @param upload_data_ptr Data to upload.
	 */
	void upload(upload_data_shared_ptr_t const& upload_data_ptr);

	/**
	 * Start a new upload thread. Will use the current reinit id.
	 *
	 * @param upload_data_ptr Data to upload.
	 */
	void start_upload_thread(upload_data_shared_ptr_t const& upload_data_ptr);

	/**
	 * Check if the current upload thread is still running or if it needs to be restarted.
	 */
	bool is_upload_thread_running();

	/**
	 * Prepare for new upload with new upload id.
	 */
	void prepare_new_upload();

	/**
	 * Loop for a single upload.
	 *
	 * The upload loop has a local copy of its unique id to distinguish if it
	 * is still up-to-date. This is due to the fact that we only abort pending
	 * remote calls in RCF via the progress callback, i.e. in long intervals
	 * and not immediately.
	 *
	 * We avoid that by signalling the thread to abort via stop token and
	 * detaching it so that it can finish on its own within one callback
	 * period.
	 *
	 * However, there might be race conditions in notification/upload flags
	 * might be overwritten despite the upload not being up-to-date.
	 *
	 * Ergo, we supply each thread with its unique id so that it can check on
	 * its own.
	 */
	void loop_upload(std::stop_token st, upload_data_shared_ptr_t, std::size_t unique_id);

	void join_loop_upload();

	/**
	 * Check all stopped threads for termination.
	 *
	 * This is necessary because RCF only allows terminating running calls in
	 * fixed intervals via callback.
	 *
	 * @param join_all If true, we join all threads.
	 */
	void trim_stopped_threads(bool join_all);

	// connect a client to remote
	client_shared_ptr_t connect(std::stop_token);

	void reset_unique_id();

	RCF::RcfInit m_rcf_init;
	log4cxx::LoggerPtr m_log;

	// m_request needs to be a unique_ptr because we need to defer its default
	// construction until we have initialized RCF
	std::unique_ptr<RCF::Future<bool>> m_request;

	f_create_client_shared_ptr_t m_f_create_client;
	f_notify_t m_f_notify;
	f_pending_t m_f_pending;
	f_upload_t m_f_upload;

	std::mutex m_mutex_loop_upload;
	std::atomic_bool m_is_uploaded;
	std::atomic_bool m_is_notified;
	std::condition_variable m_cv_wait_for_finish;

	std::jthread m_thread_loop_upload;
	std::deque<std::jthread> m_threads_stopped;
	std::mutex m_mutex_safe_to_join;
	std::unordered_set<std::thread::id> m_threads_safe_to_join;

	std::size_t m_unique_id;
	upload_data_shared_ptr_t m_upload_data;

	static constexpr std::size_t num_errors_max = 10;
	// Period with which the client checks if he should terminate.
	static constexpr auto period_client_progress_callback = std::chrono::milliseconds(10);
	// Delay to wait after an error occurs
	static constexpr auto delay_after_error = std::chrono::milliseconds(1000);
};

} // namespace rcf_extensions

#include "rcf-extensions/on-demand-upload.tcc"
