//
// Created by YuQiang on 2017-09-29.
//

#include <stdint.h>
#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <assert.h>
#include <fcntl.h>

#include <linux/if_tun.h>
#include <sys/ioctl.h>
#include <net/if.h>

#include "inc.h"
#include "log.h"
#include "event.h"
#include "util.h"
#include "auth.h"
#include "tun.h"

#ifdef DEBUG_TUN
#define debugTun(s...) __LOG(stdout, DEBUG_TUN, s)
#else
#define debugTun(s...)
#endif

// tun 操作的上下文
static struct {
    int isServer;
    // device
    const char *device;
    // file descriptor
    int tunFd;
    // event
    uev_t tunEvent;
    // buffer
    char dataBuffer[DATA_BUFFER_SIZE];

} _tunCtx;

// tun transport
static struct itransport _tunTransport;
static int _isTunInit = 0;

/**
 *
 * @param buf
 * @param len
 * @return
 */
static ssize_t tunWrteData(char *buf, size_t len, void *context) {
    assert(0 != _tunCtx.tunFd);
    return write(_tunCtx.tunFd, buf, len);
}

/**
 *
 * @param buf
 * @param len
 * @return
 */
static ssize_t tunReadData(char *buf, size_t len, void *context) {
    assert(0 != _tunCtx.tunFd);
    return read(_tunCtx.tunFd, buf, len);
}


/*
   RFC791
   0                   1                   2                   3
   0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
   |Version|  IHL  |Type of Service|          Total Length         |
   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
   |         Identification        |Flags|      Fragment Offset    |
   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
   |  Time to Live |    Protocol   |         Header Checksum       |
   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
   |                       Source Address                          |
   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
   |                    Destination Address                        |
   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
   |                    Options                    |    Padding    |
   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
*/
typedef struct {
    uint8_t ver;
    uint8_t tos;
    uint16_t total_len;
    uint16_t id;
    uint16_t frag;
    uint8_t ttl;
    uint8_t proto;
    uint16_t checksum;
    uint32_t saddr;
    uint32_t daddr;
} ipv4_hdr_t;


/**
 *  on read event
 *
 * @param w
 * @param arg
 * @param events
 */
static void tunOnRead(uev_t *w, void *arg, int events) {
    ssize_t totalSize = 0;
    ssize_t readSize = 0;
    client_info_t *client = NULL;
    while ((readSize = tunReadData(_tunCtx.dataBuffer, DATA_BUFFER_SIZE, NULL)) > 0) {
        totalSize += readSize;
        debugTun("tun read data [%d] bytes {%s}", readSize, log_hex_memory_32_bytes(_tunCtx.dataBuffer));

        ipv4_hdr_t *iphdr = (ipv4_hdr_t *) _tunCtx.dataBuffer;
        if ((iphdr->ver & 0xf0) != 0x40) {
            // check header, currently IPv4 only
            // bypass IPv6
            continue;
        }
        uint32_t tunIp = 0;
        if (_tunCtx.isServer) {
            tunIp = ntohl(iphdr->daddr);
        } else {
            tunIp = ntohl(iphdr->saddr);
        }

        debugTun("tunOnRead  tunip : [%s] \n", ipToString(tunIp));

        client = sectunAuthFindClientByTunIp(tunIp);
        if (NULL == client) {
            errf("tunip : [%s] find no client\n", ipToString(tunIp));
            continue;
        }

        _tunTransport.forwardRead(_tunCtx.dataBuffer, readSize, client);
    }

    if (totalSize > 0 && NULL != _tunTransport.forwardReadFinish) {
        _tunTransport.forwardReadFinish(totalSize, client);
    }
}

/**
 * start tun device
 *
 * @return
 */
static int tunStart() {

    if (_tunCtx.tunFd > 0) {
        errf("tun already open Fd [%d]", _tunCtx.tunFd);
        return -1;
    }

    struct ifreq ifr;

    if ((_tunCtx.tunFd = open("/dev/net/tun", O_RDWR)) < 0) {
        err("open");
        errf("can not open /dev/net/tun");
        return -1;
    }

    memset(&ifr, 0, sizeof(ifr));

    /* Flags: IFF_TUN   - TUN device (no Ethernet headers)
     *        IFF_TAP   - TAP device
     *
     *        IFF_NO_PI - Do not provide packet information
     */
    ifr.ifr_flags = IFF_TUN | IFF_NO_PI;

    strncpy(ifr.ifr_name, _tunCtx.device, IFNAMSIZ);

    int e;
    if ((e = ioctl(_tunCtx.tunFd, TUNSETIFF, (void *) &ifr)) < 0) {
        err("ioctl[TUNSETIFF]");
        errf("can not setup tun device: %s", _tunCtx.device);
        close(_tunCtx.tunFd);
        _tunCtx.tunFd = 0;
        return -1;
    }

    int tunFlag = -1;
    if ((tunFlag = fcntl(_tunCtx.tunFd, F_GETFL, 0)) < 0
        || fcntl(_tunCtx.tunFd, F_SETFL, tunFlag | O_NONBLOCK) < 0) {
        errf("can not set tun to nonblock");
        return -1;
    }

    // setup event handle
    const sectun_event_t *const pEvent = sectunGetEventInstance();
    pEvent->io_init(&_tunCtx.tunEvent, tunOnRead, NULL, _tunCtx.tunFd, UEV_READ);
    pEvent->io_start(&_tunCtx.tunEvent);

    return 0;
}

/**
 * stop tun device
 */
static int tunStop() {

    if (_tunCtx.tunFd <= 0) {
        errf("tun already close Fd [%d]", _tunCtx.tunFd);
        return -1;
    }

    // stop event
    const sectun_event_t *const pEvent = sectunGetEventInstance();
    pEvent->io_stop(&_tunCtx.tunEvent);

    // close file
    close(_tunCtx.tunFd);
    _tunCtx.tunFd = 0;
}

/**
 *
 * @param transport
 */
static void tunSetNextLayerTransport(struct itransport *transport) {
    struct itransport *pTunTransport = sectunGetTunTransport();
    pTunTransport->forwardRead = transport->writeData;
    pTunTransport->forwardReadFinish = transport->forwardWriteFinish;
    transport->forwardRead = pTunTransport->writeData;
}

/**
 *  init tun device
 * @param dev  device name
 * @return
 */
int sectunTunInit(const char *dev, int isServer) {

    assert(NULL != dev);

    memset(&_tunCtx, 0, sizeof(_tunCtx));
    _tunCtx.isServer = isServer;
    _tunCtx.device = dev;

    // init tun transport
    _tunTransport = __dummyTransport;

    _tunTransport.readData = tunReadData;
    _tunTransport.writeData = tunWrteData;
    _tunTransport.start = tunStart;
    _tunTransport.stop = tunStop;
    _tunTransport.setNextLayer = tunSetNextLayerTransport;

    // finish init
    _isTunInit = 1;
    return 0;
}

/**
 *
 * @return  tunTransport
 */
struct itransport *const sectunGetTunTransport() {
    assert(_isTunInit > 0);
    return &_tunTransport;
}


