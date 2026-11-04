/* The host's port opened in the home router (UPnP), for players over the internet (netplay_router.c). */

#ifndef DEFJAM_NETPLAY_ROUTER_H
#define DEFJAM_NETPLAY_ROUTER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    NETPLAY_ROUTER_OFF = 0,     /* not asked, or left alone (RECOMP_NETPLAY_UPNP=0) */
    NETPLAY_ROUTER_TRYING,      /* finding the router and asking it */
    NETPLAY_ROUTER_OPEN,        /* forwarded: the text is the public address and port, or empty */
    NETPLAY_ROUTER_FAILED       /* not forwarded: the text says why */
};

/* Ask the router, in the background, to forward TCP `port` to this PC. */
void netplay_router_open(uint16_t port);

/* Take the forward away again, waiting for an ask still going. Safe to call at any time. */
void netplay_router_close(void);

/* Where that stands (NETPLAY_ROUTER_*), with its text copied into `text`. */
int netplay_router_status(char *text, size_t size);

/* The parts that read a router's replies, for tests: the LOCATION of an SSDP reply; the WAN
 * connection service in a device description and the absolute URL its commands go to; the text
 * of an element of a SOAP reply, whatever its namespace prefix; an HTTP body sent in chunks,
 * joined in place (returns its length). Each returns 1 when it found what it was after. */
int netplay_router_location(const char *reply, char *url, size_t size);
int netplay_router_control(const char *xml, const char *location, char *control, size_t control_size,
                           char *service, size_t service_size);
int netplay_router_value(const char *xml, const char *tag, char *out, size_t size);
size_t netplay_router_dechunk(char *data, size_t length);

#ifdef __cplusplus
}
#endif

#endif
