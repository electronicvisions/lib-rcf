#include <iostream>
#include <iomanip> // std::setw
#include <sstream> // std::stringstream
#include <thread>
#include <vector>

typedef std::vector<int> intvec;


#include <chrono>
// convenience for std::chrono
namespace chronoz = std::chrono;
typedef chronoz::steady_clock       clockz;
typedef clockz::time_point          timepointz;
typedef clockz::duration            durationz;

typedef std::ratio<1>               ratio_identity;
typedef chronoz::duration<double,ratio_identity>
                                    duration_t;
// <--


#include <RCF/RcfServer.hpp>
#include <RCF/TcpEndpoint.hpp>
#include <RCF/Idl.hpp>
#include <RCF/InitDeinit.hpp>

#include <SF/vector.hpp>



// ----- Globals -------------------------------------------------------------------------------------------------

const int port = 50001;
const size_t max_message_length = 128 * 1024 * 1024; // 100 MiB plus 28 to be surely above max batch.

size_t bytes_intentional = 0; // is set at begining of main, configureable using argv[1], giving it KiBs 

#ifdef DEBUG
const bool enableTwoway = false; // they are very slow and anoying during debugging
#else
const bool enableTwoway = true;
#endif


// ----- Interface -----------------------------------------------------------------------------------------------

RCF_BEGIN(I_HelloWorld, "I_HelloWorld")
    RCF_METHOD_R0(size_t, reset)

    RCF_METHOD_V0(void, rcfcall)
    RCF_METHOD_V1(void, swallow, const int &)
    RCF_METHOD_V1(void, swallow, const intvec &)
RCF_END(I_HelloWorld)

//    RCF_METHOD_V1(void, swallow, const & std::vector<DummyPulse>)
//    RCF_METHOD_V1(void, swallow, const & DummyPulsePacket)




// ----- Implementation ------------------------------------------------------------------------------------------

class HelloWorldImpl
{
public:
    int total       = 0;
    size_t callse   = 0;

    void rcfcall() {
        callse++;
    }

    void swallow(const int & i)
    {
        total +=i; // just against optimization..
        callse++;

        #ifdef DEBUG0
        std::cout << "I_HelloWorld service: " << i << std::endl;
        #endif
    }
    
    void swallow(const intvec & v)
    {
        for (int i : v) total += i; // just against optimization..
        callse++;

        #ifdef DEBUG0
        assert(v.size() == 256);
        int prps = (int) callse - 1;
        for (int i=0; i < 256; i++) {
            assert( v[i] == (prps + i) ); // just against optimization..
        }

        std::cout << "I_HelloWorld service: " << v.size() << std::endl;
        #endif
    }


    size_t reset() {
        size_t r = callse;
        callse = 0;
        return r;
    }
};



// ----- TestFunc ------------------------------------------------------------------------------------------------

class SimpleTest {
    static const int w_title    = 20;
    static const int w_xferC    = 12;
    static const int w_xferB    = 8;
    static const int w_float    = 12;

    // members

    public:
    std::string name;
    size_t szTransferObject;

    size_t nbTransfers  = 0;
    size_t bytes        = 0;


    // result values:
    timepointz begin;
    timepointz end;

    duration_t duration;


    // constructor
    SimpleTest(const std::string & _name, size_t _szTransferObject) : name (_name), szTransferObject(_szTransferObject)
    {
        assert ( name.length() < (size_t)w_title );

        nbTransfers = (bytes_intentional / _szTransferObject);
        bytes       = (nbTransfers * szTransferObject); 
    }


    // reuse..
    void beginTest() {
        std::cout << std::left << std::setw(w_title) << name << std::right << std::flush;
        
        std::this_thread::yield();
        begin = clockz::now();
    }

    void endTest(RcfClient<I_HelloWorld> & client) {
        // assure that all previous calls have finished..
        size_t r = client.reset(RCF::Twoway);
        // stop time
        stopTest(); // now we're really done..

        // this is the least which must be true
        assert( r == nbTransfers );
        outputTestResults();
    }

    void outputTestResults() {
        std::cout   << std::setw(w_xferC) << nbTransfers 
                    << std::setw(w_xferB) << szTransferObject
                    << std::setw(w_float) << duration.count()
                    << std::setw(w_float) << double(nbTransfers * szTransferObject) / duration.count()
                    << std::setw(w_float) << double(nbTransfers) / duration.count()
                    << std::setw(w_float) << duration.count() / double(nbTransfers * szTransferObject)
                    << std::setw(w_float) << duration.count() / double(nbTransfers)
                    << std::endl;
    }

    static void outputTitle() {
        std::cout   << std::left 
                    << std::setw(w_title) << "# TestName"
                    << std::right
                    << std::setw(w_xferC) << "Transfers"
                    << std::setw(w_xferB) << "b/xfer"
                    << std::setw(w_float) << "Duration"
                    << std::setw(w_float) << "Throughput"
                    << std::setw(w_float) << "Calls/sec"
                    << std::setw(w_float) << "sec/Byte"
                    << std::setw(w_float) << "sec/Call"
                    << std::endl;
    }

