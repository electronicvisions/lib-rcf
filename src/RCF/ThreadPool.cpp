
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

#include <RCF/ThreadPool.hpp>

#include <RCF/Exception.hpp>
#include <RCF/InitDeinit.hpp>
#include <RCF/ThreadLocalData.hpp>

#ifdef BOOST_WINDOWS
#include <RCF/Iocp.hpp>
#endif

#ifdef RCF_USE_BOOST_ASIO
#include <RCF/AsioDeadlineTimer.hpp>
#include <RCF/AsioServerTransport.hpp>
#include <boost/asio/io_service.hpp>
#endif

namespace RCF {

#ifdef RCF_USE_BOOST_ASIO

    class AsioMuxer;
    typedef boost::shared_ptr<AsioMuxer> AsioMuxerPtr;
    typedef boost::weak_ptr<AsioMuxer> AsioMuxerWeakPtr;

    class TimeoutHandler
    {
    public:
        TimeoutHandler(AsioMuxerWeakPtr asioMuxerWeakPtr);
        void operator()(boost::system::error_code ec);
        AsioMuxerWeakPtr mAsioMuxerWeakPtr;
    };

    class DummyHandler
    {
    public:
        void operator()() {}
    };

    // Custom handler allocation, to avoid heap allocation.
    void * asio_handler_allocate(std::size_t size, TimeoutHandler * pHandler);
    void asio_handler_deallocate(void * pointer, std::size_t size, TimeoutHandler * pHandler);
    void * asio_handler_allocate(std::size_t size, DummyHandler * pHandler);
    void asio_handler_deallocate(void * pointer, std::size_t size, DummyHandler * pHandler);

    class AsioMuxer : public boost::enable_shared_from_this<AsioMuxer>
    {
    public:
        AsioMuxer() : 
            mIoService(), 
            mCycleTimer(mIoService)
        {
            mIoService.reset();
        }

        ~AsioMuxer()
        {
            mCycleTimer.mImpl.cancel();
        }

        void startTimer()
        {
            mCycleTimer.mImpl.expires_from_now(
                boost::posix_time::milliseconds(1000));

            AsioMuxerWeakPtr thisWeakPtr = shared_from_this();
            mCycleTimer.mImpl.async_wait( TimeoutHandler(thisWeakPtr) );
        }

        void cycle(int timeoutMs)
        {
            RCF_ASSERT_GTEQ(timeoutMs , -1);

            mIoService.run_one();
        }

        void stopCycle()
        {
            mIoService.stop();
        }

        static void onTimer(
            AsioMuxerWeakPtr thisWeakPtr, 
            const boost::system::error_code& error)
        {
            AsioMuxerPtr thisPtr = thisWeakPtr.lock();

            if (!error)
            {
                ThreadInfoPtr threadInfoPtr = getThreadInfoPtr();
                if (threadInfoPtr)
                {
                    ThreadPool & threadPool = threadInfoPtr->getThreadPool();
                    std::size_t threadCount = threadPool.getThreadCount();
                    RCF_ASSERT(threadCount >= 1);
                    for (std::size_t i=0; i<threadCount-1; ++i)
                    {
                        //thisPtr->mIoService.post( &dummyHandler );
                        thisPtr->mIoService.post( DummyHandler() );
                    }
                }

                thisPtr->mCycleTimer.mImpl.expires_from_now(
                    boost::posix_time::milliseconds(1000));

                thisPtr->mCycleTimer.mImpl.async_wait(TimeoutHandler(thisWeakPtr) );
            }
        }

        AsioIoService mIoService;
        AsioDeadlineTimer mCycleTimer;
    };

    class HandlerCache
    {
    public:
        typedef boost::shared_ptr< std::vector<char> > VecPtr;
        Mutex mHandlerMutex;
        std::vector<VecPtr> mHandlerFreeList;
        std::vector<VecPtr> mHandlerUsedList;
    };

    HandlerCache * gpTimeoutHandlerCache = NULL;
    HandlerCache * gpDummyHandlerCache = NULL;

    void initHandlerCache()
    {
        gpTimeoutHandlerCache = new HandlerCache(); 
        gpDummyHandlerCache = new HandlerCache();
    }

