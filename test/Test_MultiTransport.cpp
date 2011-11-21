
#include <string>

#include <RCF/test/TestMinimal.hpp>

#include <RCF/Idl.hpp>
#include <RCF/RcfServer.hpp>
#include <RCF/ThreadLibrary.hpp>
#include <RCF/test/TransportFactories.hpp>
#include <RCF/test/ThreadGroup.hpp>
#include <RCF/util/CommandLine.hpp>

#include <RCF/TcpEndpoint.hpp>

#if defined(BOOST_WINDOWS) || defined(BOOST_ASIO_HAS_LOCAL_SOCKETS)
#include <RCF/NamedPipeEndpoint.hpp>
#endif

namespace Test_MultiTransportServer {

    class Echo
    {
    public:
        std::string echo(const std::string &s)
        {
            return s;
        }
    };

    RCF_BEGIN(I_Echo, "I_Echo")
        RCF_METHOD_R1(std::string, echo, const std::string &)
    RCF_END(I_Echo)

    RCF::Mutex gIoMutex;

    void clientTask(
        const RCF::I_ClientTransport &clientTransport,
        unsigned int calls,
        std::string s0)
    {
        RcfClient<I_Echo> echo( clientTransport.clone());
        for (unsigned int i=0; i<calls; ++i)
        {
            std::string s = echo.echo(s0);
            RCF_CHECK_EQ(s , s0);
            if (s != s0)
            {
                std::cout << "----------------------" << std::endl;
                std::cout << s << std::endl;
                std::cout << s0 << std::endl;
                std::cout << typeid(clientTransport).name() << std::endl;
                std::cout << i << std::endl;
                std::cout << calls << std::endl;
                std::cout << "----------------------" << std::endl;
            }
        }
    }

} // namespace Test_MultiTransportServer

