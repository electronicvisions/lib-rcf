#pragma once

#include "rcf-extensions/logging.h"

#include <iostream>
#include <memory>
#include <string>

#include <RCF/Idl.hpp>
#include <RCF/RCF.hpp>

struct ReinitData
{
	ReinitData() : payload(0) {}
	ReinitData(int payload) : payload(payload) {}

	int payload;

	bool operator==(ReinitData const& other) const
	{
		return payload == other.payload;
	}

	bool operator!=(ReinitData const& other) const
	{
		return !(*this == other);
	}
};

void get_time()
{
	auto time = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
	std::cout << std::ctime(&time) << " ";
}

namespace SF {

void serialize(SF::Archive& ar, ReinitData& data)
{
	auto log = log4cxx::Logger::getLogger("ReinitData");
	if (ar.isWrite()) {
		RCF_LOG_INFO(log, "Serializing (possibly) huge re-init payload data.");
	} else if (ar.isRead()) {
		RCF_LOG_INFO(log, "Deserializing (possibly) huge re-init payload data.");
	}
	ar& data.payload;
}

} // namespace SF


RCF_BEGIN(I_OnDemandReload, "I_OnDemandReload")
RCF_METHOD_R0(bool, notify_new_reinit)
RCF_METHOD_V1(void, upload_new_reinit, ReinitData)
RCF_METHOD_R0(ReinitData, request_reinit)
RCF_END(I_OnDemandReload)

using on_demand_client_t = RcfClient<I_OnDemandReload>;
using on_demand_client_shared_ptr = std::shared_ptr<on_demand_client_t>;
