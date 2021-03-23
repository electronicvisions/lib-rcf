#include "waiting-worker.h"

#include "rcf-extensions/logging.h"

#include <functional>
#include <iostream>
#include <string>
#include <utility>
#include <boost/program_options.hpp>

namespace po = boost::program_options;

using namespace std::chrono_literals;


int main(int argc, const char* argv[])
{
	std::string ip, message, user, session;
	uint16_t port;
	size_t num_messages;
	bool silent = false;
	bool out_of_order = false;
	bool synchronous = false;
	std::size_t reinit_runtime;
#ifdef RCF_LOG_THRESHOLD
	size_t loglevel = RCF_LOG_THRESHOLD;
#else
	size_t loglevel = 2; // info
#endif

	WorkUnit work_unit;

	po::options_description desc("Allowed options");
	desc.add_options()("help,h", "produce help message")(
	    "quiet,q", po::bool_switch(&silent), "suppress output")(
	    "out-of-order,o", po::bool_switch(&out_of_order), "submit in order or out of order")(
	    "synchronous,S", po::bool_switch(&synchronous),
	    "Submit jobs one by one in a synchronous fashion")(
	    "ip,i", po::value<std::string>(&ip)->default_value("127.0.0.1"), "specify server IP")(
	    "port,p", po::value<uint16_t>(&port)->required(), "specify server port")(
	    "loglevel,l", po::value<size_t>(&loglevel),
	    "specify loglevel [0-TRACE,1-DEBUG,2-INFO,3-WARNING,4-ERROR]")(
	    "message,m", po::value<std::string>(&work_unit.message)->required(),
	    "specify message to print")(
	    "user,u", po::value<std::string>(&user)->required(), "specify issuing user")(
	    "runtime,r", po::value<size_t>(&work_unit.runtime)->default_value(1),
	    "specifiy the runtime on server")(
	    "reinit-runtime,R", po::value<size_t>(&reinit_runtime)->default_value(100),
	    "specifiy the runtime on server")(
	    "session,s", po::value<std::string>(&session)->required(), "Session name for the")(
	    "num-messages,n", po::value<size_t>(&num_messages)->default_value(1),
	    "how many messages do we want to submit");

	// populate vm variable
	po::variables_map vm;
	po::store(po::parse_command_line(argc, argv, desc), vm);

	if (vm.count("help")) {
		std::cout << desc << std::endl;
		return EXIT_FAILURE;
	}
	po::notify(vm);

	RCF::RcfInit rcfInit;

	logger_default_config(Logger::log4cxx_level_v2(loglevel));
	auto log = log4cxx::Logger::getLogger("client");

	if (!silent) {
		RCF_LOG_INFO(
		    log, "Calling with " << user << "/" << work_unit.runtime << "/" << work_unit.message);
	}

	RCF::globals().setDefaultConnectTimeoutMs(24 * 3600 * 1000);

	std::deque<std::pair<
	    std::shared_ptr<rr_waiter_client_t>, RCF::Future<typename rr_waiter_t::work_return_t>>>
	    futures;

	ReinitWorkUnit reinit{reinit_runtime, "Reinit program for " + user + "@" + session, session};

	auto create_client = [ip, port, user, session] {
		auto client = std::make_shared<rr_waiter_client_t>(RCF::TcpEndpoint(ip, port));
		client->getClientStub().setRemoteCallTimeoutMs(24 * 3600 * 1000);
		client->getClientStub().setRequestUserData(user + ":" + session);
		return client;
	};

	auto uploader = rr_waiter_construct_reinit_uploader(create_client);

	work_unit.session_id = session;

	uploader.upload(std::move(reinit));
	create_client()->reinit_enforce();

	std::size_t previous_job_id = 0;

	for (size_t i = 0; i < num_messages; ++i) {
		auto client = create_client();

		WorkUnit my_work_unit(work_unit);
		my_work_unit.first_unit = (i == 0);

		if (!silent) {
			RCF_LOG_TRACE(
			    log, "Sending work unit [runtime: "
			             << my_work_unit.runtime << "ms, message: " << my_work_unit.message
			             << ", session_id: " << my_work_unit.session_id
			             << ", first_unit: " << my_work_unit.first_unit << "]");
		}

		if (synchronous) {
			if (!silent) {
				RCF_LOG_TRACE(
				    log, "Submitting synchronously.. #"
				             << i << " " << (out_of_order ? "ouf-of-order" : "in-order"));
			}
			std::size_t ran_in_job_id;
			if (out_of_order) {
				ran_in_job_id = client->submit_work(
				    my_work_unit, rcf_extensions::SequenceNumber::out_of_order());
			} else {
				ran_in_job_id = client->submit_work(my_work_unit, i);
			}
			if (!silent) {
				RCF_LOG_INFO(log, "Ran in job ID: " << ran_in_job_id);
			}

			if (!out_of_order) {
				if (previous_job_id == 0) {
					BOOST_ASSERT(ran_in_job_id >= previous_job_id);
				} else {
					BOOST_ASSERT(ran_in_job_id > previous_job_id);
				}
				previous_job_id = ran_in_job_id;
			}
		} else {
			if (!silent) {
				RCF_LOG_TRACE(
				    log, "Submitting asynchronously.. #"
				             << i << " " << (out_of_order ? "ouf-of-order" : "in-order"));
			}
			RCF::Future<typename rr_waiter_t::work_return_t> future;
			if (out_of_order) {
				future = client->submit_work(
				    my_work_unit, rcf_extensions::SequenceNumber::out_of_order());
			} else {
				future = client->submit_work(my_work_unit, i);
			}
			futures.push_back(std::make_pair(std::move(client), std::move(future)));
		}
	}

	if (!synchronous) {
		for (auto& [client, future] : futures) {
			std::ignore = client;
			future.wait(0);
			auto ran_in_job_id = *future;
			if (!silent) {
				RCF_LOG_INFO(log, "Ran in job ID: " << ran_in_job_id);
			}
			if (!out_of_order) {
				if (previous_job_id == 0) {
					BOOST_ASSERT(ran_in_job_id >= previous_job_id);
				} else {
					BOOST_ASSERT(ran_in_job_id > previous_job_id);
				}
				previous_job_id = ran_in_job_id;
			}
		}
	}
	return 0;
}
