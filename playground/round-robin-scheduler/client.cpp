#include "waiting-worker.h"

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

	WorkUnit work_unit;

	po::options_description desc("Allowed options");
	desc.add_options()("help,h", "produce help message")(
		"quiet,q", po::bool_switch(&silent), "suppress output")(
		"ip,i", po::value<std::string>(&ip)->default_value("127.0.0.1"), "specify server IP")(
		"port,p", po::value<uint16_t>(&port)->required(), "specify server port")(
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

	if (!silent) {
		std::cout << "Calling with " << user << "/" << work_unit.runtime << "/" << work_unit.message
				  << std::endl;
	}

	std::deque<std::pair<
		RcfClient<typename rr_waiter_t::rcf_interface_t>,
		RCF::Future<typename rr_waiter_t::work_return_t> > >
		futures;

	for (size_t i = 0; i < num_messages; ++i) {
		RcfClient<typename rr_waiter_t::rcf_interface_t> client(RCF::TcpEndpoint(ip, port));
		client.getClientStub().setRemoteCallTimeoutMs(90 * 1000);
		client.getClientStub().setRequestUserData(user);
		client.getClientStub().getTransport().setMaxIncomingMessageLength(1280 * 1024 * 1024);

		RCF::Future<typename rr_waiter_t::work_return_t> future = client.submit_work(work_unit);
		futures.push_back(std::make_pair(std::move(client), std::move(future)));
	}

	for (auto & [ client, future ] : futures) {
		std::ignore = client;
		future.wait(0);
		if (!silent) {
			std::cout << "Ran in job ID: " << *future << std::endl;
		}
	}
	return 0;
}
