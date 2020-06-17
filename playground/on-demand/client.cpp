#include <chrono>

#include "rcf-extensions/logging.h"
#include "rcf-extensions/on-demand-upload.h"

#include "interface.hpp"

#include <RCF/RCF.hpp>

int main(int argc, char* argv[])
{
	using namespace std::chrono_literals;

#ifdef RCF_LOG_THRESHOLD
	size_t loglevel = RCF_LOG_THRESHOLD;
#else
	size_t loglevel = 2; // info
#endif
	logger_default_config(Logger::log4cxx_level_v2(loglevel));
	auto log = log4cxx::Logger::getLogger("OnDemandSimpleTest.client");

	RCF::RcfInit rcfInit;

	std::string networkInterface = "127.0.0.1";

	int port = 50001;

	if (argc > 1) {
		port = atoi(argv[1]);
	}
	RCF_LOG_INFO(log, "Connecting to server on " << networkInterface << ":" << port << ".");
	auto connect_to = RCF::TcpEndpoint(networkInterface, port);
	RcfClient<I_OnDemandReload> client(connect_to);

	client.getClientStub().setRemoteCallTimeoutMs(60000);

	auto my_reinit_data = ReinitData(42);

	using rcf_client_t = RcfClient<I_OnDemandReload>;
	using uploader_t = rcf_extensions::OnDemandUpload<rcf_client_t, ReinitData>;
	uploader_t uploader(
	    [connect_to]() -> decltype(auto) {
		    auto client = std::make_shared<on_demand_client_t>(connect_to);
		    client->getClientStub().setRemoteCallTimeoutMs(60000);
		    return client;
	    },
	    &uploader_t::client_t::notify_new_reinit, &uploader_t::client_t::upload_new_reinit);

	uploader.upload(my_reinit_data);

	std::this_thread::sleep_for(3s);

	auto poor_mans_assert = [log](ReinitData const& actual, ReinitData const& expected) {
		if (actual != expected) {
			RCF_LOG_ERROR(log, "Exptected: " << expected.payload << " Got: " << actual.payload);
			std::exit(1);
		}
	};

	RCF_LOG_INFO(log, "request_reinit()");
	poor_mans_assert(client.request_reinit(), my_reinit_data);

	std::this_thread::sleep_for(1s);

	// provide several re-init data pbmems, only the latest should get uploaded
	uploader.upload(ReinitData(43));
	uploader.ensure_connected();

	auto my_reinit_data_2 = ReinitData(44);
	uploader.upload(std::move(my_reinit_data_2));
	uploader.ensure_connected();

	my_reinit_data = ReinitData(45);

	RCF_LOG_INFO(log, "request_reinit()");
	poor_mans_assert(client.request_reinit(), ReinitData(44));

	uploader.wait();

	return 0;
}
