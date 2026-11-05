/*
 * netplay_router.c - the host's port opened in the home router, so players over the internet
 * can join a lobby without anyone setting up port forwarding by hand.
 *
 * UPnP's Internet Gateway Device, which most home routers offer: an SSDP search (a UDP
 * multicast to 239.255.255.250:1900) finds the router; its description names the WAN
 * connection service and the URL that takes its commands; AddPortMapping forwards the lobby's
 * TCP port to this PC, and GetExternalIPAddress gives the address the others type.
 * DeletePortMapping takes the forward away when the session ends, and it lapses by itself after
 * four hours if the game never gets to (a crash). Nothing here can help when the router is
 * itself behind another NAT, which its public address shows (a private or carrier-shared
 * one): the status says so instead.
 *
 * RECOMP_NETPLAY_UPNP=0 leaves the router alone, and so do test runs (RECOMP_SETTINGS=none)
 * unless it is 1. RECOMP_NETPLAY_UPNP_LOCATION=<description URL> skips the search.
 */

#define _CRT_SECURE_NO_WARNINGS
#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
typedef SOCKET sock_t;
#define SOCK_BAD INVALID_SOCKET
#define sock_close closesocket
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
typedef int sock_t;
#define SOCK_BAD (-1)
#define sock_close close
#endif
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "netplay_router.h"

#define LEASE       14400       /* seconds: the forward lapses after four hours */
#define WAIT_MS     3000        /* for a connection or a reply */
#define REPLY_MAX   65536

static volatile int s_state = NETPLAY_ROUTER_OFF;
static char s_text[256];                        /* written before s_state says it is final */
static char s_control[512], s_service[128];     /* where the commands go, and as what */
static char s_local[48];                        /* this PC, as the router sees it */
static unsigned s_port;
static int s_mapped;

#if defined(_WIN32)
static HANDLE s_thread;
#else
static pthread_t s_thread;
static int s_thread_live;
#endif

/* ---- Text ---------------------------------------------------------------------------- */
static int same_ci(const char *a, const char *b, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++)
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return 0;
    return 1;
}

static int contains(const char *s, size_t n, const char *needle)
{
    size_t k = strlen(needle), i;
    for (i = 0; i + k <= n; i++)
        if (!memcmp(s + i, needle, k)) return 1;
    return 0;
}

/* `n` characters of `s` into `out`, without the white space around them. */
static int copy_trimmed(char *out, size_t size, const char *s, size_t n)
{
    while (n && isspace((unsigned char)*s)) { s++; n--; }
    while (n && isspace((unsigned char)s[n - 1])) n--;
    if (!size || n >= size) return 0;
    memcpy(out, s, n);
    out[n] = 0;
    return 1;
}

int netplay_router_location(const char *reply, char *url, size_t size)
{
    const char *p = reply;
    while (p && *p) {
        if (same_ci(p, "location:", 9)) {
            p += 9;
            return copy_trimmed(url, size, p, strcspn(p, "\r\n"));
        }
        p = strchr(p, '\n');
        if (p) p++;
    }
    return 0;
}

/* The text of the first <tag>...</tag> between `from` and `to`; its length into *n. */
static const char *element(const char *from, const char *to, const char *tag, size_t *n)
{
    char open[64], close[64];
    const char *a, *b;
    snprintf(open, sizeof open, "<%s>", tag);
    snprintf(close, sizeof close, "</%s>", tag);
    a = strstr(from, open);
    if (!a || a >= to) return NULL;
    a += strlen(open);
    b = strstr(a, close);
    if (!b || b > to) return NULL;
    *n = (size_t)(b - a);
    return a;
}

/* `url` made absolute: against the description's <URLBase>, else where it was fetched from. */
static int absolute(char *out, size_t size, const char *xml, const char *location, const char *url, size_t n)
{
    char path[512], base[512];
    const char *b, *host;
    size_t bn;
    if (!copy_trimmed(path, sizeof path, url, n)) return 0;
    if (!strncmp(path, "http://", 7)) return snprintf(out, size, "%s", path) < (int)size;
    b = element(xml, xml + strlen(xml), "URLBase", &bn);
    if (!b || !copy_trimmed(base, sizeof base, b, bn) || strncmp(base, "http://", 7))
        snprintf(base, sizeof base, "%s", location);
    host = base + 7;
    base[7 + strcspn(host, "/")] = 0;           /* http://host:port */
    return snprintf(out, size, "%s%s%s", base, path[0] == '/' ? "" : "/", path) < (int)size;
}

