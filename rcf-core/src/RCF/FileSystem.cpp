
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

#include <RCF/FileSystem.hpp>

#include <chrono>

namespace RCF
{

    namespace fs = RCF_FILESYSTEM_NS;

    Path makeCanonical(const Path& p)
    {
        //return fs::canonical(p);

        bool isUncPath = false;
        if ( p.string().substr(0, 2) == "//" )
        {
            isUncPath = true;
        }

        Path abs_p = p;

        Path result;
        for ( Path::iterator it = abs_p.begin();
        it != abs_p.end();
            ++it )
        {
            if ( *it == ".." )
            {
                // /a/b/.. is not necessarily /a if b is a symbolic link
                if ( fs::is_symlink(result) )
                {
                    result /= *it;
                }
                // /a/b/../.. is not /a/b/.. under most circumstances
                // We can end up with ..s in our result because of symbolic links
                else if ( result.filename() == ".." )
                {
                    result /= *it;
                }
                // Otherwise it should be safe to resolve the parent
                else
                {
                    result = result.parent_path();
                }
            }
            else if ( *it == "." )
            {
                // Ignore
            }
            else
            {
                // Just cat other path entries
                result /= *it;
            }
        }

        // Code above collapses the leading double slash for a UNC path. So here we put it back in.
        if ( isUncPath )
        {
            result = Path(*result.begin()) / result;
        }

        return result;
    }

    void setLastWriteTime(const Path& p, std::uint64_t writeTime)
    {
        std::chrono::milliseconds dur(writeTime);
        std::chrono::time_point<std::chrono::system_clock> dt(dur);
        fs::last_write_time(p, dt);
    }

    std::uint64_t getLastWriteTime(const Path& p)
    {
        fs::file_time_type ft = fs::last_write_time(p);
        std::uint64_t ticks = std::chrono::time_point_cast<std::chrono::milliseconds>(ft).time_since_epoch().count();
        return ticks;
    }
    
}
