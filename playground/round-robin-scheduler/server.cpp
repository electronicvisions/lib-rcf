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
	size_t num_threads_input, num_threads_output;
	size_t timeout_seconds;
	size_t release_interval;
#ifdef RCF_LOG_THRESHOLD
	size_t loglevel = RCF_LOG_THRESHOLD;
#else
	size_t loglevel = 2; // info
#endif

	po::options_description desc("Allowed options");
	desc.add_options()("help,h", "produce help message")(
	    "ip,i", po::value<std::string>(&ip)->default_value("0.0.0.0"), "specify listening IP")(
	    "port,p", po::value<uint16_t>(&port)->required(), "specify listening port")(
	    "loglevel,l", po::value<size_t>(&loglevel),
	    "specify loglevel [0-TRACE,1-DEBUG,2-INFO,3-WARNING,4-ERROR]")(
	    "num-threads-input,n", po::value<size_t>(&num_threads_input)->default_value(4),
	    "number of threads for accepting work")(
	    "num-threads-output,m", po::value<size_t>(&num_threads_output)->default_value(4),
	    "number of threads for distributing work")(
	    "release-interval,r", po::value<size_t>(&release_interval)->default_value(0),
	    "Release interval of the server")(
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

	std::cout << "Setting loglevel to " << loglevel << std::endl;
	logger_default_config(Logger::log4cxx_level_v2(loglevel));

	auto log = log4cxx::Logger::getLogger(__func__);

	RCF_LOG_WARN(log, "Warn level enabled");
	RCF_LOG_INFO(log, "Info level enabled");
	RCF_LOG_DEBUG(log, "Debug level enabled");
	RCF_LOG_TRACE(log, "Trace level enabled");

	auto server = rr_waiter_construct(
	    RCF::TcpEndpoint(ip, port), Worker(), num_threads_input, num_threads_output);
	server->get_server().getServerTransport().setMaxIncomingMessageLength(1280 * 1024 * 1024);

	server->set_release_interval(std::chrono::seconds(release_interval));

	RCF_LOG_INFO(
	    log, "Started up (" << num_threads_input << "/" << num_threads_output << " threads)...");

	server->start_server(std::chrono::seconds(timeout_seconds));

	RCF_LOG_INFO(log, "Server shut down due to being idle for too long..");

	return 0;
}