    void deinitHandlerCache()
    {
        delete gpTimeoutHandlerCache; 
        gpTimeoutHandlerCache = NULL; 
        
        delete gpDummyHandlerCache; 
        gpDummyHandlerCache = NULL;
    }

    RCF_ON_INIT_DEINIT_NAMED(
        initHandlerCache(); ,
        deinitHandlerCache(); ,
        HandlerCacheInit )

    TimeoutHandler::TimeoutHandler(AsioMuxerWeakPtr asioMuxerWeakPtr) : 
        mAsioMuxerWeakPtr(asioMuxerWeakPtr)
    {
    }

    void TimeoutHandler::operator()(boost::system::error_code ec)
    {
        AsioMuxer::onTimer(mAsioMuxerWeakPtr, ec);
    }

    void * asioHandlerAllocate(std::size_t size, HandlerCache * pHandlerCache)
    {
        HandlerCache::VecPtr vecPtr;
        Lock lock(pHandlerCache->mHandlerMutex);
        if (pHandlerCache->mHandlerFreeList.empty())
        {
            vecPtr.reset( new std::vector<char>(size) );
        }
        else
        {
            vecPtr = pHandlerCache->mHandlerFreeList.back();
            pHandlerCache->mHandlerFreeList.pop_back();
        }

        pHandlerCache->mHandlerUsedList.push_back(vecPtr);
        return & (*vecPtr)[0];
    }

    void asioHandlerDeallocate(void * pointer, std::size_t size, HandlerCache * pHandlerCache)
    {
        Lock lock(pHandlerCache->mHandlerMutex);
        for (std::size_t i=0; i<pHandlerCache->mHandlerUsedList.size(); ++i)
        {
            HandlerCache::VecPtr vecPtr = pHandlerCache->mHandlerUsedList[i];
            std::vector<char> & vec  = *vecPtr;
            if ( & vec[0]  == pointer )
            {
                pHandlerCache->mHandlerUsedList.erase( pHandlerCache->mHandlerUsedList.begin() + i);
                pHandlerCache->mHandlerFreeList.push_back(vecPtr);
            }
        }
    }

    void * asio_handler_allocate(std::size_t size, TimeoutHandler * pHandler)
    {
        return asioHandlerAllocate(size, gpTimeoutHandlerCache);
    }

    void asio_handler_deallocate(void * pointer, std::size_t size, TimeoutHandler * pHandler)
    {
        asioHandlerDeallocate(pointer, size, gpTimeoutHandlerCache);
    }

    void * asio_handler_allocate(std::size_t size, DummyHandler * pHandler)
    {
        return asioHandlerAllocate(size, gpDummyHandlerCache);
    }

    void asio_handler_deallocate(void * pointer, std::size_t size, DummyHandler * pHandler)
    {
        asioHandlerDeallocate(pointer, size, gpDummyHandlerCache);
    }

#endif

    // ThreadPool

    void ThreadPool::setThreadName(const std::string &threadName)
    {
        Lock lock(mInitDeinitMutex);
        mThreadName = threadName;
    }

    std::string ThreadPool::getThreadName()
    {
        Lock lock(mInitDeinitMutex);
        return mThreadName;
    }

#if defined(BOOST_WINDOWS) && !defined (__MINGW32__)

    typedef struct tagTHREADNAME_INFO
    {
        DWORD dwType; // must be 0x1000
        LPCSTR szName; // pointer to name (in user addr space)
        DWORD dwThreadID; // thread ID (-1=caller thread)
        DWORD dwFlags; // reserved for future use, must be zero
    } THREADNAME_INFO;

    // 32 character limit on szThreadName apparently, or it gets truncated.
    void setWin32ThreadName(boost::uint32_t dwThreadID, const char * szThreadName)
    {
        THREADNAME_INFO info;
        info.dwType = 0x1000;
        info.szName = szThreadName;
        info.dwThreadID = dwThreadID;
        info.dwFlags = 0;

        __try
        {
            RaiseException( 0x406D1388, 0, sizeof(info)/sizeof(DWORD), (ULONG_PTR*)&info );
        }
        __except(EXCEPTION_CONTINUE_EXECUTION)
        {
        }
    }

