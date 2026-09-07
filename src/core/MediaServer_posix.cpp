// POSIX MediaServer.
//
// TV casting is out of scope for the Linux port — that path also needs the
// Node server, the WASM DSP and an NVENC transcode chain, none of which apply
// here.  So this reports the server as unsupported and the Preferences UI
// hides the tab.
//
// lanAddress() is implemented for real anyway: it is a two-line getifaddrs()
// walk, it can't fail in a way that matters, and leaving it working means the
// UI has one less platform branch.

#include "MediaServer.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <cstring>

MediaServer::~MediaServer() { stop(); }

bool MediaServer::supported() { return false; }

bool MediaServer::start(const std::string& /*mediaDir*/, int /*port*/) {
    m_error = "The media server is only available on the Windows build.";
    return false;
}

void MediaServer::stop() {}

bool MediaServer::isRunning() { return false; }

std::string MediaServer::lanAddress() {
    struct ifaddrs* ifs = nullptr;
    if (::getifaddrs(&ifs) != 0 || !ifs) return {};

    std::string fallback;
    std::string best;
    for (struct ifaddrs* ifa = ifs; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
        if (!(ifa->ifa_flags & IFF_UP)) continue;
        if (ifa->ifa_flags & IFF_LOOPBACK) continue;

        char buf[INET_ADDRSTRLEN] = {0};
        const auto* sin = reinterpret_cast<const struct sockaddr_in*>(ifa->ifa_addr);
        if (!::inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf))) continue;

        const std::string ip(buf);
        if (ip.rfind("169.254.", 0) == 0) continue;   // link-local, never routable

        // Prefer an RFC1918 address: on a machine with a VPN or a container
        // bridge up, the first interface enumerated is rarely the one the TV
        // can reach.
        const bool priv = ip.rfind("192.168.", 0) == 0 ||
                          ip.rfind("10.", 0) == 0 ||
                          ip.rfind("172.16.", 0) == 0 || ip.rfind("172.17.", 0) == 0 ||
                          ip.rfind("172.18.", 0) == 0 || ip.rfind("172.19.", 0) == 0 ||
                          ip.rfind("172.2", 0) == 0   || ip.rfind("172.30.", 0) == 0 ||
                          ip.rfind("172.31.", 0) == 0;
        if (priv) { best = ip; break; }
        if (fallback.empty()) fallback = ip;
    }
    ::freeifaddrs(ifs);
    return best.empty() ? fallback : best;
}
