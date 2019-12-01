/**
 * @file hued.cpp
 *
 * HUE SSDP responder daemon
 *
 * This little daemon does the SSDP handling of a Philips hue bridge or a
 * hue bridge emulation like HA-Bridge or node-red-contrib-amazon-echo.
 * SSDP can not detect devices in other subnets or in docker containers
 * because the used multicast UDP datagrams do not pass gateways or the docker
 * network bridge. If this daemon runs in the same subnet as the SSDP
 * enumerator (e.g. an Amazon Echo) then it will respond to the SSDP requests
 * and redirect the enumerator to the HUE bridge (emulator). The communication
 * to the HUE bridge is simple unicast HTTP, so the HUE bridge can be
 * elsewhere, in a docker container or around the world.
 *
 * The daemon requires one parameter in the form "server:service", e.g.
 * "my-hue.local:80".
 *
 * @author Andreas Schmitt
 */

/*
 * Boost Software License - Version 1.0 - August 17th, 2003
 *
 * Permission is hereby granted, free of charge, to any person or organization
 * obtaining a copy of the software and accompanying documentation covered by
 * this license (the "Software") to use, reproduce, display, distribute,
 * execute, and transmit the Software, and to prepare derivative works of the
 * Software, and to permit third-parties to whom the Software is furnished to
 * do so, all subject to the following:
 *
 * The copyright notices in the Software and this entire statement, including
 * the above license grant, this restriction and the following disclaimer,
 * must be included in all copies of the Software, in whole or in part, and
 * all derivative works of the Software, unless such copies or derivative
 * works are solely in the form of machine-executable object code generated by
 * a source language processor.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, TITLE AND NON-INFRINGEMENT. IN NO EVENT
 * SHALL THE COPYRIGHT HOLDERS OR ANYONE DISTRIBUTING THE SOFTWARE BE LIABLE
 * FOR ANY DAMAGES OR OTHER LIABILITY, WHETHER IN CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <cstdlib>
#include <iostream>
#include <string>
#include <boost/asio.hpp>
#include <boost/bind.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/xml_parser.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/format.hpp>
#include <unordered_map>
#include <unordered_set>
#include <regex>

using namespace boost::asio;

const uint16_t multicast_port=1900; ///< Listen on this port for SSDP requests
const uint16_t t_refresh=300; ///< Cache the description.xml for this many seconds

/// All three responses to an SSDP request start with this data
const char HUE_RESPONSE[] =
  "HTTP/1.1 200 OK\r\n"
  "HOST: 239.255.255.250:1900\r\n"
  "CACHE-CONTROL: max-age=100\r\n"
  "EXT:\r\n"
  "LOCATION: http://%1%:%2%/description.xml\r\n"
  "SERVER: Linux/3.14.0 UPnP/1.0 IpBridge/1.24.0\r\n"  // was 1.17
  "hue-bridgeid: %3%\r\n%4%";
/// The first SSDP response is HUE_RESONSE+HUE_ST1
const char HUE_ST1[] =
  "ST: upnp:rootdevice\r\n"
  "USN: uuid:%1%::upnp:rootdevice\r\n"
  "\r\n";
/// The second SSDP response is HUE_RESONSE+HUE_ST2
const char HUE_ST2[] =
  "ST: uuid:%1%\r\n"
  "USN: uuid:%1%\r\n"
  "\r\n";
/// The third SSDP response is HUE_RESONSE+HUE_ST3
const char HUE_ST3[] =
  "ST: urn:schemas-upnp-org:device:basic:1\r\n"
  "USN: uuid:%1%\r\n"
  "\r\n";
/// A list of service types to which hued responds. Those types can be found in the "ST:" field of the SSDP request.
const std::unordered_set<std::string> service_types
{
    "urn:schemas-upnp-org:device:Basic:1",
    "upnp:rootdevice",
    "ssdpsearch:all",
    "ssdp:all"
};

/// SSDP responder class
class Responder
{
private:
    io_service &_io_service;
    std::string _server;
    std::string _service;
    std::string _uuid;
    ip::udp::socket _socket;
    bool _refresh;
    deadline_timer _trefresh;
    deadline_timer _tresponse;

public:
    /// The constructor opens the UDP response port.
    Responder(io_service &io_service, const std::string &server, const std::string &service) :
        _io_service(io_service), _server(server), _service(service), _uuid(),
        _socket(io_service), _refresh(true), _trefresh(io_service), _tresponse(io_service)
    {
        _socket.open(ip::udp::v4());
    }

    /**
     * Update the UUID of the HUE bridge.
     *
     * For obtaining the UUID of the HUE bridge this function downloads the description.xml from the bridge,
     * parses this document and reads the UUID. The function immediately returns if it is called during
     * t_refresh seconds after the last call to prevent DOS to the HUE bridge.
     */
    void update()
    {
        if (_refresh)
        {
            _refresh=false;
            _trefresh.expires_from_now(boost::posix_time::seconds(t_refresh));
            _trefresh.async_wait(boost::bind(&Responder::refresh, this, placeholders::error));

            ip::tcp::tcp::resolver resolver(_io_service);
            ip::tcp::tcp::resolver::query query(_server, _service);
            ip::tcp::tcp::resolver::iterator endpoint_iterator = resolver.resolve(query);

            ip::tcp::tcp::socket squery(_io_service);
            connect(squery, endpoint_iterator);

            // Build a HTTP request for description.xml
            streambuf request;
            std::ostream request_stream(&request);
            request_stream << "GET /description.xml HTTP/1.0\r\n";
            request_stream << "Host: " << _server << "\r\n";
            request_stream << "Accept: */*\r\n";
            request_stream << "Connection: close\r\n\r\n";

            // Send the request.
            write(squery, request);

            // Read the response status line. The response streambuf will automatically
            // grow to accommodate the entire line. The growth may be limited by passing
            // a maximum size to the streambuf constructor.
            streambuf response;
            read_until(squery, response, "\r\n");

            // Check that response is OK.
            std::istream response_stream(&response);
            std::string http_version;
            response_stream >> http_version;
            uint16_t status_code;
            response_stream >> status_code;
            std::string status_message;
            std::getline(response_stream, status_message);

            // Invalid response
            if (!response_stream || http_version.substr(0, 5) != "HTTP/") return;

            // Wrong status code
            if (status_code != 200) return;

            // Read the response headers, which are terminated by a blank line.
            read_until(squery, response, "\r\n\r\n");

            // Process the response headers. Just discard the data.
            std::string header;
            while (std::getline(response_stream, header) && header != "\r");

            // Read until EOF
            boost::system::error_code error;
            while (read(squery, response,
                transfer_at_least(1), error));
            if (error != error::eof)
                throw boost::system::system_error(error);

            // Create empty property tree object
            boost::property_tree::ptree tree;

            // Parse the XML into the property tree.
            std::iostream xml(&response);
            boost::property_tree::read_xml(xml, tree);

            std::string uuid=tree.get<std::string>("root.device.UDN");
            const std::string uu("uuid:");
            if (uuid.find(uu)==0)
            {
                _uuid=uuid.substr(uu.length());
            }
        }
    }

    /**
     * Starts the SSID response.
     *
     * The function calls update() to get the current UUDI of the HUE bridge. Then it starts an async
     * timer of between 0 and a (pseudo) random time given by mx (in seconds), which triggers
     * respond(). This mechanism reduces the DDOS problem for the enumerating device when each
     * enumerated device in the subnet answers to the same request.
     *
     * \param addr      The address to respond to (HUE bridge)
     * \param port      The port to respond to (HUE bridge)
     * \param mx        An interval in seconds, in which the response should be sent.
     */
    void operator()(ip::address addr, uint16_t port, uint16_t mx)
    {
        update();
        uint32_t t_response=static_cast<uint64_t>(mx)*1000*rand()/RAND_MAX;
        _tresponse.expires_from_now(boost::posix_time::milliseconds(t_response));
        _tresponse.async_wait(boost::bind(&Responder::respond, this, placeholders::error, addr, port));
    }