int test_main(int argc, char **argv)
{

    printTestHeader(__FILE__);

    RCF::RcfInitDeinit rcfInitDeinit;

    using namespace Test_MultiTransportServer;

    util::CommandLine::getSingleton().parse(argc, argv);

    std::size_t count = 3;

    std::string s0 = "something to bounce off the server";
    Echo echo;
/*
#if defined(BOOST_WINDOWS) && defined(RCF_USE_BOOST_ASIO)
    {
        // Test RcfServer with a asio-based transport and iocp-based transport.
        // With a single server thread this will be quite slow due to polling timeouts.

        RCF::RcfServer server;

        RCF::ServicePtr iocpServicePtr( new RCF::TcpIocpServerTransport("127.0.0.1", 0) );
        server.addService(iocpServicePtr);

        RCF::ServicePtr asioServicePtr( new RCF::TcpAsioServerTransport("127.0.0.1", 0) );
        server.addService(asioServicePtr);
        
        server.bind( (I_Echo*) 0, echo);

        server.start();


        int port1 = dynamic_cast<RCF::TcpIocpServerTransport*>(iocpServicePtr.get())->getPort();
        int port2 = dynamic_cast<RCF::TcpAsioServerTransport*>(asioServicePtr.get())->getPort();

        RcfClient<I_Echo> client1( RCF::TcpEndpoint("127.0.0.1", port1) );
        RcfClient<I_Echo> client2( RCF::TcpEndpoint("127.0.0.1", port2) );

        std::string s;

        
        for (std::size_t i=0; i<3; ++i)
        {
            s = client1.echo(s0);
            s = client2.echo(s0);
        }
        server.stop();

        server.start();
        for (std::size_t i=0; i<3; ++i)
        {
            s = client1.echo(s0);
            s = client2.echo(s0);
        }
        server.stop();
    }
#endif
*/
    for (std::size_t iter=0; iter<2; ++iter)
    {
        // First time through, iocp based transports.
        // Second time through, asio based transports.

        // generate a bunch of corresponding server/client transport pairs
        std::vector<RCF::ServerTransportPtr> serverTransports;
        std::vector<RCF::ClientTransportAutoPtrPtr> clientTransports;
        
        for (unsigned int i=0; i<RCF::getTransportFactories().size(); ++i)
        {
            RCF::TransportFactoryPtr transportFactoryPtr = RCF::getTransportFactories()[i];
            
#ifdef RCF_USE_BOOST_ASIO
            if (iter == 0 && typeid(*transportFactoryPtr) == typeid(RCF::TcpAsioTransportFactory))
            {
                continue;
            }
#endif
            
#ifdef BOOST_WINDOWS
            if (    iter == 1 
                && (    typeid(*transportFactoryPtr) == typeid(RCF::TcpIocpTransportFactory)
                    || typeid(*transportFactoryPtr) == typeid(RCF::Win32NamedPipeTransportFactory)))
            {
                continue;
            }
#endif

            for (int j=0; j<count; ++j)
            {
                RCF::TransportPair transports = transportFactoryPtr->createTransports();
                serverTransports.push_back(transports.first);
                clientTransports.push_back(transports.second);

                std::cout << transportFactoryPtr->desc() << std::endl;
            }
        }

        {
            RCF::RcfServer server;
            for (std::size_t i=0; i<serverTransports.size(); ++i)
            {
                RCF::ServicePtr servicePtr(
                    boost::dynamic_pointer_cast<RCF::I_Service>(serverTransports[i]));

                server.addService(servicePtr);
            }
            server.bind( (I_Echo*) 0, echo);
            server.start();
            server.stop();
            server.start();
            for(unsigned int i=0; i<clientTransports.size(); ++i)
            {
                std::string s = RcfClient<I_Echo>( (*clientTransports[i])->clone() ).echo(s0);
                RCF_CHECK_EQ(s, s0);
            }
            server.stop();
        }

        {
            RCF::RcfServer server;
            for (unsigned int i=0; i<serverTransports.size(); ++i)
            {
                server.addServerTransport(serverTransports[i]);
            }
            server.bind( (I_Echo*) 0, echo);
            server.start();
            server.stop();
            server.start();
            for(unsigned int i=0; i<clientTransports.size(); ++i)
            {
                std::string s = RcfClient<I_Echo>( (*clientTransports[i])->clone() ).echo(s0);
                RCF_CHECK_EQ(s, s0);
            }
            server.stop();
        }

        {
            RCF::RcfServer server;
            for (unsigned int i=0; i<serverTransports.size(); ++i)
            {
                server.addServerTransport(serverTransports[i]);
            }
            server.bind( (I_Echo*) 0, echo);
            server.start();
            server.stop();
            server.start();

            int threadsPerClientTransport = 3;
            int callsPerClientThread = 50;
           
            ThreadGroup threadGroup;
            for (std::size_t i=0; i<clientTransports.size(); ++i)
            {
                for (std::size_t j=0; j<threadsPerClientTransport ; ++j)
                {
                    threadGroup.push_back( RCF::ThreadPtr( new RCF::Thread(
                        boost::bind(
                            clientTask,
                            boost::ref(**clientTransports[i]),
                            callsPerClientThread,
                            s0))));
                }
            }
            joinThreadGroup(threadGroup);

            server.stop();
        }

        for (std::size_t i=0; i<2; ++i)
        {

            RCF::RcfServer server;

            RCF::ThreadPoolPtr tmPtr;

            if (i == 0)
            {
                // Code to setup multiple transports with individual iocps and threads.

                tmPtr.reset( new RCF::ThreadPool(5, 10, "TCP V4", 30*1000));
                server.addEndpoint( RCF::TcpEndpoint("0.0.0.0", 50002) )
                    .setMaxMessageLength(20*1000)
                    .setConnectionLimit(20)
                    .setThreadPool(tmPtr);


                tmPtr.reset( new RCF::ThreadPool(5, 10, "TCP V6", 30*1000));
                server.addEndpoint( RCF::TcpEndpoint("0.0.0.0", 50003) )
                    .setMaxMessageLength(20*1000)
                    .setConnectionLimit(20)
                    .setThreadPool(tmPtr);
            }

            if (i == 1)
            {
                tmPtr.reset( new RCF::ThreadPool(1, 10, "RcfServer", 30*1000));
                server.setThreadPool(tmPtr);

                // Code to setup multiple transports sharing an iocp and thread pool.

                server.addEndpoint( RCF::TcpEndpoint("0.0.0.0", 50002) )
                    .setMaxMessageLength(20*1000)
                    .setConnectionLimit(20);

                server.addEndpoint( RCF::TcpEndpoint("0.0.0.0", 50003) )
                    .setMaxMessageLength(20*1000)
                    .setConnectionLimit(20);
            }

            Echo echo;
            server.bind( (I_Echo *) 0, echo);

            for (std::size_t j=0; j<3; ++j)
            {
                server.start();

                std::string s0 = "asdf";
                std::string s1;

                {
                    RcfClient<I_Echo> client( RCF::TcpEndpoint("127.0.0.1", 50002) );
                    s1 = client.echo(s0);
                    RCF_CHECK_EQ(s0 , s1);
                }
                {
                    RcfClient<I_Echo> client( RCF::TcpEndpoint("127.0.0.1", 50003) );
                    s1 = client.echo(s0);
                    RCF_CHECK_EQ(s0 , s1);
                }

                server.stop();
            }

    /*
            {
                // Code to setup RcfClient with multiple endpoints.

                RcfClient<I_WorkgroupService> client;

                client.getClientStub()
                    .setConnectionTimeoutMs()
                    .setMaxMessageLength();

                client.getClientStub().addEndpoint( RCF::TcpEndpointV4("MachineA", 1137) )
                    .setConnectionTimeoutMs()
                    .setMaxMessageLength();

                client.getClientStub().addEndpoint( RCF::TcpEndpointV6("MachineA", 1137) )
                    .setConnectionTimeoutMs()
                    .setMaxMessageLength();

                client.getClientStub().addEndpoint( RCF::TcpEndpointV4("MachineB", 1137) )
                    .setConnectionTimeoutMs()
                    .setMaxMessageLength();

                client.getClientStub().addEndpoint( RCF::TcpEndpointV6("MachineB", 1137) )
                    .setConnectionTimeoutMs()
                    .setMaxMessageLength();

                client.getClientStub().randomizeEndpoints();

                client.getClientStub().connect();
                client.getClientStub().getRemoteEndpoint();
            }
            */
        }
    }

    return 0;
}
