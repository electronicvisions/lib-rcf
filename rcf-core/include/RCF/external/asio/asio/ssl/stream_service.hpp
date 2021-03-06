//
// ssl/stream_service.hpp
// ~~~~~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2003-2015 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef ASIO_SSL_STREAM_SERVICE_HPP
#define ASIO_SSL_STREAM_SERVICE_HPP

#if defined(_MSC_VER) && (_MSC_VER >= 1200)
# pragma once
#endif // defined(_MSC_VER) && (_MSC_VER >= 1200)

#include "RCF/external/asio/asio/detail/config.hpp"

#if defined(ASIO_ENABLE_OLD_SSL)
# include "RCF/external/asio/asio/ssl/old/stream_service.hpp"
#endif // defined(ASIO_ENABLE_OLD_SSL)

#include "RCF/external/asio/asio/detail/push_options.hpp"

namespace asio {
namespace ssl {

#if defined(ASIO_ENABLE_OLD_SSL)

using asio::ssl::old::stream_service;

#endif // defined(ASIO_ENABLE_OLD_SSL)

} // namespace ssl
} // namespace asio

#include "RCF/external/asio/asio/detail/pop_options.hpp"

#endif // ASIO_SSL_STREAM_SERVICE_HPP