    void ThreadPool::setMyThreadName()
    {
        std::string threadName = getThreadName();
        if (!threadName.empty())
        {
            setWin32ThreadName( DWORD(-1), threadName.c_str());
        }
    }

#else

    void setWin32ThreadName(boost::uint32_t dwThreadID, const char * szThreadName)
    {
    }

    void ThreadPool::setMyThreadName()
    {
    }

#endif

    void ThreadPool::onInit()
    {
        std::vector<ThreadInitFunctor> initFunctors;
        {
            Lock lock(mInitDeinitMutex);
            std::copy(
                mThreadInitFunctors.begin(), 
                mThreadInitFunctors.end(), 
                std::back_inserter(initFunctors));
        }

        std::for_each(
            initFunctors.begin(), 
            initFunctors.end(), 
            boost::bind(&ThreadInitFunctor::operator(), _1));
    }

    void ThreadPool::onDeinit()
    {
        std::vector<ThreadDeinitFunctor> deinitFunctors;
        {
            Lock lock(mInitDeinitMutex);
            std::copy(
                mThreadDeinitFunctors.begin(), 
                mThreadDeinitFunctors.end(), 
                std::back_inserter(deinitFunctors));
        }

        std::for_each(
            deinitFunctors.begin(), 
            deinitFunctors.end(), 
            boost::bind(&ThreadDeinitFunctor::operator(), _1));
    }

    void ThreadPool::addThreadInitFunctor(ThreadInitFunctor threadInitFunctor)
    {
        Lock lock(mInitDeinitMutex);
        mThreadInitFunctors.push_back(threadInitFunctor);
    }

    void ThreadPool::addThreadDeinitFunctor(ThreadDeinitFunctor threadDeinitFunctor)
    {
        Lock lock(mInitDeinitMutex);
        mThreadDeinitFunctors.push_back(threadDeinitFunctor);
    }

    Iocp * ThreadPool::getIocp()
    {
        return mIocpPtr.get();
    }

#ifdef RCF_USE_BOOST_ASIO

    AsioIoService * ThreadPool::getIoService()
    {
        return & mAsioMuxerPtr->mIoService;
    }

#else

    AsioIoService * ThreadPool::getIoService()
    {
        return NULL;
    }

#endif

    void ThreadPool::enableMuxerType(MuxerType muxerType)
    {

#ifdef BOOST_WINDOWS
        if (muxerType == Mt_Iocp && !mIocpPtr)
        {
            mIocpPtr.reset( new Iocp() );
        }
#endif

#ifdef RCF_USE_BOOST_ASIO
        if (muxerType == Mt_Asio && !mAsioMuxerPtr)
        {
            mAsioMuxerPtr.reset( new AsioMuxer() );
            mAsioMuxerPtr->startTimer();
        }
#endif

    }

    void ThreadPool::resetMuxers()
    {
        mIocpPtr.reset();
        mAsioMuxerPtr.reset();
    }

    ThreadPool::ThreadPool(
        std::size_t threadCount,
        const std::string & threadName) :
            mThreadName(threadName),
            mStarted(RCF_DEFAULT_INIT),
            mThreadTargetCount(threadCount),
            mThreadMaxCount(threadCount),
            mReserveLastThread(false),
            mThreadIdleTimeoutMs(30*1000),
            mpUserStopFlag(RCF_DEFAULT_INIT),
            mBusyCount(RCF_DEFAULT_INIT)
    {
    }

    ThreadPool::ThreadPool(
        std::size_t threadTargetCount,
        std::size_t threadMaxCount,
        const std::string & threadName,
        boost::uint32_t threadIdleTimeoutMs,
        bool reserveLastThread) :
            mThreadName(threadName),
            mStarted(RCF_DEFAULT_INIT),
            mThreadTargetCount(threadTargetCount),
            mThreadMaxCount(threadMaxCount),
            mReserveLastThread(reserveLastThread),
            mThreadIdleTimeoutMs(threadIdleTimeoutMs),
            mpUserStopFlag(RCF_DEFAULT_INIT),
            mBusyCount(RCF_DEFAULT_INIT)
    {
        RCF_ASSERT(
            0 < mThreadTargetCount && mThreadTargetCount <= mThreadMaxCount)
            (mThreadTargetCount)(mThreadMaxCount);
    }

