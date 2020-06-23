#pragma once

// Caution: Currently logging framework is not seperated into its own namespace!
#include "logger.h"
#include "logging_ctrl.h"

#if !defined(RCF_LOG_THRESHOLD) || RCF_LOG_THRESHOLD <= 0
#define RCF_LOG_TRACE(logger, message) LOG4CXX_TRACE(logger, message)
#else
#define RCF_LOG_TRACE(logger, message)
#endif

#if !defined(RCF_LOG_THRESHOLD) || RCF_LOG_THRESHOLD <= 1
#define RCF_LOG_DEBUG(logger, message) LOG4CXX_DEBUG(logger, message)
#else
#define RCF_LOG_DEBUG(logger, message)
#endif

#if !defined(RCF_LOG_THRESHOLD) || RCF_LOG_THRESHOLD <= 2
#define RCF_LOG_INFO(logger, message) LOG4CXX_INFO(logger, message)
#else
#define RCF_LOG_INFO(logger, message)
#endif

#if !defined(RCF_LOG_THRESHOLD) || RCF_LOG_THRESHOLD <= 3
#define RCF_LOG_WARN(logger, message) LOG4CXX_WARN(logger, message)
#else
#define RCF_LOG_WARN(logger, message)
#endif

#if !defined(RCF_LOG_THRESHOLD) || RCF_LOG_THRESHOLD <= 4
#define RCF_LOG_ERROR(logger, message) LOG4CXX_ERROR(logger, message)
#else
#define RCF_LOG_ERROR(logger, message)
#endif

#if !defined(RCF_LOG_THRESHOLD) || RCF_LOG_THRESHOLD <= 5
#define RCF_LOG_FATAL(logger, message) LOG4CXX_FATAL(logger, message)
#else
#define RCF_LOG_FATAL(logger, message)
#endif
