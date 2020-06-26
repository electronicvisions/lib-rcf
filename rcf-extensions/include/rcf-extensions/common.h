#pragma once

#include <exception>
#include <sstream>
#include <string>

#include <RCF/RCF.hpp>

namespace rcf_extensions {

class UserNotAuthorized : public std::exception
{
public:
	const char* what() const noexcept
	{
		return "User is not authorized.";
	}
};

class InvalidSequenceNumber : public std::exception
{
public:
	InvalidSequenceNumber(std::size_t num_actual, std::size_t num_expected) :
	    m_num_actual(num_actual), m_num_expected(num_expected)
	{
		std::stringstream ss;
		ss << "Work package had sequence number " << m_num_actual << " but processing expected "
		   << m_num_actual << ".";
		m_message = ss.str();
	}

	const char* what() const noexcept
	{
		return m_message.c_str();
	}

	/**
	 * The acutal sequence number observed.
	 */
	std::size_t get_actual() const
	{
		return m_num_actual;
	}

	/**
	 * The sequence number processing expected.
	 */
	std::size_t get_expected() const
	{
		return m_num_expected;
	}

private:
	std::string m_message;
	std::size_t m_num_actual;
	std::size_t m_num_expected;
};

/**
 * Helper function to get and verify user data.
 *
 * @tparam Args Arguments from which to construct the RCF::RemoteCallContext
 * (note that void needs to be mapped to RCF::Void).
 *
 * @return Optional containing the verified user data, if the user was verified.
 *         If the user was not verified, UserNotAuthorized-exception will be
 *         returned to the user and the caller should abort its exectuion.
 */
template <typename... Args, typename VerifierT>
auto get_verified_user_data(VerifierT& verifier)
{
	std::string user_data = RCF::getCurrentRcfSession().getRequestUserData();
	auto verified_user_session_id = verifier.verify_user(user_data);

	if (!verified_user_session_id) {
		RCF::RemoteCallContext<Args...> context(RCF::getCurrentRcfSession());
		context.commit(UserNotAuthorized());
	}
	return verified_user_session_id;
}

} // namespace rcf_extensions
