#pragma once

#include "rcf-extensions/logging.h"
#include "rcf-extensions/round-robin-scheduler.h"

struct WorkUnit
{
	size_t runtime;
	std::string message;

	void serialize(SF::Archive& ar)
	{
		ar& runtime& message;
	}
};


class Worker
{
public:
	Worker() : m_log(log4cxx::Logger::getLogger("WaitingWorker")), m_job_count(0) {}

	void setup()
	{
		RCF_LOG_INFO(m_log, "Setting up..");
	}

	std::optional<std::string> verify_user(std::string const& user_data)
	{
		if (user_data != "mueller") {
			std::stringstream msg;
			RCF_LOG_INFO(
			    m_log, "[" << user_data << "] "
			               << "(verified)");
			return std::make_optional(user_data);
		} else {
			// mueller darf nicht
			RCF_LOG_WARN(
			    m_log, "[" << user_data << "] "
			               << "NEIN!");
			return std::nullopt;
		}
	}

	size_t work(WorkUnit const& work)
	{
		size_t job_id = m_job_count++;

		RCF_LOG_INFO(
		    m_log, "[#" << job_id << "] "
		                << "(started) " << work.runtime << " ms");

		RCF::sleepMs(work.runtime);

		RCF_LOG_INFO(m_log, "[#" << job_id << "] (finished) " << work.message);

		return job_id;
	}

	void teardown()
	{
		RCF_LOG_INFO(m_log, "Tearing down..");
	}

private:
	log4cxx::Logger* m_log;
	size_t m_job_count;
};

RR_GENERATE(Worker, rr_waiter)

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