int netplay_router_control(const char *xml, const char *location, char *control, size_t control_size,
                           char *service, size_t service_size)
{
    static const char *const wanted[] = {"WANIPConnection:", "WANPPPConnection:"};
    int k;
    for (k = 0; k < 2; k++) {
        const char *p = xml;
        while ((p = strstr(p, "<service>")) != NULL) {
            const char *end = strstr(p, "</service>"), *type, *url;
            size_t tn, un;
            if (!end) break;
            type = element(p, end, "serviceType", &tn);
            url = element(p, end, "controlURL", &un);
            if (type && url && contains(type, tn, wanted[k]) && copy_trimmed(service, service_size, type, tn))
                return absolute(control, control_size, xml, location, url, un);
            p = end;
        }
    }
    return 0;
}

int netplay_router_value(const char *xml, const char *tag, char *out, size_t size)
{
    const char *p = xml;
    size_t n = strlen(tag);
    while ((p = strchr(p, '<')) != NULL) {
        const char *name = ++p, *end = p + strcspn(p, " />"), *colon, *v, *stop;
        if (*name == '/' || *name == '?' || *name == '!') continue;
        colon = memchr(name, ':', (size_t)(end - name));
        if (colon) name = colon + 1;
        if ((size_t)(end - name) != n || memcmp(name, tag, n)) continue;
        v = strchr(end, '>');
        if (!v) return 0;
        if (v[-1] == '/') return copy_trimmed(out, size, "", 0);     /* <tag/> */
        stop = strchr(++v, '<');
        return stop && copy_trimmed(out, size, v, (size_t)(stop - v));
    }
    return 0;
}

size_t netplay_router_dechunk(char *data, size_t length)
{
    size_t in = 0, out = 0;
    while (in < length) {
        char *end;
        unsigned long chunk = strtoul(data + in, &end, 16);
        const char *line = memchr(data + in, '\n', length - in);  /* the size line can carry extensions */
        if (!line || end == data + in) break;
        in = (size_t)(line - data) + 1;
        if (!chunk) break;                                          /* the last one */
        if (chunk > length - in) chunk = length - in;               /* cut short */
        memmove(data + out, data + in, chunk);
        out += chunk;
        in += chunk;
        if (in < length && data[in] == '\r') in++;
        if (in < length && data[in] == '\n') in++;
    }
    data[out < length ? out : length] = 0;
    return out;
}

/* ---- The network --------------------------------------------------------------------- */
static void timeouts(sock_t s, int ms)
{
#if defined(_WIN32)
    DWORD t = (DWORD)ms;
#else
    struct timeval t;
    t.tv_sec = ms / 1000;
    t.tv_usec = (ms % 1000) * 1000;
#endif
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&t, sizeof t);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&t, sizeof t);
}

/* connect(), giving up after WAIT_MS rather than the system's twenty seconds and more. */
static int connect_wait(sock_t s, const struct sockaddr *to, int length)
{
    struct timeval t = {WAIT_MS / 1000, 0};
    fd_set w;
    int error = 0, ok;
#if defined(_WIN32)
    u_long on = 1, off = 0;
    int size = sizeof error;
    ioctlsocket(s, FIONBIO, &on);
#else
    socklen_t size = sizeof error;
    int flags = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, flags | O_NONBLOCK);
#endif
    ok = connect(s, to, length) == 0;
    if (!ok) {
        FD_ZERO(&w);
        FD_SET(s, &w);
        ok = select((int)s + 1, NULL, &w, NULL, &t) == 1 &&
             getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&error, &size) == 0 && error == 0;
    }
#if defined(_WIN32)
    ioctlsocket(s, FIONBIO, &off);
#else
    fcntl(s, F_SETFL, flags);
#endif
    return ok;
}

