#pragma once

#include <exception>
#include <sstream>
#include <string>

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

} // namespace rcf_extensions
