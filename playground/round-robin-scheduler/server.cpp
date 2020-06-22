#include "waiting-worker.h"

#include <iostream>
#include <string>
#include <boost/program_options.hpp>

namespace po = boost::program_options;

int main(int argc, const char* argv[])
{
	std::string ip;
	uint16_t port;
	size_t num_threads_pre, num_threads_post;
	size_t timeout_seconds;

	po::options_description desc("Allowed options");
	desc.add_options()("help,h", "produce help message")(
	    "ip,i", po::value<std::string>(&ip)->default_value("0.0.0.0"), "specify listening IP")(
	    "port,p", po::value<uint16_t>(&port)->required(), "specify listening port")(
	    "num_threads_pre,n", po::value<size_t>(&num_threads_pre)->default_value(4),
	    "number of threads for accepting work")(
	    "num_threads_post,m", po::value<size_t>(&num_threads_post)->default_value(4),
	    "number of threads for distributing work")(
	    "timeout,t", po::value<size_t>(&timeout_seconds)->default_value(0),
	    "timeout in seconds till shutdown after idle");

	// populate vm variable
	po::variables_map vm;
	po::store(po::parse_command_line(argc, argv, desc), vm);

	if (vm.count("help")) {
		std::cout << desc << std::endl;
		return EXIT_FAILURE;
	}
	po::notify(vm);

	auto server = rr_waiter_construct(
	    RCF::TcpEndpoint(ip, port), Worker(), num_threads_pre, num_threads_post);
	server->get_server().getServerTransport().setMaxIncomingMessageLength(1280 * 1024 * 1024);

	std::cout << "Started up (" << num_threads_pre << "/" << num_threads_post << " threads)..."
	          << std::endl;

	server->start_server(std::chrono::seconds(timeout_seconds));

	std::cout << "Server shutting down due to being idle for too long.." << std::endl;

	return 0;
}
