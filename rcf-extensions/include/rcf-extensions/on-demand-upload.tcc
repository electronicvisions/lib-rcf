
#include "rcf-extensions/on-demand-upload.h"

#include <exception>

namespace rcf_extensions {

template <typename RcfClientT, typename UploadDataT>
OnDemandUpload<RcfClientT, UploadDataT>::OnDemandUpload(
    f_create_client_shared_ptr_t&& func_create,
    f_notify_upload func_notify,
    f_perform_upload func_upload) :
    m_log(log4cxx::Logger::getLogger("OnDemandUpload")),
    m_f_create_client(std::move(func_create)),
    m_f_notify_upload(func_notify),
    m_f_perform_upload(func_upload)
{
	RCF::init();
}

template <typename RcfClientT, typename UploadDataT>
OnDemandUpload<RcfClientT, UploadDataT>::~OnDemandUpload()
{
	abort();
	RCF::deinit();
}

template <typename RcfClientT, typename UploadDataT>
void OnDemandUpload<RcfClientT, UploadDataT>::upload(upload_data_t&& data)
{
	upload(std::make_shared<upload_data_t>(std::move(data)));
}

template <typename RcfClientT, typename UploadDataT>
void OnDemandUpload<RcfClientT, UploadDataT>::upload(upload_data_t const& data)
{
	upload(std::make_shared<upload_data_t>(data));
}

template <typename RcfClientT, typename UploadDataT>
void OnDemandUpload<RcfClientT, UploadDataT>::wait()
{
	std::unique_lock<std::mutex> lk(m_mutex_is_uploaded);
	if (!m_is_uploaded) {
		m_cv_notify_is_uploaded.wait(lk, [&] { return m_is_uploaded; });
	}
}

template <typename RcfClientT, typename UploadDataT>
bool OnDemandUpload<RcfClientT, UploadDataT>::holds_data()
{
	return static_cast<bool>(m_request);
}

template <typename RcfClientT, typename UploadDataT>
void OnDemandUpload<RcfClientT, UploadDataT>::ensure_connected(
    std::chrono::seconds wait_interval_max)
{
	using namespace std::chrono_literals;
	auto wait = 1ms;

	while (!is_connected()) {
		RCF_LOG_TRACE(m_log, "Waiting for connection..");
		std::this_thread::sleep_for(wait);
		wait *= 2;
		if (wait > wait_interval_max) {
			wait = wait_interval_max;
		}
	}
}

template <typename RcfClientT, typename UploadDataT>
bool OnDemandUpload<RcfClientT, UploadDataT>::is_connected()
{
	return m_client && m_client->getClientStub().isConnected();
}

template <typename RcfClientT, typename UploadDataT>
void OnDemandUpload<RcfClientT, UploadDataT>::abort()
{
	m_request.reset();
	if (m_client) {
		auto& client = m_client->getClientStub();
		if (client.isConnected()) {
			client.disconnect();
		}
		m_client.reset();
	}
}

template <typename RcfClientT, typename UploadDataT>
void OnDemandUpload<RcfClientT, UploadDataT>::update_function_create_client(
    f_create_client_shared_ptr_t&& func)
{
	m_f_create_client = func;
}

template <typename RcfClientT, typename UploadDataT>
void OnDemandUpload<RcfClientT, UploadDataT>::upload(
    upload_data_shared_ptr_t const& upload_data_ptr)
{
	prepare_new_upload(upload_data_ptr);

	m_request = std::make_unique<RCF::Future<bool>>();

	// create copies of RCF objects to give to thread (sharing them by
	// reference causes double free errors)
	auto client = m_client;
	auto upload_data = m_upload_data;
	auto f_perform_upload = m_f_perform_upload;

	auto on_request = [this, request(*m_request), upload_data, client, f_perform_upload]() mutable {
		RCF_LOG_TRACE(m_log, "Notification thread returned.");
		std::unique_ptr<RCF::Exception> exception_ptr = request.getAsyncException();
		if (exception_ptr) {
			RCF_LOG_ERROR(m_log, "Server returned exception from notify-call.");
			exception_ptr->throwSelf();
		} else if (*request) {
			RCF_LOG_TRACE(m_log, "Commencing upload.");
			std::invoke(f_perform_upload, *client, *upload_data);
			{
				std::lock_guard<std::mutex> lk(m_mutex_is_uploaded);
				m_is_uploaded = true;
			}
			RCF_LOG_TRACE(m_log, "Upload completed, notifying main thread.");
			m_cv_notify_is_uploaded.notify_all();
		}
	};

	*m_request = std::invoke(m_f_notify_upload, *m_client, RCF::AsyncTwoway(on_request));
}

template <typename RcfClientT, typename UploadDataT>
void OnDemandUpload<RcfClientT, UploadDataT>::prepare_new_upload(
    upload_data_shared_ptr_t const& upload_data_ptr)
{
	RCF_LOG_TRACE(m_log, "Preparing new upload..");
	// disconnect possible previous upload
	if (m_client) {
		m_client->getClientStub().disconnect();
	}

	// reconnect
	m_client = m_f_create_client();

	// clear old upload data
	m_upload_data = upload_data_ptr;

	m_is_uploaded = false;
}

} // namespace rcf_extensions
