
#include "rcf-extensions/on-demand-upload.h"
#include <exception>
#include <random>

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
OnDemandUpload<RcfClientT, UploadDataT>::OnDemandUpload(
    f_create_client_shared_ptr_t&& func_create,
    f_notify_t func_notify,
    f_pending_t func_pending,
    f_upload_t func_upload) :
    m_log(log4cxx::Logger::getLogger("lib-rcf.OnDemandUpload")),
    m_f_create_client(std::move(func_create)),
    m_f_notify(func_notify),
    m_f_pending(func_pending),
    m_f_upload(func_upload)
{
	RCF::init();
}

template <typename RcfClientT, typename UploadDataT>
OnDemandUpload<RcfClientT, UploadDataT>::~OnDemandUpload()
{
	join_loop_upload();
	trim_stopped_threads(true);
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
	std::unique_lock lk{m_mutex_loop_upload};
	if (!m_is_uploaded) {
		m_cv_wait_for_finish.wait(lk, [&] { return m_is_uploaded; });
	}
}

template <typename RcfClientT, typename UploadDataT>
bool OnDemandUpload<RcfClientT, UploadDataT>::holds_data()
{
    return m_is_notified;
}

template <typename RcfClientT, typename UploadDataT>
void OnDemandUpload<RcfClientT, UploadDataT>::update_function_create_client(
    f_create_client_shared_ptr_t&& func)
{
	m_f_create_client = func;
}

template <typename RcfClientT, typename UploadDataT>
void OnDemandUpload<RcfClientT, UploadDataT>::upload(upload_data_shared_ptr_t const& upload_data)
{
	prepare_new_upload();

	m_thread_loop_upload = std::jthread([this, upload_data](std::stop_token st) {
		loop_upload(std::move(st), upload_data, m_unique_id);
	});

	RCF_LOG_TRACE(m_log, "Waiting for server to acknowledge reinit.");
	std::unique_lock lk{m_mutex_loop_upload};
	while (!m_is_notified) {
		m_cv_wait_for_finish.wait(lk, [&] { return m_is_notified; });
	}
	RCF_LOG_TRACE(m_log, "Reinit acknowledged.");
}

template <typename RcfClientT, typename UploadDataT>
void OnDemandUpload<RcfClientT, UploadDataT>::loop_upload(
    std::stop_token st, upload_data_shared_ptr_t upload_data, std::size_t unique_id)
{
	auto log = log4cxx::Logger::getLogger("lib-rcf.OnDemandUpload.loop_upload");
	client_shared_ptr_t client;
	std::size_t num_errors = 0;
	bool stop_flag = false;
	while (!(st.stop_requested() || stop_flag)) {
		RCF_LOG_TRACE(log, "New iteration..");
		try {
			RCF_LOG_TRACE(log, "Notifying..");
			client = connect(st);
			std::invoke(m_f_notify, *client, m_unique_id);
			if (st.stop_requested()) {
				stop_flag = true;
				break;
			}
			{
				RCF_LOG_TRACE(log, "Did notify..");
				std::lock_guard lk{m_mutex_loop_upload};
				// make sure that we only update if no other upload was started in the meantime
				if (m_unique_id == unique_id) {
					m_is_notified = true;
				}
			}
			m_cv_wait_for_finish.notify_all();

			RCF_LOG_TRACE(log, "Pending..");
			client = connect(st);
			bool response_perform_upload = std::invoke(m_f_pending, *client, m_unique_id);
			if (st.stop_requested()) {
				stop_flag = true;
				break;
			}
			if (response_perform_upload) {
				RCF_LOG_TRACE(log, "Commencing upload.");
				// We need to reconnect the client to refresh user data while creating.
				// hxcomm uses munge to set user data.
				client = connect(st);
				std::invoke(m_f_upload, *client, *upload_data, m_unique_id);
				if (st.stop_requested()) {
					stop_flag = true;
					break;
				}

				std::lock_guard lk{m_mutex_loop_upload};
				// make sure that we only update if no other upload was started in the meantime
				if (m_unique_id == unique_id) {
					m_is_uploaded = true;
					m_is_notified = true;
				}
				RCF_LOG_TRACE(log, "Upload completed.");
			} else {
				RCF_LOG_TRACE(log, "Upload aborted.");
				stop_flag = true;
				break;
			}
		} catch (RCF::Exception const& e) {
			if (st.stop_requested()) {
				stop_flag = true;
				break;
			}
			++num_errors;
			RCF_LOG_WARN(log, "Error while uploading: " << e.what());
			std::this_thread::sleep_for(delay_after_error);
		}
		if (num_errors >= num_errors_max) {
			RCF_LOG_ERROR(log, "Encountered " << num_errors_max << ", aborting!");
			stop_flag = true;
			break;
		}
		m_cv_wait_for_finish.notify_all();
	}
	// Note that thread is finished
	{
		std::lock_guard lk{m_mutex_safe_to_join};
		m_threads_safe_to_join.insert(std::this_thread::get_id());
	}
	RCF_LOG_TRACE(log, "Terminating.");
}

