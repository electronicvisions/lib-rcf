#include "waiting-worker.h"

#include "rcf-extensions/logging.h"

#include <boost/program_options.hpp>

#include <chrono>
#include <iostream>
#include <string>

namespace po = boost::program_options;

int main(int argc, const char* argv[])
{
	using namespace std::chrono_literals;

	std::string ip;
	uint16_t port;
	size_t num_threads_pre, num_threads_post;
	size_t timeout_seconds;
	size_t log_level = 4;

	po::options_description desc("Allowed options");
	desc.add_options()("help,h", "produce help message")(
	    "ip,i", po::value<std::string>(&ip)->default_value("0.0.0.0"), "specify listening IP")(
	    "port,p", po::value<uint16_t>(&port)->required(), "specify listening port")(
	    "loglevel,l", po::value<size_t>(&log_level)->default_value(4),
	    "specify loglevel [0-ERROR,1-WARNING,2-INFO,3-DEBUG,4-TRACE]")(
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

	std::cout << "Setting loglevel to " << log_level << std::endl;
	logger_default_config(Logger::log4cxx_level(log_level));

	auto log = log4cxx::Logger::getLogger(__func__);

	RCF_LOG_ERROR(log, "Error level enabled");
	RCF_LOG_WARN(log, "Warn level enabled");
	RCF_LOG_INFO(log, "Info level enabled");
	RCF_LOG_DEBUG(log, "Debug level enabled");
	RCF_LOG_TRACE(log, "Trace level enabled");

	auto server = rr_waiter_construct(
	    RCF::TcpEndpoint(ip, port), Worker(), num_threads_pre, num_threads_post);
	server->get_server().getServerTransport().setMaxIncomingMessageLength(1280 * 1024 * 1024);

	std::cout << "Started up (" << num_threads_pre << "/" << num_threads_post << " threads)..."
	          << std::endl;

	server->start_server(std::chrono::seconds(timeout_seconds));

	std::cout << "Server shut down due to being idle for too long.." << std::endl;

	return 0;
}
