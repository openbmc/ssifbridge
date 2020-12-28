/*
 * Copyright (c) 2021 Ampere Computing LLC
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * This is a daemon that forwards requests and receive responses from SSIF over
 * the D-Bus IPMI Interface.
 */

#include <getopt.h>
#include <linux/ipmi_bmc.h>

#include <CLI/CLI.hpp>
#include <boost/algorithm/string/replace.hpp>
#include <boost/asio.hpp>
#include <phosphor-logging/log.hpp>
#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/asio/object_server.hpp>

#include <iostream>

using namespace phosphor::logging;

static constexpr const char devBase[] = "/dev/ipmi-ssif-host";
/* SSIF use IPMI SSIF channel */
static constexpr const char* ssifBus =
    "xyz.openbmc_project.Ipmi.Channel.ipmi_ssif";
static constexpr const char* ssifObj =
    "/xyz/openbmc_project/Ipmi/Channel/ipmi_ssif";

class SsifChannel
{
  public:
    static constexpr size_t ssifMessageSize = 4096;
    static constexpr uint8_t netFnShift = 2;
    static constexpr uint8_t lunMask = (1 << netFnShift) - 1;

    SsifChannel(std::shared_ptr<boost::asio::io_context>& io,
                   std::shared_ptr<sdbusplus::asio::connection>& bus,
                   const std::string& channel, bool verbose);
    bool initOK() const
    {
        return !!dev;
    }
    void channelAbort(const char* msg, const boost::system::error_code& ec);
    void async_read();
    void processMessage(const boost::system::error_code& ecRd, size_t rlen);

  protected:
    std::array<uint8_t, ssifMessageSize> xferBuffer;
    std::shared_ptr<boost::asio::io_context> io;
    std::shared_ptr<sdbusplus::asio::connection> bus;
    std::shared_ptr<sdbusplus::asio::object_server> server;
    std::unique_ptr<boost::asio::posix::stream_descriptor> dev = nullptr;
    bool verbose;
};

SsifChannel::SsifChannel(std::shared_ptr<boost::asio::io_context>& io,
                         std::shared_ptr<sdbusplus::asio::connection>& bus,
                         const std::string& device, bool verbose) :
    io(io),
    bus(bus), verbose(verbose)
{
    std::string devName = devBase;
    if (!device.empty())
    {
        devName = device;
    }

    // open device
    int fd = open(devName.c_str(), O_RDWR | O_NONBLOCK);
    if (fd < 0)
    {
        log<level::ERR>("Couldn't open SSIF driver with flags O_RDWR",
                        entry("FILENAME=%s", devName.c_str()),
                        entry("ERROR=%s", strerror(errno)));
        return;
    }
    else
    {
        dev = std::make_unique<boost::asio::posix::stream_descriptor>(*io,
                                                                      fd);
    }

    async_read();
    // register interfaces...
    server = std::make_shared<sdbusplus::asio::object_server>(bus);
    std::shared_ptr<sdbusplus::asio::dbus_interface> iface =
        server->add_interface(ssifObj, ssifBus);
    iface->initialize();
}

void SsifChannel::channelAbort(const char* msg,
                               const boost::system::error_code& ec)
{
    log<level::ERR>(msg, entry("ERROR=%s", ec.message().c_str()));
    // bail; maybe a restart from systemd can clear the error
    io->stop();
}

void SsifChannel::async_read()
{
    boost::asio::async_read(*dev,
                            boost::asio::buffer(xferBuffer, xferBuffer.size()),
                            boost::asio::transfer_at_least(2),
                            [this](const boost::system::error_code& ec,
                                   size_t rlen) {
        processMessage(ec, rlen);
    });
}

