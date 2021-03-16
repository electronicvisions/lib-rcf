
#include "hate/unordered_map.h"
#include "rcf-extensions/detail/round-robin-scheduler/session-storage.h"
#include <deque>
#include <thread>
#include <RCF/RCF.hpp>

namespace rcf_extensions::detail::round_robin_scheduler {

template <typename W>
SessionStorage<W>::SessionStorage() :
    m_log(log4cxx::Logger::getLogger("lib-rcf.SessionStorage")),
    m_stop_flag(false),
    m_session_cleanup([this](std::stop_token st) {
	    std::unique_lock lk{m_mutex};
	    auto const logger = log4cxx::Logger::getLogger("lib-rcf.SessionStorage.session_cleanup");
	    while (!st.stop_requested()) {
		    m_cv_session_cleanup.wait_for(
		        lk, m_session_timeout, [&] { return st.stop_requested(); });
		    RCF_LOG_TRACE(logger, "Performing cleanup..");
		    // TODO make better use of <algorithm>
		    std::deque<session_id_t> old_sessions;
		    for (auto const& [session_id, refcount] : m_session_to_refcount) {
			    if ((*refcount == 0) && refcount.is_elapsed(m_session_timeout)) {
				    old_sessions.push_back(session_id);
			    }
		    }
		    for (auto const& session_id : old_sessions) {
			    erase_session_while_locked(session_id);
		    }
	    }
    })
{
	// Increase limit of open files (i.e. connections) to avoid lock-problems
	set_max_nofiles();
	m_max_sessions = get_limits_nofiles().rlim_cur;
}

template <typename W>
SessionStorage<W>::~SessionStorage()
{
	RCF_LOG_TRACE(m_log, "Shutting down..");
	m_session_cleanup.request_stop();
	m_cv_session_cleanup.notify_all();
	m_session_cleanup.join();
	RCF_LOG_TRACE(m_log, "Shut down.");
}

template <typename W>
void SessionStorage<W>::reinit_handle_notify(session_id_t const& session_id, std::size_t reinit_id)
{
	ensure_registered(session_id);
	std::lock_guard const lk{m_mutex};
	auto id_notified = hate::get(m_session_to_reinit_id_notified, session_id);
	if (!id_notified || *id_notified != reinit_id) {
		RCF_LOG_TRACE(
		    m_log, "notify()-ed NEW reinit id " << reinit_id << " for session: " << session_id);
		// clear previous init data if it exists
		m_session_to_reinit_data.erase(session_id);
		m_session_to_reinit_id_notified[session_id] = reinit_id;
	} else {
		RCF_LOG_TRACE(m_log, "notify()-ed existing reinit id for session: " << session_id);
	}
}

template <typename W>
bool SessionStorage<W>::reinit_handle_pending(session_id_t const& session_id, std::size_t reinit_id)
{
	ensure_registered(session_id);
	std::lock_guard const lk{m_mutex};
	auto const id_notified = hate::cget(m_session_to_reinit_id_notified, session_id);
	if (id_notified && *id_notified == reinit_id) {
		RCF_LOG_TRACE(
		    m_log,
		    "Handling pending() for reinit id " << reinit_id << " in session: " << session_id);
		m_session_to_reinit_id_pending[session_id] = reinit_id;
		// clear previous init data if it exists
		abort_pending_upload_while_locked(session_id);
		m_session_to_deferred[session_id] =
		    std::make_unique<pending_context_t>(RCF::getCurrentRcfSession());
		return true;
	} else {
		RCF_LOG_WARN(
		    m_log, "pending() called for unexpected reinit id " << reinit_id << "in session "
		                                                        << session_id << " -> ignoring.");
		return false;
	}
}

template <typename W>
void SessionStorage<W>::reinit_store(
    session_id_t const& session_id, reinit_data_t&& data, std::size_t reinit_id)
{
	ensure_registered(session_id);
	std::lock_guard const lk{m_mutex};
	auto const id_notified = hate::cget(m_session_to_reinit_id_notified, session_id);
	auto const id_pending = hate::cget(m_session_to_reinit_id_pending, session_id);
	if (id_notified && id_pending && (*id_notified == *id_pending) && (*id_pending == reinit_id)) {
		RCF_LOG_TRACE(
		    m_log, "Storing reinit data with id " << reinit_id << " for session: " << session_id);
		m_session_to_reinit_data[session_id] = std::move(data);
		m_session_to_reinit_id_stored[session_id] = reinit_id;
	} else {
		RCF_LOG_WARN(
		    m_log, "Got unexpected reinit request for session: " << session_id << " -> ignoring.");
	}
	// notify possibly waiting process if reinit was explicitly requested
	m_cv_new_reinit.notify_all();
}

template <typename W>
void SessionStorage<W>::erase_session_while_locked(session_id_t const& session_id)
{
	RCF_LOG_TRACE(m_log, "Erasing session: " << session_id);

	m_session_to_refcount.erase(session_id);

	m_session_to_reinit_data.erase(session_id);
	abort_pending_upload_while_locked(session_id);

	m_session_to_reinit_id_notified.erase(session_id);
	m_session_to_reinit_id_pending.erase(session_id);
	m_session_to_reinit_id_stored.erase(session_id);

	m_session_reinit_needed.erase(session_id);

	m_session_to_sequence_num.erase(session_id);
}

template <typename W>
void SessionStorage<W>::ensure_registered(session_id_t const& session_id)
{
	auto& session = RCF::getCurrentRcfSession();
	// SessionRegisteredToken tracks if we have checked this RcfSession before
	if (session.querySessionObject<SessionRegistered>() != nullptr) {
		RCF_LOG_TRACE(m_log, "Session already registered: " << session_id);
		return;
	} else {
		{
			RCF_LOG_TRACE(m_log, "Preparing to update refcount: " << session_id);
			std::lock_guard const lk{m_mutex};
			RCF_LOG_TRACE(m_log, "Acquired guard: " << session_id);
			if (m_session_to_refcount.contains(session_id)) {
				RCF_LOG_TRACE(m_log, "Increasing refcount for: " << session_id);
				++m_session_to_refcount[session_id].get();
			} else {
				register_new_session_while_locked(session_id);
			}
		}
		session_id_t session_id_copy{session_id};
		session.setOnDestroyCallback([session_id_copy, this](RCF::RcfSession&) {
			std::lock_guard const lk{m_mutex};
			RCF_LOG_TRACE(m_log, "Decreasing refcount for session " << session_id_copy);
			--m_session_to_refcount[session_id_copy].get();
		});
		session.createSessionObject<SessionRegistered>();
	}
}

template <typename W>
void SessionStorage<W>::reinit_request(session_id_t const& session_id)
{
	RCF_LOG_TRACE(m_log, "Handling reinit request for session: " << session_id);
	std::lock_guard const lk{m_mutex};
	if (!is_active_while_locked(session_id)) {
		RCF_LOG_TRACE(m_log, "Session is not active -> no reinit requested: " << session_id);

	} else if (reinit_is_up_to_date_while_locked(session_id)) {
		RCF_LOG_TRACE(m_log, "Reinit up to date, not requesting: " << session_id);

	} else if (
	    reinit_is_pending_while_locked(session_id) &&
	    !(reinit_is_requested_while_locked(session_id))) {
		RCF_LOG_TRACE(m_log, "Requesting pending upload " << session_id);
		request_pending_upload_while_locked(session_id);
	} else {
		RCF_LOG_TRACE(m_log, "Could not request reinit for session " << session_id);
	}
}

template <typename W>
bool SessionStorage<W>::reinit_is_requested_while_locked(session_id_t const& session_id) const
{
	auto const id_notified = hate::cget(m_session_to_reinit_id_notified, session_id);
	auto const id_pending = hate::cget(m_session_to_reinit_id_pending, session_id);

	return (
	    id_notified && id_pending && (*id_notified == *id_pending) &&
	    !m_session_to_deferred.contains(session_id));
}

template <typename W>
bool SessionStorage<W>::reinit_is_needed(session_id_t const& session_id) const
{
	std::shared_lock const lk{m_mutex};
	return m_session_to_reinit_id_notified.contains(session_id);
}

template <typename W>
void SessionStorage<W>::reinit_set_needed(session_id_t const& session_id)
{
	std::lock_guard const lk{m_mutex};
	m_session_reinit_needed.insert(session_id);
}

template <typename W>
std::optional<typename SessionStorage<W>::reinit_data_cref_t> SessionStorage<W>::reinit_get(
    session_id_t const& session_id, std::optional<std::chrono::milliseconds> grace_period)
{
	std::shared_lock lk{m_mutex};
	if (reinit_is_up_to_date_while_locked(session_id)) {
		RCF_LOG_TRACE(m_log, "Getting reinit for session: " << session_id)
		return hate::cget(m_session_to_reinit_data, session_id);
	} else if (reinit_is_pending_while_locked(session_id)) {
		RCF_LOG_TRACE(m_log, "Reinit for session not up to date, requesting: " << session_id)
		// If there is a pending request -> request it and move to next
		lk.unlock();
		reinit_request(session_id);
		lk.lock();
		// Wait a short amount of time for the reinit
		if (!grace_period) {
			return std::nullopt;
		} else {
			m_cv_new_reinit.wait_for(lk, *grace_period);
			if (reinit_is_up_to_date_while_locked(session_id)) {
				RCF_LOG_TRACE(m_log, "Getting reinit for session: " << session_id)
				return hate::cget(m_session_to_reinit_data, session_id);
			} else {
				return std::nullopt;
			}
		}

	} else {
		return std::nullopt;
	}
}

template <typename W>
bool SessionStorage<W>::reinit_is_pending_while_locked(session_id_t const& session_id) const
{
	auto const id_notified = hate::cget(m_session_to_reinit_id_notified, session_id);
	auto const id_pending = hate::cget(m_session_to_reinit_id_pending, session_id);
	return (id_notified && id_pending && *id_notified == *id_pending);
}

template <typename W>
bool SessionStorage<W>::reinit_is_up_to_date_while_locked(session_id_t const& session_id) const
{
	auto const id_notified = hate::cget(m_session_to_reinit_id_notified, session_id);
	auto const id_pending = hate::cget(m_session_to_reinit_id_pending, session_id);
	auto const id_stored = hate::cget(m_session_to_reinit_id_stored, session_id);

	RCF_LOG_TRACE(
	    m_log, "Current reinit id state (notified/pending/stored/reinit_data): "
	               << (id_notified ? std::to_string(*id_notified) : std::string{"<undefined>"})
	               << "/" << (id_pending ? std::to_string(*id_pending) : std::string{"<undefined>"})
	               << "/" << (id_stored ? std::to_string(*id_stored) : std::string{"<undefined>"})
	               << "/" << std::boolalpha << m_session_to_reinit_data.contains(session_id));

	// All ids must match and the reinit data nees to exist!
	return (
	    id_notified && id_pending && id_stored && (*id_notified == *id_pending) &&
	    (*id_pending == *id_stored) && m_session_to_reinit_data.contains(session_id));
}

template <typename W>
void SessionStorage<W>::sequence_num_fast_forward(
    session_id_t const& session_id, SequenceNumber const& sequence_num)
{
	if (!(*sequence_num == 0 || sequence_num.is_out_of_order())) {
		std::shared_lock lk_shared{m_mutex};
		if (*m_session_to_sequence_num[session_id] == 0) {
			lk_shared.unlock();
			RCF_LOG_DEBUG(
			    m_log,
			    "[" << session_id << "] Fast-forwarding to sequence number: " << *sequence_num);
			std::lock_guard const lk{m_mutex};
			m_session_to_sequence_num[session_id] = sequence_num;
		}
	}
}

template <typename W>
SequenceNumber SessionStorage<W>::sequence_num_get(session_id_t const& session_id) const
{
	std::shared_lock const lk{m_mutex};
	return m_session_to_sequence_num.at(session_id);
}

template <typename W>
auto SessionStorage<W>::get_heap_sorter_most_completed() const
{
	std::shared_lock const lk{m_mutex};
	// we need to copy the sequence numbers to get stable sorting
	// TODO: re-evaluate sorting
	// Copy by assignment because default constructions are not protected by lock.
	session_to_sequence_num_t session_to_sequence_nums = m_session_to_sequence_num;

	return [session_to_sequence_nums](
	           work_package_t const& left, work_package_t const& right) mutable {
		auto seq_num_left = session_to_sequence_nums[left.session_id];
		auto seq_num_right = session_to_sequence_nums[right.session_id];

		if (seq_num_left != seq_num_right) {
			return seq_num_left < seq_num_right;
		} else {
			return SortDescendingBySequenceNum{}(left, right);
		}
	};
}

/**
 * Advance to the next squence number for session.
 */
template <typename W>
void SessionStorage<W>::sequence_num_next(session_id_t const& session_id)
{
	std::lock_guard const lk{m_mutex};
	++(*m_session_to_sequence_num[session_id]);
}

template <typename W>
void SessionStorage<W>::register_new_session_while_locked(session_id_t const& session_id)
{
	RCF_LOG_TRACE(m_log, "Registering new connection for session: " << session_id);
	m_session_to_refcount.insert(std::make_pair(session_id, refcount_type(1)));
	m_session_to_sequence_num.insert(std::make_pair(session_id, 0));

	auto total_refs = get_total_refcount_while_locked();

	// if we reach above 95% max sessions, explicitly check number of open files and issue
	// warnings
	if (95 * m_max_sessions < total_refs * 100) {
		auto current = get_num_open_fds();

		if (current == m_max_sessions) {
			RCF_LOG_ERROR(
			    m_log, "ALL file descriptors in use, system will not be able to handle additional "
			           "connections!");
		} else {
			RCF_LOG_WARN(
			    m_log, "Currently " << current << "/" << m_max_sessions
			                        << " in use, system might dead-lock if all connections are "
			                           "used up and parts of sequences are missing!");
		}
	}
}

template <typename W>
std::size_t SessionStorage<W>::get_total_refcount() const
{
	std::shared_lock const lk{m_mutex};
	return get_total_refcount_while_locked();
}

template <typename W>
bool SessionStorage<W>::is_active(session_id_t const& session_id) const
{
	std::shared_lock const lk{m_mutex};
	return is_active_while_locked(session_id);
}

template <typename W>
bool SessionStorage<W>::is_active_while_locked(session_id_t const& session_id) const
{
	auto const refcount = hate::cget(m_session_to_refcount, session_id);
	if (refcount) {
		RCF_LOG_TRACE(
		    m_log, "[Session: " << session_id << "] Reference count: " << *(*refcount).get());
	} else {
		RCF_LOG_TRACE(m_log, "No reference count for session: " << session_id);
	}
	return refcount && (*(*refcount).get() > 0);
}

template <typename W>
std::size_t SessionStorage<W>::get_total_refcount_while_locked() const
{
	std::size_t sum = 0;
	for (auto& refcount : m_session_to_refcount) {
		sum += *(refcount.second);
	}
	return sum;
}

template <typename W>
void SessionStorage<W>::abort_pending_upload_while_locked(session_id_t const& session_id)
{
	signal_pending_upload_while_locked(session_id, false);
}

template <typename W>
void SessionStorage<W>::request_pending_upload_while_locked(session_id_t const& session_id)
{
	signal_pending_upload_while_locked(session_id, true);
}

template <typename W>
void SessionStorage<W>::signal_pending_upload_while_locked(
    session_id_t const& session_id, bool value)
{
	auto context = hate::get(m_session_to_deferred, session_id);
	if (context) {
		// std::optional -> std::reference_wrapper -> std::unique_ptr
		pending_context_t pending{*(*context).get()};
		std::thread dispatch{[=]() mutable {
			pending.parameters().r.set(value);
			pending.commit();
		}};
		dispatch.detach();
	}
	m_session_to_deferred.erase(session_id);
}

} // namespace rcf_extensions::detail::round_robin_scheduler
