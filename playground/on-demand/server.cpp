
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <RCF/RCF.hpp>

#include "rcf-extensions/deferred-upload.h"
#include "rcf-extensions/logging.h"

#include "interface.hpp"

using namespace std::chrono_literals;

class OnDemandReload
{
public:
	using reinit_context_t = RCF::RemoteCallContext<bool>;

	OnDemandReload() : m_log(log4cxx::Logger::getLogger("OnDemandReload")){};

	bool notify_new_reinit()
	{
		RCF_LOG_INFO(m_log, "notify_new_reinit() received");

		m_received_reinit.reset();
		m_upload = std::make_unique<rcf_extensions::DeferredUpload>();

		return true;
	}

	void upload_new_reinit(ReinitData data)
	{
		RCF_LOG_INFO(m_log, "Received data with payload " << data.payload);
		m_received_reinit = std::make_unique<ReinitData>(data);
		RCF_LOG_INFO(m_log, "Stored received data: " << m_received_reinit->payload);
	}

	ReinitData request_reinit()
	{
		RCF_LOG_INFO(m_log, "Requesting Reinit..");
		if (m_upload) {
			m_upload->request();

			std::size_t num_attempts = 0;

			while (!(m_upload->is_done() && m_received_reinit) && num_attempts < 100) {
				if (!m_upload->is_done()) {
					RCF_LOG_INFO(m_log, "Request to upload not done, waiting..");
				}
				if (!m_received_reinit) {
					RCF_LOG_INFO(m_log, "Did not receive ReinitData yet.");
				}
				std::this_thread::sleep_for(100ms);
				++num_attempts;
			}

			if (!m_upload->is_done()) {
				RCF_LOG_ERROR(m_log, "Failed to communicate upload request to client!");
				throw std::runtime_error("Failed to communicate upload request to client!");
			} else if (!m_received_reinit) {
				RCF_LOG_ERROR(m_log, "Did not receive upload data..");
				throw std::runtime_error("Did not receive upload data..");
			}
			return *m_received_reinit;
		} else {
			RCF_LOG_ERROR(m_log, "Requested reinit without pending upload");
			throw std::runtime_error("Requested reinit without pending upload");
		}
	}

private:
	log4cxx::Logger* m_log;
	std::unique_ptr<rcf_extensions::DeferredUpload> m_upload;
	std::unique_ptr<ReinitData> m_received_reinit;
};

using namespace std::chrono_literals;

int main(int argc, char* argv[])
{
	RCF::RcfInit rcfInit;

#ifdef RCF_LOG_THRESHOLD
	size_t loglevel = RCF_LOG_THRESHOLD;
#else
	size_t loglevel = 2; // info
#endif
	logger_default_config(Logger::log4cxx_level_v2(loglevel));
	auto log = log4cxx::Logger::getLogger("OnDemandSimpleTest.server");

	std::string networkInterface = "0.0.0.0";
	int port = 50001;

	if (argc > 1) {
		port = atoi(argv[1]);
	}
	RCF_LOG_INFO(log, "Starting server on " << networkInterface << ":" << port << ".");

	// Start a TCP server, and expose OnDemand.
	OnDemandReload runner;
	RCF::RcfServer server(RCF::TcpEndpoint(networkInterface, port));
	server.bind<I_OnDemandReload>(runner);
	RCF::ThreadPoolPtr tpPtr(new RCF::ThreadPool(4));
	server.setThreadPool(tpPtr);
	server.start();

	auto sleep = 15s;

	RCF_LOG_INFO(log, "Sleeping for " << sleep.count() << " seconds");
	std::this_thread::sleep_for(sleep);

	return 0;
}
