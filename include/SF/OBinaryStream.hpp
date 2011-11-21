
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

#ifndef INCLUDE_SF_OBINARYSTREAM_HPP
#define INCLUDE_SF_OBINARYSTREAM_HPP

#include <SF/Stream.hpp>

namespace SF {

    class OBinaryStream : public OStream
    {
    public:
        OBinaryStream() : OStream()
        {}

        OBinaryStream(std::ostream &os) : OStream(os)
        {}

        I_Encoding &getEncoding()
        {
            return mEncoding;
        }

    private:
        EncodingBinaryPortable mEncoding;
    };

}

#endif // ! INCLUDE_SF_OBINARYSTREAM_HPP
