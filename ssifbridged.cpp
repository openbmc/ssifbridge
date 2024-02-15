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
#include <boost/asio/completion_condition.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/write.hpp>
#include <phosphor-logging/log.hpp>
#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/asio/object_server.hpp>
#include <sdbusplus/timer.hpp>

#include <iostream>

/* Max length of ipmi ssif message included netfn and cmd field */
constexpr const size_t ipmiSsifPayloadMax = 254;

using phosphor::logging::level;
using phosphor::logging::log;

struct IpmiCmd
{
    uint8_t netfn;
    uint8_t lun;
    uint8_t cmd;
};

struct IpmiSsifMsg
{
    // Message length including lun/cmd bytes
    unsigned int len = 0;
    unsigned char lun_net_fn = 0;
    unsigned char cmd = 0;
    std::array<unsigned char, ipmiSsifPayloadMax> payload{};

    std::optional<std::span<unsigned char>> getPayload()
    {
        if (len > payload.size())
        {
            return std::nullopt;
        }
        return std::span<unsigned char>(payload.data(),
                                        len - sizeof(lun_net_fn) - sizeof(cmd));
    }

    boost::asio::const_buffer constBuffer()
    {
        if (len > sizeof(lun_net_fn) + sizeof(cmd) + payload.size())
        {
            return boost::asio::buffer(nullptr, 0);
        }
        return boost::asio::buffer(this, sizeof(len) + len);
    }

    boost::asio::mutable_buffer mutableBuffer()
    {
        return {this, sizeof(*this)};
    }

    bool readLengthValid(size_t read)
    {
        if (read < minSize)
        {
            return false;
        }
        if (len + sizeof(len) != read)
        {
            return false;
        }
        return true;
    }

    static constexpr size_t minSize = sizeof(len) + sizeof(lun_net_fn) +
                                      sizeof(cmd);
    // Known correct sizes for 32 bit and 64
    static_assert(minSize == 6 || minSize == 12);
} __attribute__((packed));

static_assert(sizeof(IpmiSsifMsg) == 260 || sizeof(IpmiSsifMsg) == 264);

static constexpr std::string_view devBase = "/dev/ipmi-ssif-host";
/* SSIF use IPMI SSIF channel */

/* The timer of driver is set to 15 seconds, need to send
 * response before timeout occurs
 */
static constexpr const unsigned int hostReqTimeout = 14000000;

class SsifChannel
{
  public:
    static constexpr size_t ssifMessageSize = ipmiSsifPayloadMax +
                                              sizeof(unsigned int);
    static constexpr uint8_t netFnShift = 2;
    static constexpr uint8_t lunMask = (1 << netFnShift) - 1;

    SsifChannel(std::shared_ptr<boost::asio::io_context>& io,
                std::shared_ptr<sdbusplus::asio::connection>& bus,
                const std::string& device, bool verbose);
    bool initOK() const
    {
        return dev.is_open();
    }
    void channelAbort(const char* msg, const boost::system::error_code& ec);
    void asyncRead();
    using IpmiDbusRspType =
        std::tuple<uint8_t, uint8_t, uint8_t, uint8_t, std::vector<uint8_t>>;

    void afterMethodCall(const boost::system::error_code& ec,
                         const IpmiDbusRspType& response);
    void processMessage(const boost::system::error_code& ecRd, size_t rlen);
    int showNumOfReqNotRsp() const;
    boost::asio::posix::stream_descriptor dev;
    IpmiCmd prevReqCmd{};

  protected:
    IpmiSsifMsg xferBuffer{};
    std::shared_ptr<boost::asio::io_context> io;
    std::shared_ptr<sdbusplus::asio::connection> bus;
    std::shared_ptr<sdbusplus::asio::object_server> server;
    bool verbose;
    /* This variable is always 0 when a request is responsed properly,
     * any value larger than 0 meaning there is/are request(s) which
     * not processed properly
     * */
    int numberOfReqNotRsp = 0;

    boost::asio::steady_timer rspTimer;
};

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
std::unique_ptr<SsifChannel> ssifchannel = nullptr;

