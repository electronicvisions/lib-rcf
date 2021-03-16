#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_set>

#include "hate/track_modifications.h"

#include "rcf-extensions/adjust-ulimit.h"
#include "rcf-extensions/detail/round-robin-scheduler/work-methods.h"
#include "rcf-extensions/logging.h"

namespace rcf_extensions::detail::round_robin_scheduler {

using namespace std::chrono_literals;

/**
 * Helper class that stores and provides session-specific data, most prominently reinit data.
 *
 * Uses the OnDemandUpload-concept.
 */
template <typename Worker>
class SessionStorage
{
public:
	using worker_t = Worker;
	using reinit_data_t = typename work_methods<worker_t>::reinit_data_t;
	using session_id_t = typename work_methods<worker_t>::session_id_t;
	using work_package_t = typename work_methods<worker_t>::work_package_t;

	using reinit_data_cref_t = std::reference_wrapper<reinit_data_t const>;

	using pending_context_t = RCF::RemoteCallContext<bool, std::size_t>;

	using mutex_t = std::shared_mutex;

	SessionStorage();

	~SessionStorage();

	/**
	 * Handle a new notification for a reinit data upload.
	 * Any call to reinit_get() will return nullopt until the given reinit id is uploaded.
	 *
	 * @param session_id for which session to add reinit notification
	 * @param reinit_id the identifier of the reinit data that could be uploaded
	 */
	void reinit_handle_notify(session_id_t const& session_id, std::size_t reinit_id);

	/**
	 * Handle a new pending upload (i.e. a client call that is blocked until
	 * the given upload is needed on the server-side).
	 *
	 * @param session_id for which session to add reinit notification
	 * @param reinit_id the identifier of the reinit data that could be uploaded
	 */
	bool reinit_handle_pending(session_id_t const& session_id, std::size_t reinit_id);

	/**
	 * Store the given data (called from upload-function).
	 *
	 * @param session_id which session to store reinit data for
	 * @param data to store
	 * @param id of the reinit data
	 */
	void reinit_store(session_id_t const& session_id, reinit_data_t&&, std::size_t reinit_id);

	/**
	 * Register the given session id with the current RCF::RcfSession.
	 *
	 * If the RcfSession is already registered, nothing happens.
	 *
	 * @param session_id for which to ensure it is registered
	 */
	void ensure_registered(session_id_t const& session_id);

	/**
	 * Request reinit data from the client if there is a pending notification.
	 *
	 * Otherwise do nothing.
	 *
	 * @param session_id for which session to request the reinit
	 */
	void reinit_request(session_id_t const& session_id);

	/**
	 * @return whether a reinit program is needed.
	 */
	bool reinit_is_needed(session_id_t const& session_id) const;

	/**
	 * Indicate that a reinit is mandatory for the given session_id.
	 */
	void reinit_set_needed(session_id_t const& session_id);

	/**
	 * Get a const reference to the given reinit_data_t if available.
	 *
	 * The reint_data_t will be requested if it was not requested up until now.
	 * Please note that this function returning nullopt does _not_ mean that no
	 * reinit is needed, but merely that it is not available yet.
	 *
	 * @param session_id The session for which to get reinit
	 * @param grace_period optional grace period to wait for reinit
	 * @return optional indicating whether correct reinit data is available.
	 */
	std::optional<reinit_data_cref_t> reinit_get(
	    session_id_t const& session_id,
	    std::optional<std::chrono::milliseconds> grace_period = std::nullopt);

	/**
	 * Adjust the given sessions sequence number.
	 * This is necessary if the server is restarted while the client is still running.
	 * Hence the client will continue sending well advanced sequence numbers
	 * that would stall forever.
	 * Hence, if the server still expects to see sequence number zero, but the
	 * client submits sequence number 2 or higher (i.e. 2 work packages would
	 * have been lost if the server did not restart) we fast forward the
	 * sequence number.
	 *
	 * @param session_id Session for which to fast forward sequence numbers.
	 * @param sequence_num The currently submitted sequence number.
	 */
	void sequence_num_fast_forward(
	    session_id_t const& session_id, SequenceNumber const& sequence_num);

	/**
	 * Get current (i.e., expected) sequence number for session.
	 */
	SequenceNumber sequence_num_get(session_id_t const& session_id) const;

	/**
	 * Advance to the next squence number for session.
	 */
	void sequence_num_next(session_id_t const& session_id);