/* http://host[:port]/path */
static int split_url(const char *url, char *host, size_t host_size, unsigned *port, const char **path)
{
    const char *p = url + 7, *end, *colon;
    size_t n;
    if (strncmp(url, "http://", 7)) return 0;
    end = p + strcspn(p, "/");
    colon = memchr(p, ':', (size_t)(end - p));
    n = (size_t)((colon ? colon : end) - p);
    if (!n || n >= host_size) return 0;
    memcpy(host, p, n);
    host[n] = 0;
    *port = colon ? (unsigned)atoi(colon + 1) : 80;
    *path = *end ? end : "/";
    return *port > 0 && *port < 65536;
}

static int resolve(const char *host, unsigned port, int type, struct sockaddr_in *out)
{
    struct addrinfo hints, *found = NULL;
    char service[8];
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = type;
    snprintf(service, sizeof service, "%u", port);
    if (getaddrinfo(host, service, &hints, &found) != 0 || !found) return 0;
    memcpy(out, found->ai_addr, sizeof *out);
    freeaddrinfo(found);
    return 1;
}

static int has_header(const char *head, size_t length, const char *name, const char *value)
{
    size_t n = strlen(name), v = strlen(value), i, j;
    for (i = 0; i + n < length; i++) {
        if ((i && head[i - 1] != '\n') || !same_ci(head + i, name, n) || head[i + n] != ':') continue;
        for (j = i + n + 1; j + v <= length && head[j] != '\r' && head[j] != '\n'; j++)
            if (same_ci(head + j, value, v)) return 1;
    }
    return 0;
}

/* One HTTP/1.1 exchange that closes its connection: the reply's body into `body`, joined
 * from chunks when it came in them. Returns the status code, or -1 for no reply. */
static int http(const char *url, const char *method, const char *headers, const char *payload,
                char *body, size_t size)
{
    struct sockaddr_in to;
    char host[128], *reply, *start;
    const char *path;
    unsigned port;
    size_t got = 0, length = payload ? strlen(payload) : 0;
    int status = -1, n;
    sock_t s;
    if (size) body[0] = 0;
    if (!split_url(url, host, sizeof host, &port, &path) || !resolve(host, port, SOCK_STREAM, &to)) return -1;
    if ((s = socket(AF_INET, SOCK_STREAM, 0)) == SOCK_BAD) return -1;
    timeouts(s, WAIT_MS);
    reply = (char *)malloc(REPLY_MAX + 1);
    if (reply && connect_wait(s, (struct sockaddr *)&to, sizeof to)) {
        n = payload ? snprintf(reply, REPLY_MAX, "%s %s HTTP/1.1\r\nHost: %s:%u\r\nConnection: close\r\n"
                                                 "Content-Length: %u\r\n%s\r\n",
                               method, path, host, port, (unsigned)length, headers ? headers : "")
                    : snprintf(reply, REPLY_MAX, "%s %s HTTP/1.1\r\nHost: %s:%u\r\nConnection: close\r\n%s\r\n",
                               method, path, host, port, headers ? headers : "");
        if (n > 0 && send(s, reply, n, 0) == n && (!length || send(s, payload, (int)length, 0) == (int)length))
            while (got < REPLY_MAX && (n = recv(s, reply + got, (int)(REPLY_MAX - got), 0)) > 0)
                got += (size_t)n;
    }
    sock_close(s);
    if (!reply) return -1;
    reply[got] = 0;
    if (got && sscanf(reply, "HTTP/%*d.%*d %d", &status) == 1 && (start = strstr(reply, "\r\n\r\n")) != NULL) {
        size_t head = (size_t)(start - reply), rest;
        start += 4;
        rest = got - (size_t)(start - reply);
        if (has_header(reply, head, "transfer-encoding", "chunked"))
            rest = netplay_router_dechunk(start, rest);
        if (size) {
            if (rest >= size) rest = size - 1;
            memcpy(body, start, rest);
            body[rest] = 0;
        }
    } else {
        status = -1;
    }
    free(reply);
    return status;
}

