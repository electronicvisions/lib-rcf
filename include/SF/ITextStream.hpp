
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

#ifndef INCLUDE_SF_ITEXTSTREAM_HPP
#define INCLUDE_SF_ITEXTSTREAM_HPP

#include <SF/Stream.hpp>

namespace SF {

    class ITextStream : public IStream
    {
    public:
        ITextStream() : IStream()
        {}

        ITextStream(std::istream &is) : IStream(is)
        {}

        I_Encoding &getEncoding()
        {
            return mEncoding;
        }

    private:
        EncodingText mEncoding;
    };

}

#endif // ! INCLUDE_SF_ITEXTSTREAM_HPP
