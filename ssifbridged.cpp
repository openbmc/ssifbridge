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

/* Max length of ipmi ssif message included netfn and cmd field */
#define IPMI_SSIF_PAYLOAD_MAX         254

using namespace phosphor::logging;

struct ipmi_cmd {
    uint8_t netfn;
    uint8_t lun;
    uint8_t cmd;
} prev_req_cmd;

static constexpr const char devBase[] = "/dev/ipmi-ssif-host";
/* SSIF use IPMI SSIF channel */
static constexpr const char* ssifBus =
    "xyz.openbmc_project.Ipmi.Channel.ipmi_ssif";
static constexpr const char* ssifObj =
    "/xyz/openbmc_project/Ipmi/Channel/ipmi_ssif";

class SsifChannel
{
  public:
    static constexpr size_t ssifMessageSize = IPMI_SSIF_PAYLOAD_MAX +
                                              sizeof(unsigned int);
    size_t sizeofLenField = sizeof(unsigned int);
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
        std::string msgToLog = "Couldn't open SSIF driver with flags O_RDWR."
                " FILENAME=" + devName +
                " ERROR=" + strerror(errno);
        log<level::ERR>(msgToLog.c_str());
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
    std::string msgToLog = std::string(msg) + " ERROR=" + ec.message();
    log<level::ERR>(msgToLog.c_str());
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
    uint8_t netfn = rawIter[sizeofLenField] >> netFnShift;
    uint8_t lun = rawIter[sizeofLenField] & lunMask;
    uint8_t cmd = rawIter[sizeofLenField + 1];

    prev_req_cmd.netfn = netfn;
    prev_req_cmd.lun = lun;
    prev_req_cmd.cmd = cmd;

    if (verbose)
    {
        unsigned int lenRecv;
        unsigned int *p = (unsigned int *) rawIter;
        lenRecv = p[0];
        std::string msgToLog = "Read ssif request message with"
                " len=" + std::to_string(lenRecv) +
                " netfn=" + std::to_string(netfn) +
                " lun=" + std::to_string(lun) +
                " cmd=" + std::to_string(cmd);
        log<level::INFO>(msgToLog.c_str());
    }
    // copy out payload
    std::vector<uint8_t> data(rawIter + sizeofLenField + 2, rawEnd);
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
    static constexpr int dbusTimeout = 40000000;
    bus->async_method_call_timed(
        [this, netfnCap{netfn}, lunCap{lun},
         cmdCap{cmd}](const boost::system::error_code& ec,
                      const IpmiDbusRspType& response) {
            std::vector<uint8_t> rsp;
            const auto& [netfn, lun, cmd, cc, payload] = response;
            if (ec)
            {
                std::string msgToLog = "ssif<->ipmid bus error:"
                        " netfn=" + std::to_string(netfn) +
                        " lun=" + std::to_string(lun) +
                        " cmd=" + std::to_string(cmd) +
                        " error=" + ec.message();
                log<level::ERR>(msgToLog.c_str());
                rsp.resize(sizeofLenField + sizeof(netfn) + sizeof(cmd) +
                           sizeof(cc));
                /* if dbusTimeout, just return and do not send any response
                 * to let host continue with other commands, response here
                 * is potentionally make the response duplicated 
                 * */
                return;
            }
            else
            {
                if(prev_req_cmd.netfn != (netfn-1) || 
                        prev_req_cmd.lun != lun ||
                        prev_req_cmd.cmd != cmd)
                {
                    /* Only send response to the last request command to void
                     * duplicated response which makes host driver confused and
                     * failed to create interface
                     * */
                    return;
                }
                rsp.resize(sizeofLenField + sizeof(netfn) + sizeof(cmd) +
                           sizeof(cc) + payload.size());

                // write the response
                auto rspIter = rsp.begin();
                unsigned int *p = (unsigned int *) &rspIter[0];
                *p = payload.size() + 3;
                rspIter[sizeofLenField] = (netfn << netFnShift) | (lun & lunMask);
                rspIter[sizeofLenField + 1] = cmd;
                rspIter[sizeofLenField + 2] = cc;
                if (payload.size())
                {
                    std::copy(payload.cbegin(), payload.cend(),
                            rspIter + sizeofLenField + 3);
                }
            }
            if (verbose)
            {
                std::string msgToLog = "Send ssif respond message with"
                        " len=" + std::to_string(payload.size() + 3) +
                        " netfn=" + std::to_string(netfn) +
                        " lun=" + std::to_string(lun) +
                        " cmd=" + std::to_string(cmd) +
                        " cc=" + std::to_string(cc);
                log<level::INFO>(msgToLog.c_str());
            }
            boost::system::error_code ecWr;
            size_t wlen =
                boost::asio::write(*dev, boost::asio::buffer(rsp), ecWr);
            if (ecWr || wlen != rsp.size())
            {
                std::string msgToLog = "Failed to send ssif respond message:"
                        " size=" + std::to_string(wlen) +
                        " expect=" + std::to_string(rsp.size()) +
                        " error=" + ecWr.message() +
                        " netfn=" + std::to_string(netfn) +
                        " lun=" + std::to_string(lun) +
                        " cmd=" + std::to_string(cmd) +
                        " cc=" + std::to_string(cc);
                log<level::ERR>(msgToLog.c_str());
            }
        },
        ipmiQueueService, ipmiQueuePath, ipmiQueueIntf, ipmiQueueMethod, dbusTimeout,
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