/* This PC's IPv4 addresses, loopback aside. */
static int local_ipv4(struct in_addr *out, int max)
{
    char host[256];
    struct addrinfo hints, *found = NULL, *a;
    int n = 0;
    if (gethostname(host, sizeof host) != 0) return 0;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    if (getaddrinfo(host, NULL, &hints, &found) != 0) return 0;
    for (a = found; a && n < max; a = a->ai_next) {
        struct in_addr address = ((struct sockaddr_in *)a->ai_addr)->sin_addr;
        if ((ntohl(address.s_addr) >> 24) != 127) out[n++] = address;
    }
    freeaddrinfo(found);
    return n;
}

/* The router's description URL, from the first internet gateway that answers a search. */
static int discover(char *location, size_t size)
{
    static const char *const targets[] = {
        "urn:schemas-upnp-org:device:InternetGatewayDevice:1", "urn:schemas-upnp-org:service:WANIPConnection:1",
        "urn:schemas-upnp-org:service:WANPPPConnection:1", "urn:schemas-upnp-org:device:InternetGatewayDevice:2"};
    struct sockaddr_in to;
    struct in_addr adapters[8];
    char buffer[1500];
    int round, i, k, count, waited, found = 0;
    sock_t s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s == SOCK_BAD) return 0;
    timeouts(s, 500);
    memset(&to, 0, sizeof to);
    to.sin_family = AF_INET;
    to.sin_port = htons(1900);
    inet_pton(AF_INET, "239.255.255.250", &to.sin_addr);
    /* Out of every adapter: a multicast leaves by one only, the system's choice, and with a VPN
     * or a virtual switch installed that is often not the one the router is on (2026-11-03: a
     * VPN adapter with the lowest metric took them all). Twice: a search can be lost. */
    count = local_ipv4(adapters, 8);
    for (round = 0; round < 2 && !found; round++) {
        for (k = 0; k < (count ? count : 1); k++) {
            if (count)
                setsockopt(s, IPPROTO_IP, IP_MULTICAST_IF, (const char *)&adapters[k], sizeof adapters[k]);
            for (i = 0; i < 4; i++) {
                int n = snprintf(buffer, sizeof buffer, "M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\n"
                                                        "MAN: \"ssdp:discover\"\r\nMX: 2\r\nST: %s\r\n\r\n", targets[i]);
                sendto(s, buffer, n, 0, (struct sockaddr *)&to, sizeof to);
            }
        }
        for (waited = 0; waited < 2500 && !found; waited += 500) {
            int n;
            while (!found && (n = recv(s, buffer, sizeof buffer - 1, 0)) > 0) {
                buffer[n] = 0;
                if (strstr(buffer, "InternetGatewayDevice") || strstr(buffer, "WANIPConnection") ||
                    strstr(buffer, "WANPPPConnection"))
                    found = netplay_router_location(buffer, location, size);
            }
        }
    }
    sock_close(s);
    return found;
}

/* This PC's address on the way to `host`: the one the router sees, and forwards to. */
static int local_address(const char *host, char *out, size_t size)
{
    struct sockaddr_in to, me;
    socklen_t n = sizeof me;
    sock_t s;
    int ok;
    if (!resolve(host, 1900, SOCK_DGRAM, &to) || (s = socket(AF_INET, SOCK_DGRAM, 0)) == SOCK_BAD) return 0;
    ok = connect(s, (struct sockaddr *)&to, sizeof to) == 0 && getsockname(s, (struct sockaddr *)&me, &n) == 0 &&
         inet_ntop(AF_INET, &me.sin_addr, out, (socklen_t)size) != NULL;
    sock_close(s);
    return ok;
}

/* ---- The commands ---------------------------------------------------------------------- */
static int soap(const char *action, const char *args, char *reply, size_t size)
{
    char body[2048], headers[384];
    snprintf(body, sizeof body,
             "<?xml version=\"1.0\"?>\r\n<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
             "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body><u:%s xmlns:u=\"%s\">%s</u:%s>"
             "</s:Body></s:Envelope>\r\n", action, s_service, args, action);
    snprintf(headers, sizeof headers, "Content-Type: text/xml; charset=\"utf-8\"\r\nSOAPAction: \"%s#%s\"\r\n",
             s_service, action);
    return http(s_control, "POST", headers, body, reply, size);
}

