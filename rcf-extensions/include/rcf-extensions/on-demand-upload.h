#pragma once

#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <thread>

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
	using f_notify_upload = RCF::FutureConverter<bool> (client_t::*)(RCF::CallOptions const&);
	using f_perform_upload = RCF::FutureConverter<RCF::Void> (client_t::*)(UploadDataT);

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
	 * `OnDemandUpload::client_t` that performs the notification.
	 *
	 * @param func_upload Member-method pointer of
	 * `OnDemandUpload::client_t` that performs the data upload.
	 */
	OnDemandUpload(
	    f_create_client_shared_ptr_t&& func_create,
	    f_notify_upload func_notify,
	    f_perform_upload func_upload);

	OnDemandUpload(OnDemandUpload&&) = delete;
	OnDemandUpload(OnDemandUpload const&) = delete;

	~OnDemandUpload();

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
	 * Exponential wait for connection to be established.
	 *
	 * Once this function returns we can be sure the server was notified.
	 *
	 * @param wait_interval_max The maximum amount of time to wait between checks.
	 */
	void ensure_connected(std::chrono::seconds wait_interval_max = std::chrono::seconds(1));

	/**
	 * Check if this instance is connected to the remote site.
	 *
	 * @return If this instance is connected to the remote site.
	 */
	bool is_connected();

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
	 */
	void upload(upload_data_shared_ptr_t const& upload_data_ptr);

	void prepare_new_upload(upload_data_shared_ptr_t const& upload_data_ptr);

	log4cxx::Logger* m_log;

	client_shared_ptr_t m_client;
	std::shared_ptr<UploadDataT> m_upload_data;
	// m_request needs to be a unique_ptr because we need to defer its default
	// construction until we have initialized RCF
	std::unique_ptr<RCF::Future<bool>> m_request;

	f_create_client_shared_ptr_t m_f_create_client;
	f_notify_upload m_f_notify_upload;
	f_perform_upload m_f_perform_upload;

	std::mutex m_mutex_is_uploaded;
	bool m_is_uploaded;
	std::condition_variable m_cv_notify_is_uploaded;
};

} // namespace rcf_extensions

#include "rcf-extensions/on-demand-upload.tcc"
