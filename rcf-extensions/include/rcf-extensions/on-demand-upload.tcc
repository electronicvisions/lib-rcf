
#include "rcf-extensions/on-demand-upload.h"
#include <exception>
#include <random>

namespace rcf_extensions {

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
    m_f_upload(func_upload),
    m_is_uploaded(false),
    m_is_notified(false)
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
	{
		std::lock_guard const lk{m_mutex_loop_upload};
		m_upload_data = std::make_shared<upload_data_t>(std::move(data));
	}
	upload(m_upload_data);
}

template <typename RcfClientT, typename UploadDataT>
void OnDemandUpload<RcfClientT, UploadDataT>::upload(upload_data_t const& data)
{
	{
		std::lock_guard const lk{m_mutex_loop_upload};
		m_upload_data = std::make_shared<upload_data_t>(data);
	}
	upload(m_upload_data);
}

template <typename RcfClientT, typename UploadDataT>
void OnDemandUpload<RcfClientT, UploadDataT>::wait()
{
	using namespace std::chrono_literals;
	std::unique_lock lk{m_mutex_loop_upload};
	if (!m_is_uploaded.load(std::memory_order_acquire)) {
		m_cv_wait_for_finish.wait_for(
		    lk, 100ms, [&] { return m_is_uploaded.load(std::memory_order_acquire); });
	}
}

template <typename RcfClientT, typename UploadDataT>
bool OnDemandUpload<RcfClientT, UploadDataT>::holds_data()
{
	std::lock_guard const lk{m_mutex_loop_upload};
	return bool(m_upload_data);
}

template <typename RcfClientT, typename UploadDataT>
bool OnDemandUpload<RcfClientT, UploadDataT>::is_upload_thread_running()
{
	if (m_thread_loop_upload.joinable()) {
		std::lock_guard const lk{m_mutex_loop_upload};
		return !m_threads_safe_to_join.contains(m_thread_loop_upload.get_id());
	} else {
		return false;
	}
}

template <typename RcfClientT, typename UploadDataT>
void OnDemandUpload<RcfClientT, UploadDataT>::update_function_create_client(
    f_create_client_shared_ptr_t&& func)
{
	m_f_create_client = func;
}

template <typename RcfClientT, typename UploadDataT>
void OnDemandUpload<RcfClientT, UploadDataT>::refresh()
{
	if (!holds_data()) {
		RCF_LOG_TRACE(m_log, "Not holding data -> no refresh necessary.");
	} else if (is_upload_thread_running()) {
		// Note: m_unique_id not protected for debug message
		RCF_LOG_TRACE(
		    m_log,
		    "Upload thread still running with id " << m_unique_id << "-> no refresh necessary.");
	} else {
		RCF_LOG_TRACE(m_log, "Performing refresh..");

		upload_data_shared_ptr_t upload_data;
		{
			std::lock_guard const lk{m_mutex_loop_upload};
			upload_data = m_upload_data;
		}
		join_loop_upload();
		start_upload_thread(upload_data);

		RCF_LOG_TRACE(m_log, "Performed refresh..");
	}
}

template <typename RcfClientT, typename UploadDataT>
void OnDemandUpload<RcfClientT, UploadDataT>::upload(upload_data_shared_ptr_t const& upload_data)
{
	prepare_new_upload();
	start_upload_thread(upload_data);
}

template <typename RcfClientT, typename UploadDataT>
void OnDemandUpload<RcfClientT, UploadDataT>::start_upload_thread(
    upload_data_shared_ptr_t const& upload_data)
{
	m_thread_loop_upload = std::jthread([this, upload_data](std::stop_token st) {
		loop_upload(std::move(st), upload_data, m_unique_id);
	});

	RCF_LOG_TRACE(m_log, "Waiting for server to acknowledge reinit.");
	std::unique_lock lk{m_mutex_loop_upload};
	while (!m_is_notified.load(std::memory_order_acquire)) {
		m_cv_wait_for_finish.wait(
		    lk, [&] { return m_is_notified.load(std::memory_order_acquire); });
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

	// used at several locations -> DRY
	auto note_thread_finished = [&, this] {
		stop_flag = true;
		std::lock_guard const lk{m_mutex_safe_to_join};
		m_threads_safe_to_join.insert(std::this_thread::get_id());
	};

	while (!(st.stop_requested() || stop_flag)) {
		RCF_LOG_TRACE(log, "New iteration..");
		try {
			RCF_LOG_TRACE(log, "Notifying..");
			client = connect(st);
			std::invoke(m_f_notify, *client, m_unique_id);
			if (st.stop_requested()) {
				note_thread_finished();
				break;
			}
			{
				RCF_LOG_TRACE(log, "Did notify..");
				std::lock_guard lk{m_mutex_loop_upload};
				// make sure that we only update if no other upload was started in the meantime
				if (m_unique_id == unique_id) {
					m_is_notified.store(true, std::memory_order_release);
				}
			}
			m_cv_wait_for_finish.notify_all();

			RCF_LOG_TRACE(log, "Pending..");
			client = connect(st);
			bool response_perform_upload = std::invoke(m_f_pending, *client, m_unique_id);
			if (st.stop_requested()) {
				note_thread_finished();
				break;
			}
			if (response_perform_upload) {
				RCF_LOG_TRACE(log, "Commencing upload.");
				// We need to reconnect the client to refresh user data while creating.
				// hxcomm uses munge to set user data.
				client = connect(st);
				std::invoke(m_f_upload, *client, *upload_data, m_unique_id);
				if (st.stop_requested()) {
					note_thread_finished();
					break;
				}

				std::lock_guard lk{m_mutex_loop_upload};
				// make sure that we only update if no other upload was started in the meantime
				if (m_unique_id == unique_id) {
					m_is_notified.store(true, std::memory_order_release);
					m_is_uploaded.store(true, std::memory_order_release);
				}
				RCF_LOG_TRACE(log, "Upload completed.");
			} else {
				RCF_LOG_TRACE(log, "Upload aborted.");
				note_thread_finished();
				break;
			}
		} catch (RCF::Exception const& e) {
			if (st.stop_requested()) {
				note_thread_finished();
				break;
			}
			++num_errors;
			RCF_LOG_WARN(log, "Error while uploading: " << e.what());
			std::this_thread::sleep_for(delay_after_error);
		}
		if (num_errors >= num_errors_max) {
			RCF_LOG_ERROR(log, "Encountered " << num_errors_max << ", aborting!");
			note_thread_finished();
			break;
		}
		m_cv_wait_for_finish.notify_all();
	}
	// Again because we might have been terminated by stop_tokenj
	note_thread_finished();
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