private:
    /**
     * Small helper to set the _refresh flag after the timeout t_refresh.
     */
    void refresh(const boost::system::error_code &e)
    {
        if (e) return;
        _refresh=true;
    }

    /**
     * Sends three response telegrams (see #HUE_RESPONSE, #HUE_ST1, #HUE_ST2, #HUE_ST3) to the endpoint
     * given by addr and port.
     *
     * \param e     If this async timer error code says something other than OK the fuction returns
     *                  immediately.
     * \param addr  The address of the SSDP enumerator
     * \param port  The port of the SSDP enumerator
     */
    void respond(const boost::system::error_code &e, ip::address addr, uint16_t port)
    {
        if (e) return;

        const ip::udp::endpoint endpoint(addr, port);

        boost::format st1(HUE_ST1);
        st1%_uuid;
        boost::format st2(HUE_ST2);
        st2%_uuid;
        boost::format st3(HUE_ST3);
        st3%_uuid;
        boost::format msg1(HUE_RESPONSE);
        boost::format msg2(HUE_RESPONSE);
        boost::format msg3(HUE_RESPONSE);
        msg1%_server%_service%_uuid%st1;
        msg2%_server%_service%_uuid%st2;
        msg3%_server%_service%_uuid%st3;
        _socket.send_to(buffer(msg1.str()), endpoint);
        _socket.send_to(buffer(msg2.str()), endpoint);
        _socket.send_to(buffer(msg3.str()), endpoint);
    }

};

