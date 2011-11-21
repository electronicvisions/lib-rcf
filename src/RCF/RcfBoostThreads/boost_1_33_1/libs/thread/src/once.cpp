
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

// Copyright (C) 2001-2003
// William E. Kempf
//
// Permission to use, copy, modify, distribute and sell this software
// and its documentation for any purpose is hereby granted without fee,
// provided that the above copyright notice appear in all copies and
// that both that copyright notice and this permission notice appear
// in supporting documentation.  William E. Kempf makes no representations
// about the suitability of this software for any purpose.
// It is provided "as is" without express or implied warranty.

#ifdef BOOST_NO_STDC_NAMESPACE
namespace std { using ::sprintf; }
#endif

#if defined(BOOST_HAS_PTHREADS)
namespace {
pthread_key_t key;
pthread_once_t once = PTHREAD_ONCE_INIT;

typedef void (*once_callback)();
}

extern "C" {

    static void key_init()
    {
        pthread_key_create(&key, 0);
    }

    static void do_once()
    {
        once_callback* cb = reinterpret_cast<once_callback*>(
            pthread_getspecific(key));
        (**cb)();
    }

}
#elif defined(BOOST_HAS_MPTASKS)
namespace {
void *remote_call_proxy(void *pData)
{
    ::std::pair<void (*)(), boost::once_flag *> &rData(
        *reinterpret_cast< ::std::pair<void (*)(), boost::once_flag *> *>(pData));

    if(*rData.second == false)
    {
        rData.first();
        *rData.second = true;
    }

    return(NULL);
}
}

#elif defined(BOOST_HAS_WINTHREADS)
namespace {
// The signature for InterlockedCompareExchange has changed with the
// addition of Win64 support. I can't determine any (consistent and
// portable) way of using conditional compilation to detect this, so
// we use these type wrappers.  Unfortunately, the various vendors
// use different calling conventions and other signature anamolies,
// and thus have unique types as well.  This is known to work on VC6,
// VC7, Borland 5.5.2 and gcc 3.2.  Are there other signatures for
// other platforms?
inline LONG ice_wrapper(LONG (__stdcall *ice)(LONG*, LONG, LONG),
    volatile LONG* dest, LONG exch, LONG cmp)
{
    return (*ice)(const_cast<LONG*>(dest), exch, cmp);
}

inline LONG ice_wrapper(LONG (__stdcall *ice)(volatile LONG*, LONG, LONG),
    volatile LONG* dest, LONG exch, LONG cmp)
{
    return (*ice)(dest, exch, cmp);
}

inline LONG ice_wrapper(LPVOID (__stdcall *ice)(LPVOID*, LPVOID, LPVOID),
    volatile LONG* dest, LONG exch, LONG cmp)
{
    return (LONG)(*ice)((LPVOID*)dest, (LPVOID)exch, (LPVOID)cmp);
}

// The friendly form of InterlockedCompareExchange that defers
// according to the above function type wrappers.
inline LONG compare_exchange(volatile LPLONG dest, LONG exch, LONG cmp)
{
#if defined(_MSC_VER) && _MSC_VER == 1200
    return ice_wrapper(&InterlockedCompareExchange, dest, exch, cmp);
#else
    return InterlockedCompareExchange(dest, exch, cmp);
#endif
}
}
#endif

namespace boost {

void call_once(void (*func)(), once_flag& flag)
{
#if defined(BOOST_HAS_WINTHREADS)
    if (compare_exchange(&flag, 1, 1) == 0)
    {
#if defined(BOOST_NO_STRINGSTREAM)
        ::std::ostrstream strm;
        strm << "2AC1A572DB6944B0A65C38C4140AF2F4"
             << ::std::hex
             << GetCurrentProcessId()
             << &flag
             << ::std::ends;
        unfreezer unfreeze(strm);
#   if defined (BOOST_NO_ANSI_APIS)
        USES_CONVERSION;
        HANDLE mutex = CreateMutexW(NULL, FALSE, A2CW(strm.str()));
#   else
        HANDLE mutex = CreateMutexA(NULL, FALSE, strm.str());
#   endif
#else
#   if defined (BOOST_NO_ANSI_APIS)
        ::std::wostringstream strm;
        strm << L"2AC1A572DB6944B0A65C38C4140AF2F4"
             << ::std::hex
             << GetCurrentProcessId()
             << &flag;
        HANDLE mutex = CreateMutexW(NULL, FALSE, strm.str().c_str());
#   else
        ::std::ostringstream strm;
        strm << "2AC1A572DB6944B0A65C38C4140AF2F4"
             << ::std::hex
             << GetCurrentProcessId()
             << &flag;
        HANDLE mutex = CreateMutexA(NULL, FALSE, strm.str().c_str());
#   endif
#endif
        assert(mutex != NULL);

        int res = 0;
        res = WaitForSingleObject(mutex, INFINITE);
        assert(res == WAIT_OBJECT_0);

        if (compare_exchange(&flag, 1, 1) == 0)
        {
            try
            {
                func();
            }
            catch (...)
            {
                res = ReleaseMutex(mutex);
                assert(res);
                res = CloseHandle(mutex);
                assert(res);
                throw;
            }
            InterlockedExchange(&flag, 1);
        }

        res = ReleaseMutex(mutex);
        assert(res);
        res = CloseHandle(mutex);
        assert(res);
    }
#elif defined(BOOST_HAS_PTHREADS)
    pthread_once(&once, &key_init);
    pthread_setspecific(key, &func);
    pthread_once(&flag, do_once);
#elif defined(BOOST_HAS_MPTASKS)
    if(flag == false)
    {
        // all we do here is make a remote call to blue, as blue is not
        // reentrant.
        ::std::pair<void (*)(), once_flag *> sData(func, &flag);
        MPRemoteCall(remote_call_proxy, &sData, kMPOwningProcessRemoteContext);
        assert(flag == true);
    }
#endif
}

}