void SsifChannel::processMessage(const boost::system::error_code& ecRd,
                                 size_t rlen)
{
    if (ecRd || rlen < 2)
    {
        channelAbort("Failed to read req msg", ecRd);
        return;
    }
    async_read();

    auto rawIter = xferBuffer.cbegin();
    auto rawEnd = rawIter + rlen;
    uint8_t len = rawIter[0] + 1;
    uint8_t netfn = rawIter[1] >> netFnShift;
    uint8_t lun = rawIter[1] & lunMask;
    uint8_t cmd = rawIter[2];
    if (verbose)
    {
        log<level::INFO>("Read req msg", entry("NETFN=0x%02x", netfn),
                         entry("LUN=0x%02x", lun),
                         entry("CMD=0x%02x", cmd));
    }
    // copy out payload
    std::vector<uint8_t> data(&rawIter[3], rawEnd);
    // non-session bridges still need to pass an empty options map
    std::map<std::string, std::variant<int>> options;
    // the response is a tuple because dbus can only return a single value
    using IpmiDbusRspType = std::tuple<uint8_t, uint8_t, uint8_t, uint8_t,
                                       std::vector<uint8_t>>;
    static constexpr const char ipmiQueueService[] =
        "xyz.openbmc_project.Ipmi.Host";
    static constexpr const char ipmiQueuePath[] =
        "/xyz/openbmc_project/Ipmi";
    static constexpr const char ipmiQueueIntf[] =
        "xyz.openbmc_project.Ipmi.Server";
    static constexpr const char ipmiQueueMethod[] = "execute";
    bus->async_method_call(
        [this, netfnCap{netfn}, lunCap{lun},
         cmdCap{cmd}](const boost::system::error_code& ec,
                      const IpmiDbusRspType& response) {
            std::vector<uint8_t> rsp;
            const auto& [netfn, lun, cmd, cc, payload] = response;
            if (ec)
            {
                log<level::ERR>(
                    "ssif<->ipmid bus error:", entry("NETFN=0x%02x", netfn),
                    entry("LUN=0x%02x", lun), entry("CMD=0x%02x", cmd),
                    entry("ERROR=%s", ec.message().c_str()));
                // send unspecified error for a D-Bus error
                constexpr uint8_t ccResponseNotAvailable = 0xce;
                rsp.resize(sizeof(uint8_t) + sizeof(netfn) + sizeof(cmd) +
                           sizeof(cc));
                rsp[0] = 3;
                rsp[1] =
                    ((netfnCap + 1) << netFnShift) | (lunCap & lunMask);
                rsp[2] = cmdCap;
                rsp[3] = ccResponseNotAvailable;
            }
            else
            {
                rsp.resize(sizeof(uint8_t) + sizeof(netfn) + sizeof(cmd) +
                           sizeof(cc) + payload.size());

                // write the response
                auto rspIter = rsp.begin();
                rspIter[0] = payload.size() + 3;
                rspIter[1] = (netfn << netFnShift) | (lun & lunMask);
                rspIter[2] = cmd;
                rspIter[3] = cc;
                if (payload.size())
                {
                    std::copy(payload.cbegin(), payload.cend(),
                              &rspIter[4]);
                }
            }
            if (verbose)
            {
                log<level::INFO>(
                    "Send rsp msg", entry("NETFN=0x%02x", netfn),
                    entry("LUN=0x%02x", lun), entry("CMD=0x%02x", cmd),
                    entry("CC=0x%02x", cc));
            }
            boost::system::error_code ecWr;
            size_t wlen =
                boost::asio::write(*dev, boost::asio::buffer(rsp), ecWr);
            if (ecWr || wlen != rsp.size())
            {
                log<level::ERR>(
                    "Failed to send rsp msg", entry("SIZE=%d", wlen),
                    entry("EXPECT=%d", rsp.size()),
                    entry("ERROR=%s", ecWr.message().c_str()),
                    entry("NETFN=0x%02x", netfn), entry("LUN=0x%02x", lun),
                    entry("CMD=0x%02x", cmd), entry("CC=0x%02x", cc));
            }
        },
        ipmiQueueService, ipmiQueuePath, ipmiQueueIntf, ipmiQueueMethod,
        netfn, lun, cmd, data, options);
}


int main(int argc, char* argv[])
{
    CLI::App app("SSIF IPMI bridge");
    std::string device;
    app.add_option("-d,--device", device,
                   "use <DEVICE> file. Default is /dev/ipmi-ssif-host");
    bool verbose = false;
    app.add_option("-v,--verbose", verbose, "print more verbose output");
    CLI11_PARSE(app, argc, argv);

    // Connect to system bus
    auto io = std::make_shared<boost::asio::io_context>();
    sd_bus* dbus;
    sd_bus_default_system(&dbus);
    auto bus = std::make_shared<sdbusplus::asio::connection>(*io, dbus);
    bus->request_name(ssifBus);
    // Create the SSIF channel, listening on D-Bus and on the SSIF device
    SsifChannel ssifchannel(io, bus, device, verbose);
    if (!ssifchannel.initOK())
    {
        return EXIT_FAILURE;
    }
    io->run();

    return 0;
}
