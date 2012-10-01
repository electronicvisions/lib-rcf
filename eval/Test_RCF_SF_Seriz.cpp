#include <iostream>
#include <iomanip> // std::setw
#include <sstream> // std::stringstream


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



// ----- Interface -----------------------------------------------------------------------------------------------

RCF_BEGIN(I_HelloWorld, "I_HelloWorld")
    RCF_METHOD_V1(void, swallow, const int &)
RCF_END(I_HelloWorld)

//    RCF_METHOD_V1(void, swallow, const & std::vector<int>)
//    RCF_METHOD_V1(void, swallow, const & std::vector<DummyPulse>)
//    RCF_METHOD_V1(void, swallow, const & DummyPulsePacket)



// ----- Implementation ------------------------------------------------------------------------------------------

class HelloWorldImpl
{
public:
    int total = 0;

    void swallow(const int & i)
    {
        //total +=i; // just against optimization..

        #ifdef DEBUG0
        std::cout << "I_HelloWorld service: " << i << std::endl;
        #endif
    }
};



// ----- Globals -------------------------------------------------------------------------------------------------

#ifdef DEBUG
const size_t bytes_intentional  =       500 * 1024; // (500 KiB) // about 1 min/MiB
#else
const size_t bytes_intentional  = 50 * 1024 * 1024; // (50 MiB)
#endif
const size_t bytes_iKiB         = bytes_intentional / 1024;



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

        begin = clockz::now();
    }

    void endTest() {
        end = clockz::now();

        durationz durZ = end - begin;
        duration = chronoz::duration_cast<duration_t>(durZ);

        std::cout   << std::setw(w_xferC) << nbTransfers 
                    << std::setw(w_xferB) << szTransferObject
                    << std::setw(w_float) << duration.count()
                    << std::setw(w_float) << double(nbTransfers * szTransferObject) / duration.count()
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
                    << std::endl;
    }

};



// ----- Executable ----------------------------------------------------------------------------------------------

int main()
{
    std::cout.precision(3);                             // number of decimals
    std::cout.setf(std::ios::scientific, std::ios::floatfield);   // floatfield is either fixed or scientific.
    //if (showpos) out.setf(ios::showpos);              // show positive



    // ----- setup server ----------------------------------------------------------------------------------------
    RCF::RcfInitDeinit rcfInit;

    HelloWorldImpl helloWorld;

    RCF::RcfServer server( RCF::TcpEndpoint(50001) );

    server.bind<I_HelloWorld>(helloWorld);

    server.start();
    std::cout << "# Server started!" << std::endl;



    // ----- setup client ----------------------------------------------------------------------------------------
    RcfClient<I_HelloWorld> client( RCF::TcpEndpoint(50001) );
    client.getClientStub().connect(); // connect explicitly


    { // ping
    timepointz c_begin = clockz::now();
    client.getClientStub().ping();
    timepointz c_end = clockz::now();
    
    durationz c_dura = c_end - c_begin;
    auto duration = chronoz::duration_cast<duration_t>(c_dura);

    std::cout << "# Client started! Ping: " << duration.count() << "s." << std::endl;
    }



    // ----- testing phase ---------------------------------------------------------------------------------------
    std::cout << "# Looping the swallow: " << bytes_intentional << ", " << bytes_iKiB << " KiB." << std::endl;
    SimpleTest::outputTitle();


    {
        SimpleTest a("TwowaySgl", sizeof(int));
        a.beginTest();
        for (size_t i=0; i < a.nbTransfers; i++) {
            client.swallow(i);
        }
        a.endTest();
    }

    {
        SimpleTest a("OnewaySgl", sizeof(int));
        a.beginTest();
        for (size_t i=0; i < a.nbTransfers; i++) {
            client.swallow(RCF::Oneway, i);
        }
        a.endTest();
    }

    // batching
    for (int i = 1; i < 101; i*=10) 
    {
        std::stringstream s;
        s << "BatchedSgl_" << i << "KiB";
        size_t batchSz = i * 1024;

        SimpleTest a(s.str(), sizeof(int));
        client.getClientStub().enableBatching();
        client.getClientStub().setMaxBatchMessageLength( batchSz );

        a.beginTest();
        for (size_t i=0; i < a.nbTransfers; i++) {
            client.swallow(RCF::Oneway, i);
        }
        a.endTest();
        client.getClientStub().disableBatching();
    }



    // ----- saying goodbye --------------------------------------------------------------------------------------
    std::cout << "\n\n# Saying Goodbye with a funny number: " << helloWorld.total << std::endl;
    
    return 0;
}
