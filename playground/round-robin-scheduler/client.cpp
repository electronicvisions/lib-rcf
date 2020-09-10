#include "waiting-worker.h"

#include "rcf-extensions/logging.h"

#include <iostream>
#include <string>
#include <utility>
#include <boost/program_options.hpp>

namespace po = boost::program_options;

int main(int argc, const char* argv[])
{
	std::string ip, message, user;
	uint16_t port;
	size_t num_messages;
	bool silent = false;
	size_t log_level = 4;

	WorkUnit work_unit;

	po::options_description desc("Allowed options");
	desc.add_options()("help,h", "produce help message")(
	    "quiet,q", po::bool_switch(&silent), "suppress output")(
	    "ip,i", po::value<std::string>(&ip)->default_value("127.0.0.1"), "specify server IP")(
	    "port,p", po::value<uint16_t>(&port)->required(), "specify server port")(
	    "loglevel,l", po::value<size_t>(&log_level)->default_value(4),
	    "specify loglevel [0-ERROR,1-WARNING,2-INFO,3-DEBUG,4-TRACE]")(
	    "message,m", po::value<std::string>(&work_unit.message)->required(),
	    "specify message to print")(
	    "user,u", po::value<std::string>(&user)->required(), "specify issuing user")(
	    "runtime,r", po::value<size_t>(&work_unit.runtime)->default_value(1),
	    "specifiy the runtime on server")(
	    "number,n", po::value<size_t>(&num_messages)->default_value(1),
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

	logger_default_config(Logger::log4cxx_level(log_level));
	auto log = log4cxx::Logger::getLogger("client");

	if (!silent) {
		RCF_LOG_INFO(
		    log, "Calling with " << user << "/" << work_unit.runtime << "/" << work_unit.message);
	}

	RCF::globals().setDefaultConnectTimeoutMs(3600 * 1000);

	std::deque<std::pair<rr_waiter_client_t, RCF::Future<typename rr_waiter_t::work_return_t>>>
	    futures;

	for (size_t i = 0; i < num_messages; ++i) {
		rr_waiter_client_t client(RCF::TcpEndpoint(ip, port));
		client.getClientStub().setRemoteCallTimeoutMs(90 * 1000);
		client.getClientStub().setRequestUserData(user);
		client.getClientStub().getTransport().setMaxIncomingMessageLength(1280 * 1024 * 1024);

		RCF::Future<typename rr_waiter_t::work_return_t> future = client.submit_work(work_unit);
		futures.push_back(std::make_pair(std::move(client), std::move(future)));
	}

	for (auto& [client, future] : futures) {
		std::ignore = client;
		future.wait(0);
		if (!silent) {
			RCF_LOG_INFO(log, "Ran in job ID: " << *future);
		}
	}
	return 0;
}
