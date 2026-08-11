
#ifndef PIKO_SYNC_NET_IO_H
#define PIKO_SYNC_NET_IO_H

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <string>

#include "protocol.h"

namespace piko_sync {

inline void set_nonblock(int fd)
{
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0)
        fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

inline bool write_all(int fd, const char *data, size_t len)
{
    size_t sent = 0;
    int stalls = 0;
    while (sent < len) {
        ssize_t n = write(fd, data + sent, len - sent);
        if (n > 0) {
            sent += static_cast<size_t>(n);
            stalls = 0;
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (++stalls > 500)
                return false;
            fd_set wfds;
            FD_ZERO(&wfds);
            FD_SET(fd, &wfds);
            struct timeval tv;
            tv.tv_sec = 0;
            tv.tv_usec = 10000;
            select(fd + 1, 0, &wfds, 0, &tv);
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        return false;
    }
    return true;
}

inline bool send_frame_blocking(int fd, uint32_t type, const std::string &payload)
{
    std::string f = encode_frame(type, payload);
    return write_all(fd, f.data(), f.size());
}

inline std::string wlan0_address()
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return std::string();

    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, "wlan0", IFNAMSIZ - 1);

    std::string result;
    if (ioctl(fd, SIOCGIFADDR, &ifr) == 0) {
        struct sockaddr_in *sin = reinterpret_cast<struct sockaddr_in *>(&ifr.ifr_addr);
        char buf[INET_ADDRSTRLEN];
        if (inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf)))
            result = buf;
    }
    close(fd);
    return result;
}

}

#endif
