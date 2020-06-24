#pragma once

#include <exception>
#include <filesystem>

#include <sys/resource.h>
#include <sys/time.h>

namespace rcf_extensions {

/**
 * Get information about the maximum number of open files for the current process.
 *
 * This is important as each connection in RCF will allocate one file
 * descriptor and RCF will silently stop accepting new connections if file
 * descriptors are used up.
 *
 * @return rlimit object containing the current and maxmimum (soft and hard)
 * limits for allowed number of open file descriptors.
 */
inline rlimit get_limits_nofiles()
{
	rlimit rlim;

	getrlimit(RLIMIT_NOFILE, &rlim);

	return rlim;
}

inline void set_max_nofiles()
{
	rlimit rlim;

	getrlimit(RLIMIT_NOFILE, &rlim);
	rlim.rlim_cur = rlim.rlim_max;
	setrlimit(RLIMIT_NOFILE, &rlim);

	getrlimit(RLIMIT_NOFILE, &rlim);

	if (rlim.rlim_cur != rlim.rlim_max) {
		throw std::runtime_error("Could not set allowed number of open files maximum.");
	}
}

/**
 * Count and return the number of currently openend files by this process.
 *
 * @return Number of entries in /proc/self/fd.
 */
inline std::size_t get_num_open_fds()
{
	std::size_t count = 0;

	for (auto& entry :
	     std::filesystem::directory_iterator(std::filesystem::path("/proc/self/fd"))) {
		std::ignore = entry;
		++count;
	}

	return count;
}

} // namespace rcf_extensions