static int error_code(const char *reply)
{
    char code[16];
    return netplay_router_value(reply, "errorCode", code, sizeof code) ? atoi(code) : 0;
}

static int add_mapping(unsigned lease, char *reply, size_t size)
{
    char args[640];
    snprintf(args, sizeof args,
             "<NewRemoteHost></NewRemoteHost><NewExternalPort>%u</NewExternalPort><NewProtocol>TCP</NewProtocol>"
             "<NewInternalPort>%u</NewInternalPort><NewInternalClient>%s</NewInternalClient><NewEnabled>1</NewEnabled>"
             "<NewPortMappingDescription>Def Jam online</NewPortMappingDescription>"
             "<NewLeaseDuration>%u</NewLeaseDuration>", s_port, s_port, s_local, lease);
    return soap("AddPortMapping", args, reply, size);
}

static int delete_mapping(void)
{
    char args[256], reply[1024];
    snprintf(args, sizeof args, "<NewRemoteHost></NewRemoteHost><NewExternalPort>%u</NewExternalPort>"
                                "<NewProtocol>TCP</NewProtocol>", s_port);
    return soap("DeletePortMapping", args, reply, sizeof reply) == 200;
}

static void finish(int state, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vsnprintf(s_text, sizeof s_text, format, args);
    va_end(args);
    s_state = state;
    fprintf(stderr, "[NETPLAY] router: %s%s\n", state == NETPLAY_ROUTER_OPEN ? "port open, " : "", s_text);
    fflush(stderr);
}

/* An address the internet cannot reach: private, or a carrier's shared one (100.64.0.0/10). */
static int behind_another_nat(const char *address)
{
    unsigned a = 0, b = 0;
    if (sscanf(address, "%u.%u", &a, &b) != 2) return 0;
    return a == 10 || (a == 172 && b >= 16 && b < 32) || (a == 192 && b == 168) || (a == 100 && b >= 64 && b < 128) ||
           a == 0;
}

static void open_at(const char *location)
{
    char *xml = (char *)malloc(REPLY_MAX + 1), reply[4096], host[128], external[64], client[64];
    const char *path;
    unsigned port;
    int status, code;
    if (!xml) { finish(NETPLAY_ROUTER_FAILED, "out of memory"); return; }
    status = http(location, "GET", NULL, NULL, xml, REPLY_MAX + 1);
    if (status != 200 || !netplay_router_control(xml, location, s_control, sizeof s_control, s_service, sizeof s_service)) {
        free(xml);
        finish(NETPLAY_ROUTER_FAILED, "your router answered, but offers no internet connection service to ask");
        return;
    }
    free(xml);
    if (!split_url(s_control, host, sizeof host, &port, &path) || !local_address(host, s_local, sizeof s_local)) {
        finish(NETPLAY_ROUTER_FAILED, "could not tell this PC's address towards the router");
        return;
    }
    status = add_mapping(LEASE, reply, sizeof reply);
    if (status != 200 && error_code(reply) == 725)             /* OnlyPermanentLeasesSupported */
        status = add_mapping(0, reply, sizeof reply);
    if (status != 200 && error_code(reply) == 718) {           /* ConflictInMappingEntry: whose? */
        char args[256];
        snprintf(args, sizeof args, "<NewRemoteHost></NewRemoteHost><NewExternalPort>%u</NewExternalPort>"
                                    "<NewProtocol>TCP</NewProtocol>", s_port);
        if (soap("GetSpecificPortMappingEntry", args, reply, sizeof reply) == 200 &&
            netplay_router_value(reply, "NewInternalClient", client, sizeof client) && strcmp(client, s_local)) {
            finish(NETPLAY_ROUTER_FAILED, "port %u is already forwarded to another device on your network (%s)",
                   s_port, client);
            return;
        }
        delete_mapping();                                       /* this PC's, left by an earlier run */
        status = add_mapping(LEASE, reply, sizeof reply);
    }
    if (status != 200) {
        char why[128];
        code = error_code(reply);
        if (status < 0)
            finish(NETPLAY_ROUTER_FAILED, "your router stopped answering");
        else if (netplay_router_value(reply, "errorDescription", why, sizeof why) && why[0])
            finish(NETPLAY_ROUTER_FAILED, "your router refused to open port %u (%s, %d)", s_port, why, code);
        else
            finish(NETPLAY_ROUTER_FAILED, "your router refused to open port %u (HTTP %d)", s_port, status);
        return;
    }
    s_mapped = 1;
    if (soap("GetExternalIPAddress", "", reply, sizeof reply) != 200 ||
        !netplay_router_value(reply, "NewExternalIPAddress", external, sizeof external) || !external[0]) {
        finish(NETPLAY_ROUTER_OPEN, "");
        return;
    }
    if (behind_another_nat(external)) {
        /* The forward reaches only the network outside this router, not the internet. */
        delete_mapping();
        s_mapped = 0;
        finish(NETPLAY_ROUTER_FAILED, "your router is behind another one (its own address is %s): your provider "
                                      "or another router of yours shares one public address", external);
        return;
    }
    finish(NETPLAY_ROUTER_OPEN, "%s:%u", external, s_port);
}

