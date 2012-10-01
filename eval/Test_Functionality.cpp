#include <iostream>

#include <RCF/RcfServer.hpp>
#include <RCF/TcpEndpoint.hpp>
#include <RCF/Idl.hpp>
#include <RCF/InitDeinit.hpp>

//#include <RCF/RCF.hpp>

RCF_BEGIN(I_HelloWorld, "I_HelloWorld")
    RCF_METHOD_V1(void, Print, const std::string &)
RCF_END(I_HelloWorld)

class HelloWorldImpl
{
public:
    void Print(const std::string & s)
    {
        std::cout << "I_HelloWorld service: " << s << std::endl;
    }
};


int main()
{
    RCF::RcfInitDeinit rcfInit;

    HelloWorldImpl helloWorld;
    RCF::RcfServer server( RCF::TcpEndpoint(50001) );

    server.bind<I_HelloWorld>(helloWorld);

    server.start();

    std::cout << "Calling the I_HelloWorld Print() method." << std::endl;

    RcfClient<I_HelloWorld> client( RCF::TcpEndpoint(50001) );
    client.Print("Hello World");
    
    return 0;
}
