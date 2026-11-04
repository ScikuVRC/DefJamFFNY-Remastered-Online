/* A created fighter's profile under another name (netplay_saves.h).
 *
 * A profile file is "20CM", its size, the header size (0x40) and the payload size, then three
 * HMAC-SHA1 signatures under the console's signature key (the port's kernel exports zeros):
 * at 0x10 over the identity block [0x4C, 0x8C), at 0x24 over the payload [0x8C, end), at 0x38
 * over [0, 0x38). The payload starts with the profile's name, 20 bytes with its end. The same
 * layout as scripts/save-import.py, which checks a renamed file in the unit tests. */
#include "netplay_saves.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define HEADER          0x4C
#define IDENTITY_END    0x8C
#define NAME_FIELD      20

/* ---- SHA-1 and HMAC-SHA1 ------------------------------------------------------------ */
typedef struct {
    uint32_t h[5];
    uint64_t length;
    unsigned char block[64];
    size_t used;
} Sha1;

static uint32_t rol(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

static void sha1_block(Sha1 *s, const unsigned char *p)
{
    uint32_t w[80], a, b, c, d, e, t;
    int i;
    for (i = 0; i < 16; i++)
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    a = s->h[0]; b = s->h[1]; c = s->h[2]; d = s->h[3]; e = s->h[4];
    for (i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20)      { f = (b & c) | (~b & d);          k = 0x5A827999; }
        else if (i < 40) { f = b ^ c ^ d;                   k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
        else             { f = b ^ c ^ d;                   k = 0xCA62C1D6; }
        t = rol(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rol(b, 30); b = a; a = t;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e;
}

static void sha1_init(Sha1 *s)
{
    static const uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    memcpy(s->h, h, sizeof h);
    s->length = 0;
    s->used = 0;
}

static void sha1_update(Sha1 *s, const unsigned char *p, size_t n)
{
    s->length += n;
    while (n) {
        size_t take = 64 - s->used < n ? 64 - s->used : n;
        memcpy(s->block + s->used, p, take);
        s->used += take; p += take; n -= take;
        if (s->used == 64) { sha1_block(s, s->block); s->used = 0; }
    }
}

static void sha1_final(Sha1 *s, unsigned char out[20])
{
    uint64_t bits = s->length * 8;
    unsigned char pad = 0x80, zero = 0, len[8];
    int i;
    sha1_update(s, &pad, 1);
    while (s->used != 56) sha1_update(s, &zero, 1);
    for (i = 0; i < 8; i++) len[i] = (unsigned char)(bits >> (56 - 8 * i));
    sha1_update(s, len, 8);
    for (i = 0; i < 20; i++) out[i] = (unsigned char)(s->h[i / 4] >> (24 - 8 * (i % 4)));
}

/* HMAC-SHA1 under the signature key: 16 zero bytes, so every key byte is 0. */
static void hmac(const unsigned char *data, size_t n, unsigned char out[20])
{
    unsigned char pad[64], inner[20];
    Sha1 s;
    memset(pad, 0x36, sizeof pad);
    sha1_init(&s); sha1_update(&s, pad, 64); sha1_update(&s, data, n); sha1_final(&s, inner);
    memset(pad, 0x5C, sizeof pad);
    sha1_init(&s); sha1_update(&s, pad, 64); sha1_update(&s, inner, 20); sha1_final(&s, out);
}

static uint32_t le32(const unsigned char *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* ---- The save's name ------------------------------------------------------------------ */
void netplay_save_dir_name(const char *name, char out[13])
{
    static const char hex[] = "0123456789ABCDEF";
    uint64_t hash = 0;
    int i;
    for (; *name; name++)
        hash = (hash * UINT64_C(65536) + (unsigned char)*name) % UINT64_C(0xFFFFFFFFFFC5);
    for (i = 0; i < 12; i++) out[11 - i] = hex[(hash >> (4 * i)) & 15];
    out[12] = 0;
}

int netplay_rename_profile(unsigned char *data, size_t size, const char *name)
{
    size_t n = strlen(name);
    if (size <= IDENTITY_END + NAME_FIELD || memcmp(data, "20CM", 4) || le32(data + 4) != size
        || le32(data + 8) != 0x40 || le32(data + 12) != size - IDENTITY_END || !n || n >= NAME_FIELD)
        return 0;
    memset(data + IDENTITY_END, 0, NAME_FIELD);
    memcpy(data + IDENTITY_END, name, n);
    hmac(data + HEADER, IDENTITY_END - HEADER, data + 0x10);
    hmac(data + IDENTITY_END, size - IDENTITY_END, data + 0x24);
    hmac(data, 0x38, data + 0x38);
    return 1;
}

size_t netplay_save_meta(const char *name, unsigned char *out, size_t size)
{
    char text[64];
    size_t n, i;
    snprintf(text, sizeof text, "Name=%s\r\n", name);
    n = strlen(text);
    if (size < 2 + 2 * n) return 0;
    out[0] = 0xFF; out[1] = 0xFE;
    for (i = 0; i < n; i++) { out[2 + 2 * i] = (unsigned char)text[i]; out[3 + 2 * i] = 0; }
    return 2 + 2 * n;
}

void netplay_other_name(const char *name, int k, char *out, size_t size)
{
    char digits[12];
    size_t keep, d;
    snprintf(digits, sizeof digits, "%d", k);
    d = strlen(digits);
    keep = strlen(name);
    if (keep + d > NETPLAY_SAVE_NAME_MAX) keep = NETPLAY_SAVE_NAME_MAX - d;
    snprintf(out, size, "%.*s%s", (int)keep, name, digits);
}