    private: 
    void stopTest() {
        end = clockz::now();

        durationz durZ = end - begin;
        duration = chronoz::duration_cast<duration_t>(durZ);
    }


};



// ----- Executable ----------------------------------------------------------------------------------------------

int main(int argc, char *argv[]) 
{
    assert (bytes_intentional == 0); // global parameter

    if (argc > 1) {
        bytes_intentional = atoi(argv[1]) * 1024; // enter -1 to set maximum!
        assert (bytes_intentional > 1023 && "Bad argv[1], minimum 1"); // at least one KiB!
    } else {
        std::cout << "# Information: default test packet size chosen: 32 KiB" << std::endl;
        std::cout << "               you can specify that size in KiBs as argv[1]." << std::endl;
        bytes_intentional = 32 * 1024; // 32 KiB
    }
    const size_t bytes_iKiB         = bytes_intentional / 1024;

    std::cout.precision(3);                             // number of decimals
    std::cout.setf(std::ios::scientific, std::ios::floatfield);   // floatfield is either fixed or scientific.
    //if (showpos) out.setf(ios::showpos);              // show positive



    // ----- setup server ----------------------------------------------------------------------------------------
    RCF::RcfInitDeinit rcfInit;

    HelloWorldImpl helloWorld;

    RCF::RcfServer server( (RCF::TcpEndpoint(port)) );
    server.getServerTransport().setMaxMessageLength( max_message_length );

    server.bind<I_HelloWorld>(helloWorld);

    server.start();
    std::cout << "# Server started!" << std::endl;



    // ----- setup client ----------------------------------------------------------------------------------------
    RcfClient<I_HelloWorld> client( (RCF::TcpEndpoint( port)) );
    client.getClientStub().getTransport().setMaxMessageLength( max_message_length );
    client.getClientStub().connect(); // connect explicitly

    { // ping
    timepointz c_begin = clockz::now();
    client.getClientStub().ping();
    timepointz c_end = clockz::now();
    
    durationz c_dura = c_end - c_begin;
    auto duration = chronoz::duration_cast<duration_t>(c_dura);

    std::cout << "# Client started! Ping: " << duration.count() << "s." << std::endl;
    }


    if (true) { // multiping (you may watch ifstat -l)
        #ifdef DEBUG
        int multiping = 1000;
        #else
        int multiping = 100 * 1000;
        #endif

        timepointz c_begin = clockz::now();
        for (int i=0; i < multiping; i++) {
            client.getClientStub().ping();
        }
        timepointz c_end = clockz::now();

        durationz c_dura = c_end - c_begin;
        auto duration = chronoz::duration_cast<duration_t>(c_dura);
        std::cout << "# Pingtest, PingAvg: " << duration.count()/(double(multiping)) << "s." << std::endl;
        std::cout << "# PingCalls/second : " << (double(multiping))/duration.count() << "s." << std::endl;
    }


    // ----- testing phase ---------------------------------------------------------------------------------------
    std::cout << "\n# Looping the swallow: " << bytes_intentional << ", " << bytes_iKiB << " KiB." << std::endl;
    

    // ----- call speed ------------------------------------------------------------------------------------------
    std::cout << "\n# Testing empty calls/ call speed (no data transferred, anyway one call handled as one byte)." << std::endl;
    SimpleTest::outputTitle();

    if (enableTwoway) {
        SimpleTest a("TwowayCall", 1); // setting one byte (=> Throughput == Calls/sec)
        a.beginTest();
        for (size_t i=0; i < a.nbTransfers; i++) {
            client.rcfcall();
        }
        a.endTest(client);
    }
    
    if (true) {
        SimpleTest a("OnewayCall", 1);
        a.beginTest();
        for (size_t i=0; i < a.nbTransfers; i++) {
            client.rcfcall(RCF::Oneway);
        }
        a.endTest(client);
    }
    
    // call batched
    for (int i = 1; i < 1001; i*=10) 
    {
        std::stringstream s;
        s << "BatchedCall_" << i << "KiB";
        size_t batchSz = i * 1024;

        SimpleTest a(s.str(), 1);
        client.getClientStub().enableBatching();
        client.getClientStub().setMaxBatchMessageLength( batchSz );
        //client.getClientStub().setRemoteCallSemantics(RCF::Oneway); // needs some include?

        a.beginTest();
        for (size_t i=0; i < a.nbTransfers; i++) {
            client.rcfcall(RCF::Oneway);
        }
        client.getClientStub().flushBatch();

        // disabling batching as otherwise subsequent twoway calls will fail
        client.getClientStub().disableBatching();
        a.endTest(client); // this includes a twoway call to sync with the server
    }



    // ----- single ints -----------------------------------------------------------------------------------------
    std::cout << "\n# Testing single int calls, bytes/call: " << sizeof(int) << "." << std::endl;
    SimpleTest::outputTitle();
    

    if (enableTwoway) {
        SimpleTest a("TwowaySgl", sizeof(int));
        a.beginTest();
        for (size_t i=0; i < a.nbTransfers; i++) {
            client.swallow(i);
        }
        a.endTest(client);
    }

    if (true) {
        SimpleTest a("OnewaySgl", sizeof(int));
        a.beginTest();
        for (size_t i=0; i < a.nbTransfers; i++) {
            client.swallow(RCF::Oneway, i);
        }
        a.endTest(client);
    }


    // batching
    for (int i = 1; i < 1001; i*=10) 
    {
        std::stringstream s;
        s << "BatchedSgl_" << i << "KiB";
        size_t batchSz = i * 1024;

        SimpleTest a(s.str(), sizeof(int));
        client.getClientStub().enableBatching();
        client.getClientStub().setMaxBatchMessageLength( batchSz );
        //client.getClientStub().setRemoteCallSemantics(RCF::Oneway); // needs some include?

        a.beginTest();
        for (size_t i=0; i < a.nbTransfers; i++) {
            client.swallow(RCF::Oneway, i);
        }
        client.getClientStub().flushBatch();

        // disabling batching as otherwise subsequent twoway calls will fail
        client.getClientStub().disableBatching();
        a.endTest(client); // this includes a twoway call to sync with the server
    }



    // ----- int vectors -----------------------------------------------------------------------------------------
    // as the vector speed is far higher we increase the bytes_intentional here..
    bytes_intentional *= 1024;

    std::cout << "\n# Looping the vector: " << bytes_intentional << ", " << (bytes_intentional / (1024*1024)) << " MiB." << std::endl;


    for (int vi = 1; vi < 101; vi*=10) {  
    int vecsize = 256 * vi; // equals 1 to 100 KiB in bytes (assuming 4 byte ints)
    size_t objsize = vecsize * sizeof(int);
    if (objsize > bytes_intentional) break;
    std::cout << "\n# Testing int[" << vecsize << "] vectors, bytes/call: " << objsize << "." << std::endl;
    SimpleTest::outputTitle();
    
    // in the following tests an empty vector is initialized during test time and then reused for each transfer
    // this will not work when the vectors size is not the same all the time..
    // However in sender and receiver each vectors element is changed/read to have a fair comparision to the
    // single int swallow function!

    if (enableTwoway) {
        SimpleTest a("TwowayVec", objsize);
        a.beginTest();
        intvec v(vecsize);
        for (size_t i=0; i < a.nbTransfers; i++) {
            for (int inner=0; inner < vecsize; inner++) v[inner] = (i + inner); //v.at(inner) = (i + inner);
            client.swallow(v);
        }
        a.endTest(client);
    }

    if (true) {
        SimpleTest a("OnewayVec", objsize);
        a.beginTest();
        intvec v(vecsize);
        for (size_t i=0; i < a.nbTransfers; i++) {
            for (int inner=0; inner < vecsize; inner++) v[inner] = (i + inner); //v.at(inner) = (i + inner);
            client.swallow(RCF::Oneway, v);
        }
        a.endTest(client);
    }


    // batching MiB! (KiB batching is useless with vecs greater than that!)
    for (int i = 1; i < 101; i*=10) 
    {
        size_t batchSz = i * 1024 * 1024; // 1 to 100 MiBs (maxmesgl == 101 MiB)
        bool lastBatch = false;
        if ( (batchSz ) > bytes_intentional ) // this tests size is bigger than data.. no more needed..
            lastBatch = true;

        std::stringstream s;
        s << "BatchedVec_" << i << "MiB";

        SimpleTest a(s.str(), objsize);
        client.getClientStub().enableBatching();
        client.getClientStub().setMaxBatchMessageLength( batchSz );
        //client.getClientStub().setRemoteCallSemantics(RCF::Oneway); // needs some include?

        a.beginTest();
        intvec v(vecsize);
        for (size_t i=0; i < a.nbTransfers; i++) {
            for (int inner=0; inner < vecsize; inner++) v[inner] = (i + inner); //v.at(inner) = (i + inner);
            client.swallow(RCF::Oneway, v);
        }
        client.getClientStub().flushBatch();

        // disabling batching as otherwise subsequent twoway calls will fail
        client.getClientStub().disableBatching();
        a.endTest(client); // this includes a twoway call to sync with the server

        if (lastBatch) break; // it breakes the inner?
    }
    } // for vi (not indented)



    // ----- saying goodbye --------------------------------------------------------------------------------------
    std::cout << "\n\n# Saying Goodbye with a funny number: " << helloWorld.total << std::endl;
    
    return 0;
}
