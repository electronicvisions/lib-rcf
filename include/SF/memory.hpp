
//******************************************************************************
// RCF - Remote Call Framework
//
// Copyright (c) 2005 - 2011, Delta V Software. All rights reserved.
// http://www.deltavsoft.com
//
// RCF is distributed under dual licenses - closed source or GPL.
// Consult your particular license for conditions of use.
//
// Version: 1.3.1
// Contact: support <at> deltavsoft.com 
//
//******************************************************************************

#ifndef INCLUDE_SF_MEMORY_HPP
#define INCLUDE_SF_MEMORY_HPP

#include <memory>

#include <SF/SerializeSmartPtr.hpp>

namespace SF {

    // serialize std::auto_ptr
    SF_SERIALIZE_SIMPLE_SMARTPTR( std::auto_ptr )

} // namespace SF

#endif // ! INCLUDE_SF_MEMORY_HPP
