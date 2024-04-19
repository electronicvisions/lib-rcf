#pragma once
#include "RCF/Exception.hpp"
#include "rcf-extensions/logging.h"
#include <chrono>
#include <functional>
#include <thread>
#include <type_traits>
#include <RCF/Future.hpp>
#include <log4cxx/logger.h>

namespace rcf_extensions {

namespace detail {

template <typename T>
struct is_future_converter : public std::false_type
{};

template <typename T>
struct is_future_converter<RCF::FutureConverter<T>> : public std::true_type
{};

} // namespace detail


/**
 * Invoke a function on a to be generated client with given arguments.
 * Invokation is retried on connection errors.
 * @tparam GetClient Function type getting a client
 * @tparam Function Function type to be invoked with the client
 * @tparam Args Arguments to the function
 * @param get_client Function to get a client
 * @param attempt_num_max Maximal number of attempts at invoking the function
 * @param wait_between_attempts Wait duration between successive attempts at invoking the function
 * @param function Function to invoke
 * @param args Arguments to function to invoke (additionally to client)
 */
template <typename GetClient, typename Function, typename... Args>
auto retrying_client_invoke(
    GetClient&& get_client,
    size_t attempt_num_max,
    std::chrono::milliseconds wait_between_attempts,
    Function&& function,
    Args&&... args)
{
	auto log = log4cxx::Logger::getLogger("lib-rcf.retrying_client_invoke");

	if (attempt_num_max == 0) {
		throw std::invalid_argument(
		    "Retrying client invoke needs attempt_num_max to be larger than zero.");
	}

	auto last_user_notification = std::chrono::system_clock::now();
	for (size_t attempts_performed = 1; attempts_performed <= attempt_num_max - 1;
	     ++attempts_performed) {
		// build request and send it to server
		try {
			auto client = get_client();
			auto ret =
			    std::invoke(std::forward<Function>(function), client, std::forward<Args>(args)...);
			if constexpr (detail::is_future_converter<decltype(ret)>::value) {
				return ret.get();
			} else {
				return ret;
			}
		} catch (RCF::Exception const& e) {
			if ((e.getErrorId() != RCF::RcfError_ClientConnectFail.getErrorId() &&
			     e.getErrorId() != RCF::RcfError_PeerDisconnect.getErrorId()) ||
			    attempts_performed == attempt_num_max) {
				// reraise if something unexpected happened or we reached the
				// maximum number of tries
				throw;
			}
		}
		using namespace std::chrono_literals;
		// Give the user feedback once per second in order to not spam the
		// terminal
		if ((std::chrono::system_clock::now() - last_user_notification) > 1s) {
			RCF_LOG_INFO(
			    log, "Server not ready yet, waiting "
			             << wait_between_attempts.count() << " ms in between attempts.. [Attempt: "
			             << attempts_performed << "/" << attempt_num_max << "]");
			last_user_notification = std::chrono::system_clock::now();
		}
		std::this_thread::sleep_for(wait_between_attempts);
	}
	// NOTE: Should never be reached.
	RCF_LOG_FATAL(log, "Could not submit request.");
	throw std::runtime_error("Error submitting request.");
}

} // namespace rcf_extensions