    ThreadPool::~ThreadPool()
    {
        RCF_DTOR_BEGIN
            stop();
        RCF_DTOR_END
    }

    // Not synchronized - caller must hold lock on mThreadsMutex.
    bool ThreadPool::launchThread(const volatile bool &userStopFlag)
    {
        RCF_ASSERT_LTEQ(mThreads.size() , mThreadMaxCount);

        if (mThreads.size() == mThreadMaxCount)
        {
            // We've hit the max thread limit.
            return false;
        }
        else
        {
            ThreadInfoPtr threadInfoPtr( new ThreadInfo(*this));

            ThreadPtr threadPtr( new Thread(
                boost::bind(
                    &ThreadPool::repeatTask,
                    this,
                    threadInfoPtr,
                    1000,
                    boost::ref(userStopFlag))));

            RCF_ASSERT(mThreads.find(threadInfoPtr) == mThreads.end());

            mThreads[threadInfoPtr] = threadPtr;

            return true;
        }
    }

    void ThreadPool::notifyBusy()
    {
        if (!getThreadInfoPtr()->mBusy)
        {
            getThreadInfoPtr()->mBusy = true;

            Lock lock(mThreadsMutex);

            ++mBusyCount;
            RCF_ASSERT_LTEQ(0 , mBusyCount);
            RCF_ASSERT_LTEQ(mBusyCount , mThreads.size());

            if (! (*mpUserStopFlag))
            {
                if (mBusyCount == mThreads.size())
                {
                    bool launchedOk = launchThread(*mpUserStopFlag);                    
                    if (!launchedOk && mReserveLastThread)
                    {
                        Exception e(_RcfError_AllThreadsBusy());
                        RCF_THROW(e);
                    }
                }
            }
        }
    }

    void ThreadPool::notifyReady()
    {
        ThreadInfoPtr threadInfoPtr = getThreadInfoPtr();

        if (threadInfoPtr->mBusy)
        {
            threadInfoPtr->mBusy = false;

            Lock lock(mThreadsMutex);
            
            --mBusyCount;            
            
            RCF_ASSERT_LTEQ(0 , mBusyCount);
            RCF_ASSERT_LTEQ(mBusyCount , mThreads.size());
        }

        // Has this thread been idle? The touch timer is reset when the thread
        // does any work.
        if (threadInfoPtr->mTouchTimer.elapsed(mThreadIdleTimeoutMs))
        {
            // If we have more than our target count of threads running, and
            // if at least two of the threads are not busy, then let this thread
            // exit.

            Lock lock(mThreadsMutex);

            if (    mThreads.size() > mThreadTargetCount 
                &&  mBusyCount < mThreads.size() - 1)
            {                
                threadInfoPtr->mStopFlag = true; 

                RCF_ASSERT( mThreads.find(threadInfoPtr) != mThreads.end() );
                mThreads.erase( mThreads.find(threadInfoPtr) );
            }
        }
    }

    ShouldStop::ShouldStop(
        const volatile bool & stopFlag, 
        ThreadInfoPtr threadInfoPtr) :
            mStopFlag(stopFlag),
            mTaskFlag(false),
            mThreadInfoPtr(threadInfoPtr)
    {
    }

    bool ShouldStop::operator()() const
    {
        return 
                mStopFlag 
            ||  mTaskFlag 
            ||  (mThreadInfoPtr.get() && mThreadInfoPtr->mStopFlag);
    }

    void ThreadPool::cycle(int timeoutMs, ShouldStop & shouldStop)
    {

#ifdef BOOST_WINDOWS
        if (mIocpPtr.get() && !shouldStop())
        {
            mIocpPtr->cycle(timeoutMs);
        }
#endif

#ifdef RCF_USE_BOOST_ASIO
        if (mAsioMuxerPtr.get() && !shouldStop())
        {
            mAsioMuxerPtr->cycle(timeoutMs);
        }
#endif

        if ( (mTask ? true : false) && !shouldStop())
        {
            shouldStop.mTaskFlag = mTask(timeoutMs, shouldStop.mStopFlag, false);
        }
    }