SsifChannel::SsifChannel(std::shared_ptr<boost::asio::io_context>& io,
                         std::shared_ptr<sdbusplus::asio::connection>& bus,
                         const std::string& device, bool verbose) :
    dev(*io),
    io(io), bus(bus), verbose(verbose), rspTimer(*io)
{
    std::string devName(devBase);
    if (!device.empty())
    {
        devName = device;
    }

    // open device
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    int fd = open(devName.c_str(), O_RDWR | O_NONBLOCK);
    if (fd < 0)
    {
        std::string msgToLog = "Couldn't open SSIF driver with flags O_RDWR."
                               " FILENAME=" +
                               devName + " ERROR=" + strerror(errno);
        log<level::ERR>(msgToLog.c_str());
        return;
    }

    dev.assign(fd);

    asyncRead();
    // register interfaces...
    server = std::make_shared<sdbusplus::asio::object_server>(bus);
    std::shared_ptr<sdbusplus::asio::dbus_interface> iface =
        server->add_interface("/xyz/openbmc_project/Ipmi/Channel/ipmi_ssif",
                              "xyz.openbmc_project.Ipmi.Channel.ipmi_ssif");
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

void SsifChannel::asyncRead()
{
    dev.async_read_some(xferBuffer.mutableBuffer(),
                        [this](const boost::system::error_code& ec,
                               size_t rlen) { processMessage(ec, rlen); });
}

int SsifChannel::showNumOfReqNotRsp() const
{
    return numberOfReqNotRsp;
}

void rspTimerHandler(const boost::system::error_code& ec)
{
    if (ec == boost::asio::error::operation_aborted)
    {
        return;
    }
    constexpr uint8_t ccResponseNotAvailable = 0xce;
    const IpmiCmd& prevReqCmd = ssifchannel->prevReqCmd;
    std::string msgToLog = "timeout, send response to keep host alive"
                           " netfn=" +
                           std::to_string(prevReqCmd.netfn) +
                           " lun=" + std::to_string(prevReqCmd.lun) +
                           " cmd=" + std::to_string(prevReqCmd.cmd) +
                           " cc=" + std::to_string(ccResponseNotAvailable) +
                           " numberOfReqNotRsp=" +
                           std::to_string(ssifchannel->showNumOfReqNotRsp());
    log<level::INFO>(msgToLog.c_str());
    IpmiSsifMsg msg;
    msg.len = 3;
    msg.lun_net_fn = ((prevReqCmd.netfn + 1) << ssifchannel->netFnShift) |
                     (prevReqCmd.lun & ssifchannel->lunMask);
    msg.cmd = prevReqCmd.cmd;
    msg.payload[0] = ccResponseNotAvailable;

    boost::system::error_code ecWr;

    size_t wlen = boost::asio::write(ssifchannel->dev, msg.constBuffer(), ecWr);
    if (ecWr)
    {
        msgToLog = "Failed to send ssif respond message:"
                   " size=" +
                   std::to_string(wlen) + " error=" + ecWr.message() +
                   " netfn=" + std::to_string(prevReqCmd.netfn + 1) +
                   " lun=" + std::to_string(prevReqCmd.lun) +
                   " cmd=" + std::to_string(msg.cmd) +
                   " cc=" + std::to_string(ccResponseNotAvailable);
        log<level::ERR>(msgToLog.c_str());
    }
}

void SsifChannel::afterMethodCall(const boost::system::error_code& ec,
                                  const IpmiDbusRspType& response)
{
    const auto& [netfn, lun, cmd, cc, payload] = response;
    numberOfReqNotRsp--;
    if (ec)
    {
        std::string msgToLog =
            "ssif<->ipmid bus error:"
            " netfn=" +
            std::to_string(netfn) + " lun=" + std::to_string(lun) +
            " cmd=" + std::to_string(cmd) + " error=" + ec.message();
        log<level::ERR>(msgToLog.c_str());
        /* if dbusTimeout, just return and do not send any response
         * to let host continue with other commands, response here
         * is potentially make the response duplicated
         * */
        return;
    }

    if ((prevReqCmd.netfn != (netfn - 1) || prevReqCmd.lun != lun ||
         prevReqCmd.cmd != cmd) ||
        ((prevReqCmd.netfn == (netfn - 1) && prevReqCmd.lun == lun &&
          prevReqCmd.cmd == cmd) &&
         numberOfReqNotRsp != 0))
    {
        /* Only send response to the last request command to void
         * duplicated response which makes host driver confused and
         * failed to create interface
         *
         * Drop responses which are (1) different from the request
         * (2) parameters are the same as request but handshake flow
         * are in dupplicate request state
         * */
        if (verbose)
        {
            std::string msgToLog =
                "Drop ssif respond message with"
                " len=" +
                std::to_string(payload.size() + 3) +
                " netfn=" + std::to_string(netfn) +
                " lun=" + std::to_string(lun) + " cmd=" + std::to_string(cmd) +
                " cc=" + std::to_string(cc) +
                " numberOfReqNotRsp=" + std::to_string(numberOfReqNotRsp);
            log<level::INFO>(msgToLog.c_str());
        }
        return;
    }

    IpmiSsifMsg rsp;
    rsp.len = payload.size() + 3;
    rsp.lun_net_fn = (netfn << netFnShift) | (lun & lunMask);
    rsp.cmd = cmd;
    rsp.payload[0] = cc;

    if (static_cast<unsigned int>(!payload.empty()) != 0U)
    {
        std::copy(payload.begin(), payload.end(), &rsp.payload[1]);
    }
    if (verbose)
    {
        std::string msgToLog =
            "Send ssif respond message with"
            " len=" +
            std::to_string(payload.size() + 3) +
            " netfn=" + std::to_string(netfn) + " lun=" + std::to_string(lun) +
            " cmd=" + std::to_string(cmd) + " cc=" + std::to_string(cc) +
            " numberOfReqNotRsp=" + std::to_string(numberOfReqNotRsp);
        log<level::INFO>(msgToLog.c_str());
    }
    boost::system::error_code ecWr;
    size_t wlen = boost::asio::write(dev, rsp.constBuffer(), ecWr);
    if (ecWr)
    {
        std::string msgToLog =
            "Failed to send ssif respond message:"
            " size=" +
            std::to_string(wlen) + " expect=" + std::to_string(rsp.len) +
            " error=" + ecWr.message() + " netfn=" + std::to_string(netfn) +
            " lun=" + std::to_string(lun) + " cmd=" + std::to_string(cmd) +
            " cc=" + std::to_string(cc);
        log<level::ERR>(msgToLog.c_str());
    }
    rspTimer.cancel();
}

void SsifChannel::processMessage(const boost::system::error_code& ecRd,
                                 size_t rlen)
{
    if (ecRd || !xferBuffer.readLengthValid(rlen))
    {
        channelAbort("Failed to read req msg", ecRd);
        return;
    }

    uint8_t netfn = xferBuffer.lun_net_fn >> netFnShift;
    uint8_t lun = xferBuffer.lun_net_fn & lunMask;
    uint8_t cmd = xferBuffer.cmd;

    /* keep track of previous request */
    prevReqCmd.netfn = netfn;
    prevReqCmd.lun = lun;
    prevReqCmd.cmd = cmd;

    /* there is a request coming */
    numberOfReqNotRsp++;
    /* start response timer */
    rspTimer.expires_after(std::chrono::microseconds(hostReqTimeout));
    rspTimer.async_wait(rspTimerHandler);

    if (verbose)
    {
        std::string msgToLog =
            "Read ssif request message with"
            " len=" +
            std::to_string(xferBuffer.len) + " netfn=" + std::to_string(netfn) +
            " lun=" + std::to_string(lun) + " cmd=" + std::to_string(cmd) +
            " numberOfReqNotRsp=" + std::to_string(numberOfReqNotRsp);
        log<level::INFO>(msgToLog.c_str());
    }
    // copy out payload
    std::optional<std::span<unsigned char>> data = xferBuffer.getPayload();

    if (!data)
    {
        channelAbort("Failed to read req msg", ecRd);
        return;
    }
    // non-session bridges still need to pass an empty options map
    std::map<std::string, std::variant<int>> options;
    static constexpr const char* ipmiQueueService =
        "xyz.openbmc_project.Ipmi.Host";
    static constexpr const char* ipmiQueuePath = "/xyz/openbmc_project/Ipmi";
    static constexpr const char* ipmiQueueIntf =
        "xyz.openbmc_project.Ipmi.Server";
    static constexpr const char* ipmiQueueMethod = "execute";
    /* now, we do not care dbus timeout, since we already have actions
     * before dbus timeout occurs
     */
    static constexpr unsigned int dbusTimeout = 60000000;
    bus->async_method_call_timed(
        [this](const boost::system::error_code& ec,
               const IpmiDbusRspType& response) {
        afterMethodCall(ec, response);
    },
        ipmiQueueService, ipmiQueuePath, ipmiQueueIntf, ipmiQueueMethod,
        dbusTimeout, netfn, lun, cmd, *data, options);
    asyncRead();
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

    auto io = std::make_shared<boost::asio::io_context>();

    auto bus = std::make_shared<sdbusplus::asio::connection>(*io);
    bus->request_name("xyz.openbmc_project.Ipmi.Channel.ipmi_ssif");
    // Create the SSIF channel, listening on D-Bus and on the SSIF device
    ssifchannel = make_unique<SsifChannel>(io, bus, device, verbose);
    if (!ssifchannel->initOK())
    {
        return EXIT_FAILURE;
    }
    io->run();

    return 0;
}
