
#include "rcf-extensions/detail/round-robin-scheduler/session-storage.h"

#include <RCF/RCF.hpp>

namespace rcf_extensions::detail::round_robin_scheduler {

template <typename W>
SessionStorage<W>::SessionStorage() :
    m_log(log4cxx::Logger::getLogger("lib-rcf.SessionStorage")),
    m_stop_flag(false),
    m_session_cleanup([this](std::stop_token st) {
	    auto lk = lock();
	    auto const logger = log4cxx::Logger::getLogger("lib-rcf.SessionStorage.session_cleanup");
	    while (!st.stop_requested()) {
		    m_cv_session_cleanup.wait_for(
		        lk, m_session_timeout, [&] { return !st.stop_requested(); });
		    RCF_LOG_TRACE(logger, "Performing cleanup..");
		    std::erase_if(m_session_to_refcount, [&timeout(m_session_timeout)](auto const& item) {
			    auto const& [session_id, refcount] = item;
			    return (*refcount == 0) && refcount.is_elapsed(timeout);
		    });
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
void SessionStorage<W>::reinit_handle_notify(session_id_t const& session_id)
{
	ensure_registered(session_id);
	auto const lk = lock_guard();
	RCF_LOG_TRACE(m_log, "Handling notify for session: " << session_id);
	// clear previous init data if it exists
	m_session_to_reinit_data.erase(session_id);

	m_session_to_reinit_notify[session_id] = std::make_unique<DeferredUpload>();
}

template <typename W>
void SessionStorage<W>::reinit_store(session_id_t const& session_id, reinit_data_t&& data)
{
	ensure_registered(session_id);
	{
		auto const lk = lock_guard();
		RCF_LOG_TRACE(m_log, "Storing reinit data for session: " << session_id);
		m_session_to_reinit_data[session_id] = std::move(data);
		m_session_to_reinit_notify.erase(session_id);
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
	m_session_to_reinit_notify.erase(session_id);

	m_session_reinit_needed.erase(session_id);

	m_session_to_sequence_num.erase(session_id);
}

template <typename W>
void SessionStorage<W>::ensure_registered(session_id_t const& session_id)
{
	auto& session = RCF::getCurrentRcfSession();
	// SessionRegisteredToken tracks if we have checked this RcfSession before
	if (session.querySessionObject<SessionRegistered>() != nullptr) {
		return;
	} else {
		{
			auto const lk = lock_guard();
			if (m_session_to_refcount.contains(session_id)) {
				++m_session_to_refcount[session_id].get();
			} else {
				register_new_session_while_locked(session_id);
			}
		}
		session_id_t session_id_copy{session_id};
		session.setOnDestroyCallback([session_id_copy, this](RCF::RcfSession&) {
			auto const lk = lock_guard();
			--m_session_to_refcount[session_id_copy].get();
		});
		session.createSessionObject<SessionRegistered>();
	}
}

template <typename W>
void SessionStorage<W>::reinit_request(session_id_t const& session_id)
{
	auto const lk = lock_guard();
	if (m_session_to_reinit_notify.contains(session_id)) {
		m_session_to_reinit_notify[session_id]->request();
	}
}

template <typename W>
bool SessionStorage<W>::reinit_is_requested(session_id_t const& session_id) const
{
	auto const lk = lock_shared();
	return reinit_is_requested_while_locked(session_id);
}

template <typename W>
bool SessionStorage<W>::reinit_is_requested_while_locked(session_id_t const& session_id) const
{
	return (
	    m_session_to_reinit_notify.contains(session_id) &&
	    m_session_to_reinit_notify.at(session_id)->was_requested());
}

template <typename W>
bool SessionStorage<W>::reinit_is_needed(session_id_t const& session_id) const
{
	auto const lk = lock_shared();
	return m_session_reinit_needed.contains(session_id) ||
	       m_session_to_reinit_data.contains(session_id) ||
	       reinit_is_requested_while_locked(session_id);
}

template <typename W>
void SessionStorage<W>::reinit_set_needed(session_id_t const& session_id)
{
	auto const lk = lock_guard();
	m_session_reinit_needed.insert(session_id);
}

template <typename W>
std::optional<typename SessionStorage<W>::reinit_data_cref_t> SessionStorage<W>::reinit_get(
    session_id_t const& session_id)
{
	auto lk = lock_shared();
	// If there is a pending request -> get it and wait
	if (reinit_is_notified_while_locked(session_id)) {
		lk.unlock();
		reinit_request(session_id);
		lk.lock();
	}

	if (reinit_is_requested_while_locked(session_id)) {
		m_cv_new_reinit.wait(lk, [&session_id, this] {
			// If by some race condition a session gets disconnected while we
			// wait, the refcount will go to zero and the clean-up threat might
			// erase session data if enough time passes -> we might deadlock if
			// we don't check for it (though very very unlikely)
			return m_session_to_reinit_data.contains(session_id) ||
			       !m_session_to_refcount.contains(session_id);
		});
	}

	if (m_session_to_reinit_data.contains(session_id)) {
		return std::make_optional(std::cref(m_session_to_reinit_data[session_id]));
	} else {
		return std::nullopt;
	}
}

template <typename W>
bool SessionStorage<W>::reinit_is_notified(session_id_t const& session_id) const
{
	auto lk = lock_shared();
	return reinit_is_notified_while_locked(session_id);
}

template <typename W>
bool SessionStorage<W>::reinit_is_notified_while_locked(session_id_t const& session_id) const
{
	return m_session_to_reinit_notify.contains(session_id);
}

template <typename W>
SequenceNumber SessionStorage<W>::sequence_num_get(session_id_t const& session_id) const
{
	auto lk = lock_shared();
	return m_session_to_sequence_num.at(session_id);
}

template <typename W>
auto SessionStorage<W>::get_heap_sorter_most_completed() const
{
	auto const lk = lock_shared();
	// we need to copy the sequence numbers to get stable sorting
	// TODO: re-evaluate sorting
	session_to_sequence_num_t session_to_sequence_nums{m_session_to_sequence_num};

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
	auto lk = lock_guard();
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
	auto const lk = lock_guard();
	return get_total_refcount_while_locked();
}

template <typename W>
bool SessionStorage<W>::is_active(session_id_t const& session_id) const
{
	auto lk = lock_guard();
	return m_session_to_refcount.contains(session_id) &&
	       (*m_session_to_refcount.at(session_id) > 0);
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

} // namespace rcf_extensions::detail::round_robin_scheduler