    void ThreadPool::repeatTask(
        RCF::ThreadInfoPtr threadInfoPtr,
        int timeoutMs,
        const volatile bool &stopFlag)
    {
        setThreadInfoPtr(threadInfoPtr);

        setMyThreadName();

        onInit();

        ShouldStop shouldStop(stopFlag, threadInfoPtr);
        while (!shouldStop())
        {
            try
            {
                while (!shouldStop())
                {
                    cycle(timeoutMs, shouldStop);
                    notifyReady();
                }
            }
            catch(const std::exception &e)
            {
                RCF_LOG_1()(e)(mThreadName) << "Thread pool: std::exception caught at top level."; 
            }
            catch(...)
            {
                RCF_LOG_1()(mThreadName) << "Thread pool: Unknown exception (...) caught at top level."; 
            }
        }

        onDeinit();

        {
            Lock lock(mThreadsMutex);
            ThreadMap::iterator iter = mThreads.find(threadInfoPtr);
            if (iter != mThreads.end())
            {
                mThreads.erase(iter);
            }            
        }

        RCF_LOG_2()(stopFlag)(getThreadName()) << "ThreadPool - thread terminating.";
    }

    // not synchronized
    void ThreadPool::start(const volatile bool &stopFlag)
    {
        if (!mStarted)
        {
            Lock lock(mThreadsMutex);

            RCF_ASSERT(mThreads.empty())(mThreads.size());
            mThreads.clear();
            mBusyCount = 0;
            mpUserStopFlag = &stopFlag;

            for (std::size_t i=0; i<mThreadTargetCount; ++i)
            {
                bool ok = launchThread(stopFlag);
                RCF_ASSERT(ok);
            }

            mStarted = true;
        }
    }

    void ThreadPool::stop(bool wait)
    {
        if (mStarted)
        {

            ThreadMap threads;
            {
                Lock lock(mThreadsMutex);
                threads = mThreads;
            }

            ThreadMap::iterator iter;
            for (
                iter = threads.begin(); 
                iter != threads.end(); 
                ++iter)
            {
                if (mStopFunctor)
                {
                    mStopFunctor();
                }

#ifdef RCF_USE_BOOST_ASIO
                if (mAsioMuxerPtr)
                {
                    mAsioMuxerPtr->stopCycle();
                }
#endif

                if (wait)
                {
                    iter->second->join();
                }
            }

            if (wait)
            {
                RCF_ASSERT( mThreads.empty() );
                mThreads.clear();
                mStarted = false;
            }
        }
    }

    bool ThreadPool::isStarted()
    {
        return mStarted;
    }

    void ThreadPool::setTask(Task task)
    {
        RCF_ASSERT(!mStarted);
        mTask = task;
    }

    void ThreadPool::setStopFunctor(StopFunctor stopFunctor)
    {
        RCF_ASSERT(!mStarted);
        mStopFunctor = stopFunctor;
    }

    std::size_t ThreadPool::getThreadCount()
    {
        Lock lock(mThreadsMutex);
        return mThreads.size();
    }

    ThreadTouchGuard::ThreadTouchGuard() : 
        mThreadInfoPtr(getThreadInfoPtr())
    {
        if (mThreadInfoPtr)
        {
            mThreadInfoPtr->touch();
        }
    }

    ThreadTouchGuard::~ThreadTouchGuard()
    {
        if (mThreadInfoPtr)
        {
            mThreadInfoPtr->touch();
        }
    }

    ThreadInfo::ThreadInfo(ThreadPool & threadPool) :
        mThreadPool(threadPool),
        mBusy(RCF_DEFAULT_INIT),
        mStopFlag(RCF_DEFAULT_INIT),
        mTouchTimer(0)
    {}

    void ThreadInfo::touch()
    {
        mTouchTimer.restart();
    }

    void ThreadInfo::notifyBusy()
    {
        touch();
        mThreadPool.notifyBusy();
    }

    ThreadPool & ThreadInfo::getThreadPool()
    {
        return mThreadPool;
    }

} // namespace RCF
