#include "waiting-worker.h"

#include "rcf-extensions/logging.h"

#include <log4cxx/simplelayout.h>

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
	size_t period_per_user_ms;
	size_t release_interval;
#ifdef RCF_LOG_THRESHOLD
    size_t loglevel = RCF_LOG_THRESHOLD;
#else
    size_t loglevel = 2; // info
#endif
    logger_default_config(Logger::log4cxx_level_v2(loglevel));

	po::options_description desc("Allowed options");
	desc.add_options()("help,h", "produce help message")(
	    "ip,i", po::value<std::string>(&ip)->default_value("0.0.0.0"), "specify listening IP")(
	    "port,p", po::value<uint16_t>(&port)->required(), "specify listening port")(
	    "loglevel,l", po::value<size_t>(&loglevel),
	    "specify loglevel [0-TRACE,1-DEBUG,2-INFO,3-WARNING,4-ERROR]")(
	    "num-threads-input,n", po::value<size_t>(&num_threads_pre)->default_value(4),
	    "number of threads for accepting work")(
	    "num-threads-output,m", po::value<size_t>(&num_threads_post)->default_value(4),
	    "number of threads for distributing work")(
	    "release-interval,r", po::value<size_t>(&release_interval)->default_value(0),
	    "Release interval of the server in seconds.")(
	    "user-period-ms,u", po::value<size_t>(&period_per_user_ms)->default_value(500),
	    "Number of seconds after which we forcibly switch users.")(
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

	RCF_LOG_INFO(log, "Starting up server. Listening on " << ip << ":" << port << ".");

	auto server = rr_waiter_construct(
	    RCF::TcpEndpoint(ip, port), Worker(), num_threads_pre, num_threads_post);
	server->get_server().getServerTransport().setMaxIncomingMessageLength(1280 * 1024 * 1024);

	server->set_release_interval(std::chrono::seconds(release_interval));

	server->set_period_per_user(std::chrono::milliseconds(period_per_user_ms));

	RCF_LOG_INFO(
	    log, "Started up (" << num_threads_pre << "/" << num_threads_post << " threads)...");

	server->start_server(std::chrono::seconds(timeout_seconds));

	std::cout << "Server shut down due to being idle for too long.." << std::endl;

	return 0;
}
