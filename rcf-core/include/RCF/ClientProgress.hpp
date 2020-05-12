
//******************************************************************************
// RCF - Remote Call Framework
//
// Copyright (c) 2005 - 2019, Delta V Software. All rights reserved.
// http://www.deltavsoft.com
//
// RCF is distributed under dual licenses - closed source or GPL.
// Consult your particular license for conditions of use.
//
// If you have not purchased a commercial license, you are using RCF 
// under GPL terms.
//
// Version: 3.1
// Contact: support <at> deltavsoft.com 
//
//******************************************************************************

/// \file

#ifndef INCLUDE_RCF_CLIENTPROGRESS_HPP
#define INCLUDE_RCF_CLIENTPROGRESS_HPP

#include <functional>
#include <memory>

#include <RCF/Enums.hpp>

namespace RCF {

    /// Describes the status of a remote call while in progress. See RCF::ClientStub::setRemoteCallProgressCallback().
    class RemoteCallProgressInfo
    {
    public:
        std::size_t         mBytesTransferred = 0;
        std::size_t         mBytesTotal = 0;
        RemoteCallPhase     mPhase = Rcp_Connect;
    };

} // namespace RCF

#endif // ! INCLUDE_RCF_CLIENTPROGRESS_HPP