static void run(void)
{
    char location[512];
    const char *fixed = getenv("RECOMP_NETPLAY_UPNP_LOCATION");
    if (fixed && *fixed)
        snprintf(location, sizeof location, "%s", fixed);
    else if (!discover(location, sizeof location)) {
        finish(NETPLAY_ROUTER_FAILED, "no router answered a UPnP search (it may be switched off in the router)");
        return;
    }
    open_at(location);
}

#if defined(_WIN32)
static DWORD WINAPI worker(LPVOID arg) { (void)arg; run(); return 0; }
#else
static void *worker(void *arg) { (void)arg; run(); return NULL; }
#endif

static void at_exit(void)
{
    netplay_router_close();
}

void netplay_router_open(uint16_t port)
{
    static int registered;
    const char *e = getenv("RECOMP_NETPLAY_UPNP"), *settings = getenv("RECOMP_SETTINGS");
    netplay_router_close();
    s_port = port;
    s_text[0] = 0;
    if (e && *e ? *e == '0' : settings && !strcmp(settings, "none")) {
        s_state = NETPLAY_ROUTER_OFF;
        return;
    }
    if (!registered) {
        registered = 1;
        atexit(at_exit);
    }
    s_state = NETPLAY_ROUTER_TRYING;
#if defined(_WIN32)
    s_thread = CreateThread(NULL, 0, worker, NULL, 0, NULL);
    if (!s_thread)
#else
    s_thread_live = pthread_create(&s_thread, NULL, worker, NULL) == 0;
    if (!s_thread_live)
#endif
        finish(NETPLAY_ROUTER_FAILED, "could not start asking the router");
}

void netplay_router_close(void)
{
#if defined(_WIN32)
    if (s_thread) {
        WaitForSingleObject(s_thread, 15000);
        CloseHandle(s_thread);
        s_thread = NULL;
    }
#else
    if (s_thread_live) {
        pthread_join(s_thread, NULL);
        s_thread_live = 0;
    }
#endif
    if (s_mapped) {
        int gone = delete_mapping();
        s_mapped = 0;
        fprintf(stderr, "[NETPLAY] router: port %u %s\n", s_port, gone ? "closed again" : "may still be open (the router did not answer)");
        fflush(stderr);
    }
    s_state = NETPLAY_ROUTER_OFF;
}

/* This PC's address on the network the router is on, once the ask has ended ("" before, or when
 * no router answered): the home-network part of an invite code (netplay_invite.h). */
void netplay_router_local(char *text, size_t size)
{
    int state = s_state;
    if (size)
        snprintf(text, size, "%s", state == NETPLAY_ROUTER_OPEN || state == NETPLAY_ROUTER_FAILED ? s_local : "");
}

int netplay_router_status(char *text, size_t size)
{
    int state = s_state;
    if (size)
        snprintf(text, size, "%s", state == NETPLAY_ROUTER_OPEN || state == NETPLAY_ROUTER_FAILED ? s_text : "");
    return state;
}