template <typename RcfClientT, typename UploadDataT>
void OnDemandUpload<RcfClientT, UploadDataT>::prepare_new_upload()
{
	RCF_LOG_TRACE(m_log, "Preparing new upload..");
	join_loop_upload();
	reset_unique_id();
	RCF_LOG_TRACE(m_log, "New reinit id: " << m_unique_id);

	m_is_uploaded = false;
	m_is_notified = false;
}

template <typename RcfClientT, typename UploadDataT>
void OnDemandUpload<RcfClientT, UploadDataT>::join_loop_upload()
{
	trim_stopped_threads(false);
	if (m_thread_loop_upload.joinable()) {
		RCF_LOG_TRACE(m_log, "Joining loop upload thread.");
		m_thread_loop_upload.request_stop();
		m_threads_stopped.push_back(std::move(m_thread_loop_upload));
		RCF_LOG_TRACE(m_log, "Loop upload thread terminating..");
	}
}

template <typename RcfClientT, typename UploadDataT>
typename OnDemandUpload<RcfClientT, UploadDataT>::client_shared_ptr_t
OnDemandUpload<RcfClientT, UploadDataT>::connect(std::stop_token st)
{
	auto client = m_f_create_client();

	auto progress_callback_interval_ms =
	    std::chrono::duration_cast<std::chrono::milliseconds>(period_client_progress_callback)
	        .count();

	auto progress_callback =
	    [st](const RCF::RemoteCallProgressInfo&, RCF::RemoteCallAction& action) {
		    if (st.stop_requested()) {
			    action = RCF::Rca_Cancel;
		    } else {
			    action = RCF::Rca_Continue;
		    }
	    };

	client->getClientStub().setRemoteCallProgressCallback(
	    progress_callback, progress_callback_interval_ms);

	return client;
}

template <typename RcfClientT, typename UploadDataT>
void OnDemandUpload<RcfClientT, UploadDataT>::reset_unique_id()
{
	std::lock_guard lk{m_mutex_loop_upload};
	// draw a single non-deterministic random number
	m_unique_id = std::random_device{}();
}

template <typename RcfClientT, typename UploadDataT>
void OnDemandUpload<RcfClientT, UploadDataT>::trim_stopped_threads(bool join_all)
{
	RCF_LOG_TRACE(m_log, "Trimming joined old threads.");
	std::for_each(
	    m_threads_stopped.begin(), m_threads_stopped.end(), [this, join_all](auto& thread) {
		    bool safe_to_join = join_all;
		    auto thread_id = thread.get_id();
		    if (!join_all) {
			    std::lock_guard lk{m_mutex_safe_to_join};
			    safe_to_join = m_threads_safe_to_join.contains(thread_id);
		    }
		    if (safe_to_join) {
			    thread.join();
			    std::lock_guard lk{m_mutex_safe_to_join};
			    m_threads_safe_to_join.erase(thread_id);
		    }
	    });

	std::erase_if(m_threads_stopped, [](auto const& thread) { return !thread.joinable(); });

	if (join_all && m_threads_stopped.size()) {
		throw std::runtime_error("Could not join all pending RCF calls.");
	}
	RCF_LOG_TRACE(
	    m_log, "Old threads trimmed. " << m_threads_stopped.size() << " old threads remaining.");
}

} // namespace rcf_extensions
