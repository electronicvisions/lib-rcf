
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

#include "rcf-extensions/logging.h"

#include "interface.hpp"

using namespace std::chrono_literals;

class OnDemandReload
{
public:
	using pending_context_t = RCF::RemoteCallContext<bool, std::size_t>;

	OnDemandReload() : m_log{log4cxx::Logger::getLogger("OnDemandReload")}, m_is_set_up{true} {};

	~OnDemandReload()
	{
		RCF_LOG_TRACE(m_log, "Shutting down server..");
		shut_down();
		RCF_LOG_TRACE(m_log, "Server shut down");
	}

	void shut_down()
	{
		RCF_LOG_TRACE(m_log, "Shutting down OnDemandReload");
		m_cv_upload.notify_one();
		std::lock_guard lk{m_mutex_upload};
		m_is_set_up = false;
		m_upload.reset();
		RCF_LOG_TRACE(m_log, "OnDemandReload shut down.");
	}

	void notify_new_reinit(std::size_t id)
	{
		if (!m_is_set_up) {
			RCF_LOG_INFO(
			    m_log, "notify_new_reinit() received with id " << id << "while not set up.");
		} else {
			if (m_reinit_id_notified != id) {
				RCF_LOG_INFO(m_log, "notify_new_reinit() received NEW reinit with id " << id);
				m_received_reinit.reset();
				// write reinit id down and return false, server will upload again
				m_reinit_id_notified = id;
			} else {
				RCF_LOG_INFO(m_log, "notify_new_reinit() notified the current id " << id);
			}
		}
	}

	bool pending_new_reinit(std::size_t id)
	{
		if (!m_is_set_up) {
			RCF_LOG_INFO(
			    m_log, "pending_new_reinit() received with id " << id << "while not set up.");
			return false;
		} else {
			std::lock_guard lk{m_mutex_upload};
			if (m_reinit_id_notified != id) {
				RCF_LOG_INFO(
				    m_log, "pending_new_reinit() received id " << id << " but expected "
				                                               << m_reinit_id_notified);
				return false;
			} else {
				RCF_LOG_INFO(
				    m_log, "pending_new_reinit() current reinit with id "
				               << id << ". Keep pending for upload..");
				m_reinit_id_pending = id;
				m_upload = std::make_unique<pending_context_t>(RCF::getCurrentRcfSession());
				RCF_LOG_TRACE(m_log, "Async call returning.");
				return true;
			}
		}
	}


	void upload_new_reinit(ReinitData data, std::size_t id)
	{
		if (!m_is_set_up) {
			return;
		} else {
			std::lock_guard lk{m_mutex_upload};
			if (m_reinit_id_notified != id) {
				RCF_LOG_INFO(
				    m_log,
				    "Received data with id " << id << " but expected " << m_reinit_id_notified);
				return;
			} else {
				RCF_LOG_INFO(
				    m_log, "Received data with payload " << data.payload << " and id " << id);
				m_received_reinit = std::make_unique<ReinitData>(data);
				m_reinit_id_stored = id;
				RCF_LOG_INFO(m_log, "Stored received data: " << m_received_reinit->payload);
			}
		}
	}

	ReinitData request_reinit()
	{
		using namespace std::chrono_literals;
		auto log = log4cxx::Logger::getLogger("request_reinit()");
		RCF_LOG_TRACE(
		    log, "request_reinit() started with [id_notified: "
		             << m_reinit_id_notified << ", id_pending: " << m_reinit_id_pending
		             << ", id_stored: " << m_reinit_id_stored << "]");

		std::unique_lock lk{m_mutex_upload};
		for (std::size_t i = 0; (m_reinit_id_notified != m_reinit_id_pending) && i < 100; ++i) {
			RCF_LOG_TRACE(log, "[" << i << "/100] Notified reinit is not yet pending, wait..");
			m_cv_upload.wait_for(lk, 100ms);
		}
		for (std::size_t num_attempts = 0;
		     (m_reinit_id_notified != m_reinit_id_stored) && num_attempts < 100; ++num_attempts) {
			RCF_LOG_TRACE(log, "Request handling attempt #" << num_attempts);
			if (!m_upload) {
				throw std::runtime_error("Lost connection while handling request_init()");
			} else if (m_reinit_id_notified != m_reinit_id_pending) {
				auto msg = "Notified reinit id did not become pending..";
				RCF_LOG_ERROR(log, msg);
				throw std::runtime_error(msg);
			} else {
				RCF_LOG_TRACE(log, "Requesting upload..");
				m_upload->parameters().r.set(true);
				m_upload->commit();
				m_upload.reset();
			}
			RCF_LOG_TRACE(log, "Sleeping..");
			m_cv_upload.wait_for(lk, 100ms);
		}

		RCF_LOG_INFO(log, "Reinit data received.");
		if (!m_received_reinit) {
			RCF_LOG_ERROR(log, "Did not receive upload data..");
			throw std::runtime_error("Did not receive upload data..");
		} else if (m_reinit_id_notified != m_reinit_id_stored) {
			auto msg = "Wrong id stored.";
			RCF_LOG_ERROR(log, msg);
			throw std::runtime_error(msg);
		} else if (!m_received_reinit) {
			RCF_LOG_ERROR(log, "Requested reinit without pending upload");
			throw std::runtime_error("Requested reinit without pending upload");
		} else {
			return *m_received_reinit;
		}
	}

private:
	log4cxx::Logger* m_log;
	std::mutex m_mutex_upload;
	std::condition_variable m_cv_upload;
	std::unique_ptr<pending_context_t> m_upload;
	std::unique_ptr<ReinitData> m_received_reinit;
	std::size_t m_reinit_id_notified;
	std::size_t m_reinit_id_pending;
	std::size_t m_reinit_id_stored;
	bool m_is_set_up;
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
	int sleep_s = 5;

	if (argc > 1) {
		port = atoi(argv[1]);
	}
	if (argc > 2) {
		sleep_s = atoi(argv[1]);
	}
	RCF_LOG_INFO(log, "Starting server on " << networkInterface << ":" << port << ".");

	// Start a TCP server, and expose OnDemand.
	OnDemandReload runner;
	std::unique_ptr<RCF::RcfServer> server{
	    new RCF::RcfServer{RCF::TcpEndpoint(networkInterface, port)}};
	server->bind<I_OnDemandReload>(runner);
	RCF::ThreadPoolPtr tpPtr(new RCF::ThreadPool(2));
	server->setThreadPool(tpPtr);
	server->start();

	RCF_LOG_INFO(log, "Sleeping for " << sleep_s << " seconds");
	std::this_thread::sleep_for(std::chrono::seconds(sleep_s));
	RCF_LOG_INFO(log, "Stopping server.");
	runner.shut_down();

	RCF_LOG_TRACE(log, "Exiting main()");
	return 0;
}
