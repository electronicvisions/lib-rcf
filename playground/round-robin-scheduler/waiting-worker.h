#include "rcf-extensions/round-robin.h"

struct WorkUnit
{
	size_t runtime;
	std::string message;

	void serialize(SF::Archive& ar) { ar& runtime& message; }
};


class Worker
{
public:
	Worker() : m_job_count(0) {}

	void setup() { std::cout << "Setting up waiting worker " << std::endl; }

	std::optional<std::string> verify_user(std::string const& user_data)
	{
		if (user_data != "mueller") {
			std::stringstream msg;
			msg << "Waiting Worker: [" << user_data << "] "
				<< "(verified) " << std::endl;
			std::cout << msg.str();
			return std::make_optional(user_data);
		} else {
			// mueller darf nicht
			std::stringstream msg;
			msg << "Waiting Worker: [" << user_data << "] "
				<< "NEIN! " << std::endl;
			std::cout << msg.str();
			return std::nullopt;
		}
	}

	size_t work(WorkUnit const& work)
	{
		size_t job_id = m_job_count++;

		std::cout << "Waiting Worker: [#" << job_id << "] "
				  << "(started) " << work.runtime << " ms" << std::endl;

		RCF::sleepMs(work.runtime);

		std::cout << "Waiting Worker: [#" << job_id << "] (finished) " << work.message << std::endl;

		return job_id;
	}

	void teardown() { std::cout << "Tearing down waiting worker " << std::endl; }

private:
	size_t m_job_count;
};

RR_GENERATE(Worker, rr_waiter_t)

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
