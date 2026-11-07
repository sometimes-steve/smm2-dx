// Host sockets for the game's own network stack (runtime/services/bsd.c, ssl.c): thin wrappers over Winsock, and
// errno numbers as the game expects them (FreeBSD's, as Horizon's bsd service reports them).
#pragma once
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET hsock_t;
#define HSOCK_BAD INVALID_SOCKET
#define hsock_close closesocket
typedef WSAPOLLFD hpollfd;
static inline int hsock_poll(hpollfd *f, unsigned n, int ms) { return WSAPoll(f, n, ms); }
static inline int hsock_errno(void) { return WSAGetLastError(); }
static inline void hsock_startup(void) {
    static int done;
    if (!done) { WSADATA w; WSAStartup(MAKEWORD(2, 2), &w); done = 1; }
}
static inline void hsock_nonblock(hsock_t s) { u_long v = 1; ioctlsocket(s, FIONBIO, &v); }
typedef int hsocklen_t;
#define HSOCK_IOLEN(n) ((int)(n))

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

// The errno numbers the game's socket library expects. They are Linux's, not FreeBSD's: the Switch's socket errno
// enum follows Linux (EAGAIN 11, EINPROGRESS 115, EISCONN 106 ...). SM35's own socket wrapper tests the value after
// a failed connect against 115 for "in progress" and 11 / 100 / 9 after a close.
enum {
    BSD_EPERM = 1, BSD_ENOENT = 2, BSD_EINTR = 4, BSD_EIO = 5, BSD_EBADF = 9, BSD_EAGAIN = 11, BSD_ENOMEM = 12,
    BSD_EACCES = 13, BSD_EFAULT = 14, BSD_EBUSY = 16, BSD_EINVAL = 22, BSD_EMFILE = 24, BSD_EPIPE = 32,
    BSD_ENOTSOCK = 88, BSD_EDESTADDRREQ = 89, BSD_EMSGSIZE = 90, BSD_EPROTOTYPE = 91, BSD_ENOPROTOOPT = 92,
    BSD_EPROTONOSUPPORT = 93, BSD_EOPNOTSUPP = 95, BSD_EAFNOSUPPORT = 97, BSD_EADDRINUSE = 98,
    BSD_EADDRNOTAVAIL = 99, BSD_ENETDOWN = 100, BSD_ENETUNREACH = 101, BSD_ENETRESET = 102, BSD_ECONNABORTED = 103,
    BSD_ECONNRESET = 104, BSD_ENOBUFS = 105, BSD_EISCONN = 106, BSD_ENOTCONN = 107, BSD_ESHUTDOWN = 108,
    BSD_ETIMEDOUT = 110, BSD_ECONNREFUSED = 111, BSD_EHOSTDOWN = 112, BSD_EHOSTUNREACH = 113, BSD_EALREADY = 114,
    BSD_EINPROGRESS = 115,
};

static inline bool hsock_would_block(int e) {
    return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS;
}

// a host errno (WSAGetLastError) -> FreeBSD's number
static inline uint32_t hsock_bsd_errno(int e) {
    switch (e) {
    case WSAEWOULDBLOCK: return BSD_EAGAIN;
    case WSAEINPROGRESS: return BSD_EINPROGRESS;
    case WSAEALREADY: return BSD_EALREADY;
    case WSAENOTSOCK: return BSD_ENOTSOCK;
    case WSAEDESTADDRREQ: return BSD_EDESTADDRREQ;
    case WSAEMSGSIZE: return BSD_EMSGSIZE;
    case WSAEPROTOTYPE: return BSD_EPROTOTYPE;
    case WSAENOPROTOOPT: return BSD_ENOPROTOOPT;
    case WSAEPROTONOSUPPORT: return BSD_EPROTONOSUPPORT;
    case WSAEOPNOTSUPP: return BSD_EOPNOTSUPP;
    case WSAEAFNOSUPPORT: return BSD_EAFNOSUPPORT;
    case WSAEADDRINUSE: return BSD_EADDRINUSE;
    case WSAEADDRNOTAVAIL: return BSD_EADDRNOTAVAIL;
    case WSAENETDOWN: return BSD_ENETDOWN;
    case WSAENETUNREACH: return BSD_ENETUNREACH;
    case WSAENETRESET: return BSD_ENETRESET;
    case WSAECONNABORTED: return BSD_ECONNABORTED;
    case WSAECONNRESET: return BSD_ECONNRESET;
    case WSAENOBUFS: return BSD_ENOBUFS;
    case WSAEISCONN: return BSD_EISCONN;
    case WSAENOTCONN: return BSD_ENOTCONN;
    case WSAESHUTDOWN: return BSD_ESHUTDOWN;
    case WSAETIMEDOUT: return BSD_ETIMEDOUT;
    case WSAECONNREFUSED: return BSD_ECONNREFUSED;
    case WSAEHOSTDOWN: return BSD_EHOSTDOWN;
    case WSAEHOSTUNREACH: return BSD_EHOSTUNREACH;
    case WSAEINVAL: return BSD_EINVAL;
    case WSAEBADF: return BSD_EBADF;
    case WSAEACCES: return BSD_EACCES;
    case WSAEFAULT: return BSD_EFAULT;
    case WSAEMFILE: return BSD_EMFILE;
    case WSAEINTR: return BSD_EINTR;
    default: return BSD_EIO;
    }
}