/// SSDP Listener
class Listener
{
private:
    Responder &_resp;
    ip::udp::socket _socket;
    ip::udp::endpoint _sender_endpoint;
    static const uint16_t _max_length=1024;
    char _data[_max_length];

public:
    /**
     * The constructor opens the SSDP port for listening, joins the multicast group and starts listening.
     */
    Listener(io_service &io_service, Responder &resp, const ip::address &listen_address,
        const ip::address &multicast_address) :
        _resp(resp), _socket(io_service)
    {
        // Create the socket so that multiple may be bound to the same address.
        ip::udp::endpoint listen_endpoint(listen_address, multicast_port);
        _socket.open(listen_endpoint.protocol());
        _socket.set_option(ip::udp::socket::reuse_address(true));
        _socket.bind(listen_endpoint);

        // Join the multicast group.
        _socket.set_option(ip::multicast::join_group(multicast_address));

        _socket.async_receive_from(buffer(_data, _max_length), _sender_endpoint,
            boost::bind(&Listener::receive, this, placeholders::error,
                placeholders::bytes_transferred));
    }

    /**
     * Evaluates the received datagram.
     *
     * The function checks if the received datagram is a well formed "M-SEARCH" datagram, parses the data
     * for sevice type ("ST:") and response timeout ("MX:") and checks, if the requested service type is
     * a supported type for HUE bridge devices. If yes, then the function triggers a response to the same
     * address and port the SSDP datagram was received on.
     *
     * @param error     If this error code says anything other than OK then the function returns immediately.
     * @param bytes     Number of received bytes
     */
    void receive(const boost::system::error_code &error, size_t bytes)
    {
        if (error) return;

        _socket.async_receive_from(buffer(_data, _max_length), _sender_endpoint,
            boost::bind(&Listener::receive, this, placeholders::error,
                placeholders::bytes_transferred));

        const std::regex msearch("^M-SEARCH \\* HTTP/1\\.1");
        const std::regex expr{"(\\S+):\\s(\\S+)"};
        std::match_results<char*> what;

        if (std::regex_search(_data, _data+bytes, what, msearch))
        {
            std::unordered_map<std::string, std::string> request;
            for (char* start=what[0].second;
                regex_search(start, _data+bytes, what, expr);
                start=what[2].second) request[what[1]]=what[2];

            // Is this a supported service type?
            if (service_types.find(request["ST"])==service_types.end()) return;

            try
            {
                _resp(_sender_endpoint.address(), _sender_endpoint.port(), boost::lexical_cast<uint16_t>(request["MX"]));
            }
            catch(const boost::bad_lexical_cast &)
            {
            }
        }
    }
};

/// Daemon entry point, one program argument in the form "server:service" is required.
int main(int argc, char *argv[])
{
    if (argc!=2)
    {
        std::cerr << "Exactly one parameter in the form 'server:service' is required." << std::endl;
        return EXIT_FAILURE;
    }
    const std::string param(argv[1]);
    const size_t colon=param.find_first_of(':');
    if (colon==std::string::npos)
    {
        std::cerr << "Exactly one parameter in the form 'server:service' is required." << std::endl;
        return EXIT_FAILURE;
    }
    try
    {
        io_service io_service;
        Responder resp(io_service, param.substr(0, colon), param.substr(colon+1));
        Listener rec(io_service, resp, ip::address::from_string("0.0.0.0"),
            ip::address::from_string("239.255.255.250"));
        io_service.run();
    } catch (std::exception &e)
    {
        std::cerr << "Exception: " << e.what() << std::endl;
        return EXIT_FAILURE;
    }

    return 0;
}