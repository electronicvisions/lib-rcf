#pragma once

#include <exception>

namespace rcf_extensions {

class UserNotAuthorized : public std::exception
{
public:
	const char* what() const noexcept
	{
		return "User is not authorized.";
	}
};

} // namespace rcf_extensions