	/**
	 * Get a sorter for work queue heaps that prefers sessions that have more
	 * work done already.
	 *
	 * @return Sorter to be given to InputQueue::{add_work,retrieve_work}.
	 */
	auto get_heap_sorter_most_completed() const;

	/**
	 * Get the total number of tracked sessions.
	 *
	 * @return Total number of tracked sessions.
	 */
	std::size_t get_total_refcount() const;

	/**
	 * If a session has no active connections, its work packages can be discarded.
	 *
	 * This avoids wasting resources in case a client has crashed or disconnected.
	 *
	 * @return if the given session still has active connections.
	 */
	bool is_active(session_id_t const& session_id) const;

private:
	log4cxx::Logger* m_log;

	/**
	 * Time interval after which to perform a clean up of old sessions.
	 */
	static constexpr auto m_session_timeout = 5min;

	/**
	 * Token to track if have the given session registered in our reference
	 * counting and set its onDestroyCallback.
	 *
	 * It gets constructed for every session to indicate whether we already
	 * track it or not.
	 */
	struct SessionRegistered
	{};

	mutable mutex_t m_mutex;

	// the current not-yet-uploaded reinit data for each session that has one registered
	using session_to_reinit_pending_t =
	    std::unordered_map<session_id_t, std::unique_ptr<pending_context_t>>;
	session_to_reinit_pending_t m_session_to_deferred;

	// the current already-uploaded reinit data for each session that has one registered
	using session_to_reinit_data_t = std::unordered_map<session_id_t, reinit_data_t>;
	session_to_reinit_data_t m_session_to_reinit_data;

	// track how many connections still reference the session
	using refcount_type = hate::TrackModifications<int>;
	using session_to_refcount_t = std::unordered_map<session_id_t, refcount_type>;
	session_to_refcount_t m_session_to_refcount;

	using session_to_reinit_id_t = std::unordered_map<session_id_t, std::size_t>;
	// track which reinit id:
	// -> could be uploaded
	session_to_reinit_id_t m_session_to_reinit_id_notified;
	// -> pending to be uploaded
	session_to_reinit_id_t m_session_to_reinit_id_pending;
	// -> actually is uploaded and stored
	session_to_reinit_id_t m_session_to_reinit_id_stored;

	// Track if the user has indicated that a reinit program is needed (so we know
	// if we have to wait or not).
	using session_set_t = std::unordered_set<session_id_t>;
	session_set_t m_session_reinit_needed;

	using session_to_sequence_num_t = std::unordered_map<session_id_t, SequenceNumber>;
	session_to_sequence_num_t m_session_to_sequence_num;

	// condition variable that gets notified whenever a new reinit-program gets uploaded
	std::condition_variable_any m_cv_new_reinit;

	std::size_t m_max_sessions;

	bool m_stop_flag;

	std::condition_variable_any m_cv_session_cleanup;
	std::jthread m_session_cleanup;

	/**
	 * Exclusive ownwership when modifying.
	 */
	auto lock() const
	{
		return std::unique_lock<mutex_t>{m_mutex};
	}

	/**
	 * Shared ownwership when reading.
	 */
	auto lock_shared() const
	{
		return std::shared_lock<mutex_t>{m_mutex};
	}

	auto lock_guard() const
	{
		return std::lock_guard<mutex_t>{m_mutex};
	}

	void erase_session_while_locked(session_id_t const& session_id);

	bool reinit_is_requested_while_locked(session_id_t const& session_id) const;

	/**
	 * Check if the given session id belongs to a registered session and can
	 * hence be expected to exist in all maps.
	 *
	 * @param session_id session to check.
	 * @return true if given session id is registered.
	 */
	bool is_active_while_locked(session_id_t const& session_id) const;

	/**
	 * Indicate whether the reinit for the given session can be requested.
	 *
	 * @param session_id Session for which to check.
	 * @return If reinit can be requested.
	 */
	bool reinit_is_pending_while_locked(session_id_t const& session_id) const;

	bool reinit_is_up_to_date_while_locked(session_id_t const& session_id) const;

	void register_new_session_while_locked(session_id_t const& session_id);

	void abort_pending_upload_while_locked(session_id_t const& session_id);

	void request_pending_upload_while_locked(session_id_t const& session_id);

	void signal_pending_upload_while_locked(session_id_t const& session_id, bool);

	std::size_t get_total_refcount_while_locked() const;
};

} // namespace rcf_extensions::detail::round_robin_scheduler

#include "rcf-extensions/detail/round-robin-scheduler/session-storage.tcc"
