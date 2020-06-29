#pragma once

#include "rcf-extensions/logging.h"
#include "rcf-extensions/round-robin-reinit-scheduler.h"

#include <string>
#include <utility>

struct WorkUnit
{
	size_t runtime;
	std::string message;
	std::string session_id;
	bool first_unit;

	void serialize(SF::Archive& ar)
	{
		ar& runtime& message& session_id& first_unit;
	}
};

struct ReinitWorkUnit
{
	size_t runtime;
	std::string message;
	std::string session_id;

	void serialize(SF::Archive& ar)
	{
		if (ar.isWrite()) {
			RCF_LOG_DEBUG(
			    log4cxx::Logger::getLogger("ReinitWorkUnit"),
			    "Serializing (possibly) huge re-init payload data for " << message);
		}
		ar& runtime& message& session_id;
		if (ar.isRead()) {
			RCF_LOG_DEBUG(
			    log4cxx::Logger::getLogger("ReinitWorkUnit"),
			    "Deserializing (possibly) huge re-init payload data for " << message);
		}
	}
};

class Worker
{
public:
	Worker() :
	    m_log(log4cxx::Logger::getLogger("WaitingWorker")),
	    m_job_count(0),
	    m_current_session_id("<undefined>")
	{}

	void setup()
	{
		RCF_LOG_INFO(m_log, "Setting up..");
	}

	std::optional<std::pair<std::string, std::string>> verify_user(std::string const& user_data)
	{
		// user and session name are seperated by ':'
		std::string delimiter(":");
		auto session_idx = user_data.find(delimiter);

		if (session_idx == std::string::npos ||
		    session_idx + delimiter.length() >= user_data.length()) {
			RCF_LOG_WARN(m_log, "Invalid user data: " << user_data);
			return std::nullopt;
		}

		auto user_id = user_data.substr(0, session_idx);
		auto session_id = user_data.substr(
		    session_idx + delimiter.length(),
		    user_data.length() - delimiter.length() - session_idx);
		{
			std::stringstream ss;
			ss << user_id << "@" << session_id;
			session_id = ss.str();
		}

		if (user_id != "mueller") {
			RCF_LOG_INFO(
			    m_log, "[" << user_id << "->" << session_id << "] "
			               << "(verified)");
			return std::make_optional(std::make_pair(user_id, session_id));
		} else {
			// mueller darf nicht
			RCF_LOG_WARN(
			    m_log, "[" << user_id << "->" << session_id << "] "
			               << "NEIN!");
			return std::nullopt;
		}
	}

	std::size_t work(WorkUnit const& work)
	{
		if (work.first_unit) {
			// this unit of work does the setting up
			m_current_session_id = work.session_id;
			RCF_LOG_INFO(m_log, "First unit for session: " << m_current_session_id);
		} else if (work.session_id != m_current_session_id) {
			RCF_LOG_ERROR(
			    m_log, "Worker set up for session "
			               << m_current_session_id << ", but work unit expected " << work.session_id
			               << ". reinit failed?");
			throw std::runtime_error("Worker is NOT set up for current work unit - reinit failed?");
		}

		size_t job_id = m_job_count++;

		RCF_LOG_INFO(
		    m_log, "[#" << job_id << "] "
		                << "(started) " << work.runtime << " ms");

		RCF::sleepMs(work.runtime);

		RCF_LOG_INFO(m_log, "[#" << job_id << "] (finished) " << work.message);

		return job_id;
	}

	void perform_reinit(ReinitWorkUnit const& reinit)
	{
		RCF_LOG_INFO(m_log, "Performing reinint [" << reinit.runtime << "ms]: " << reinit.message);
		RCF::sleepMs(reinit.runtime);
		m_current_session_id = reinit.session_id;
		RCF_LOG_INFO(m_log, "Reinit done [" << reinit.runtime << "ms]: " << reinit.message);
	}

	void teardown()
	{
		RCF_LOG_INFO(m_log, "Tearing down..");
		m_current_session_id = "<undefined>";
	}

private:
	log4cxx::Logger* m_log;
	size_t m_job_count;

	std::string m_current_session_id;
};

RRWR_GENERATE(Worker, rr_waiter)

// Without macro:
/*
 * #pragma GCC diagnostic push
 * #pragma GCC diagnostic ignored "-Wdeprecated-declarations"
 * #pragma GCC diagnostic ignored "-Wterminate"
 * #include <RCF/RCF.hpp>
 * RCF_BEGIN(I_RoundRobin, "I_RoundRobin")
 * RCF_METHOD_R1(size_t, submit_work, WorkUnit)
 * RCF_END(I_RoundRobin)
 * typedef RoundRobinScheduler<Worker, I_RoundRobin> rr_waiter_t;
 * #pragma GCC diagnostic pop
 */
