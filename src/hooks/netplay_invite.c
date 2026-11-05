/* Invite codes (netplay_invite.h).
 *
 * The bytes: a header (version 1 in the high four bits; bit 0, a public address follows; bit 1,
 * a home-network address follows), the port (big-endian), the addresses (four bytes each, public
 * first), then a CRC-8 of all of that. Written in Crockford base32, five bits a letter, in
 * groups of four after "DJ-". With both addresses that is 12 bytes, 20 letters. */
#include "netplay_invite.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define VERSION         1
#define HAS_PUBLIC      1
#define HAS_LAN         2

static const char k_alphabet[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

static unsigned char crc8(const unsigned char *p, size_t n)
{
    unsigned char c = 0;
    size_t i;
    int b;
    for (i = 0; i < n; i++) {
        c ^= p[i];
        for (b = 0; b < 8; b++) c = (unsigned char)(c & 0x80 ? (c << 1) ^ 0x07 : c << 1);
    }
    return c;
}

static int parse_ipv4(const char *text, unsigned char out[4])
{
    unsigned a, b, c, d;
    char end;
    if (!text || !*text || sscanf(text, "%u.%u.%u.%u%c", &a, &b, &c, &d, &end) != 4
        || a > 255 || b > 255 || c > 255 || d > 255)
        return 0;
    out[0] = (unsigned char)a; out[1] = (unsigned char)b; out[2] = (unsigned char)c; out[3] = (unsigned char)d;
    return 1;
}

int netplay_invite_make(uint16_t port, const char *public_ip, const char *lan_ip, char *out, size_t size)
{
    unsigned char bytes[12];
    size_t n = 3, bits = 0, letters = 0, o = 3, i;
    unsigned acc = 0;
    bytes[0] = VERSION << 4;
    bytes[1] = (unsigned char)(port >> 8);
    bytes[2] = (unsigned char)port;
    if (parse_ipv4(public_ip, bytes + n)) { bytes[0] |= HAS_PUBLIC; n += 4; }
    if (parse_ipv4(lan_ip, bytes + n)) { bytes[0] |= HAS_LAN; n += 4; }
    if (!(bytes[0] & (HAS_PUBLIC | HAS_LAN)) || !port) return 0;
    bytes[n] = crc8(bytes, n);
    n++;
    {
        size_t total = (n * 8 + 4) / 5;     /* letters */
        if (size < 3 + total + (total - 1) / 4 + 1) return 0;
    }
    memcpy(out, "DJ-", 3);
    for (i = 0; i < n; i++) {
        acc = (acc << 8) | bytes[i];
        bits += 8;
        while (bits >= 5) {
            bits -= 5;
            if (letters && letters % 4 == 0) out[o++] = '-';
            out[o++] = k_alphabet[(acc >> bits) & 31];
            letters++;
        }
    }
    if (bits) {
        if (letters % 4 == 0) out[o++] = '-';
        out[o++] = k_alphabet[(acc << (5 - bits)) & 31];
    }
    out[o] = 0;
    return 1;
}

static int letter_value(int c)
{
    const char *at;
    c = toupper(c);
    if (c == 'O') c = '0';
    if (c == 'I' || c == 'L') c = '1';
    at = strchr(k_alphabet, c);
    return c && at ? (int)(at - k_alphabet) : -1;
}

int netplay_invite_read(const char *text, char out[][32], int max)
{
    unsigned char bytes[16];
    size_t n = 0, bits = 0, need;
    unsigned acc = 0, port;
    const char *p = text;
    int count = 0, letters = 0;
    if (!text) return 0;
    while (*p == ' ') p++;
    if ((p[0] == 'D' || p[0] == 'd') && (p[1] == 'J' || p[1] == 'j')) p += 2;
    for (; *p; p++) {
        int v;
        if (*p == '-' || *p == ' ') continue;
        if ((v = letter_value((unsigned char)*p)) < 0) return 0;     /* an address, not a code */
        if (++letters > 26) return 0;
        acc = (acc << 5) | (unsigned)v;
        bits += 5;
        if (bits >= 8) {
            bits -= 8;
            bytes[n++] = (unsigned char)(acc >> bits);
        }
    }
    if (n < 4 || (bytes[0] >> 4) != VERSION || !(bytes[0] & (HAS_PUBLIC | HAS_LAN)) || (bytes[0] & 0x0C)) return 0;
    need = 3 + ((bytes[0] & HAS_PUBLIC) ? 4 : 0) + ((bytes[0] & HAS_LAN) ? 4 : 0);
    if (n != need + 1 || crc8(bytes, need) != bytes[need]) return 0;
    port = (unsigned)bytes[1] << 8 | bytes[2];
    if (!port) return 0;
    /* The home-network address first: a friend on the same network may not get back in
     * through the router's public address. */
    if ((bytes[0] & HAS_LAN) && count < max) {
        const unsigned char *a = bytes + 3 + ((bytes[0] & HAS_PUBLIC) ? 4 : 0);
        snprintf(out[count++], 32, "%u.%u.%u.%u:%u", a[0], a[1], a[2], a[3], port);
    }
    if ((bytes[0] & HAS_PUBLIC) && count < max) {
        const unsigned char *a = bytes + 3;
        snprintf(out[count++], 32, "%u.%u.%u.%u:%u", a[0], a[1], a[2], a[3], port);
    }
    return count;
}
