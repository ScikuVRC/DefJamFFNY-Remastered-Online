// Fighter portraits for the launcher, read from the player's own copy of the game
// (pc_launcher_game_art.h). screens/screens.viv is a BIG archive; its entry
// screens/portrait/<fighter + 1>_2p.big is RefPack-compressed, a BIG holding an .xsh (EA's SHPX
// image list) whose first image is a 512x512 DXT atlas. The fighter's card, the render with the
// name logo at its top right, is the atlas's top left 281x377 (the portrait's .apt gives that size).
#include "pc_launcher_game_art.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace
{
using Bytes = std::vector<uint8_t>;
const int CARD_W = 281, CARD_H = 377;

uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0];
}
uint16_t le16(const uint8_t *p)
{
    return (uint16_t)(p[1] << 8 | p[0]);
}

struct Entry
{
    uint32_t offset, size;
};

// A BIG archive's directory: name -> where.
bool big_directory(const uint8_t *p, size_t n, std::map<std::string, Entry> &out)
{
    if (n < 16 || (memcmp(p, "BIGF", 4) && memcmp(p, "BIG4", 4)))
        return false;
    uint32_t count = be32(p + 8);
    size_t pos = 16;
    for (uint32_t i = 0; i < count && count < 100000; i++)
    {
        if (pos + 9 > n)
            return false;
        const void *z = memchr(p + pos + 8, 0, n - pos - 8);
        if (!z)
            return false;
        std::string name((const char *)p + pos + 8, (const char *)z);
        out[name] = {be32(p + pos), be32(p + pos + 4)};
        pos = (size_t)((const uint8_t *)z - p) + 1;
    }
    return true;
}

// EA's RefPack (the same format pc_language.c reads).
bool refpack(const uint8_t *src, size_t n, Bytes &out)
{
    if (n < 5 || ((src[0] << 8 | src[1]) & 0x3EFF) != 0x10FB)
        return false;
    unsigned sig = (unsigned)(src[0] << 8 | src[1]);
    size_t pos = 2, step = (sig & 0x8000) ? 4 : 3, size = 0;
    if (sig & 0x0100)
        pos += step;
    if (pos + step > n)
        return false;
    for (size_t i = 0; i < step; i++)
        size = size << 8 | src[pos + i];
    pos += step;
    if (size > (64u << 20))
        return false;
    out.assign(size + 4, 0);
    size_t at = 0;
    while (pos < n)
    {
        unsigned b0 = src[pos++], b1, b2, b3;
        size_t run, length = 0, dist = 0;
        if (b0 < 0x80)
        {
            if (pos + 1 > n)
                break;
            b1 = src[pos++];
            run = b0 & 3;
            length = ((b0 & 0x1C) >> 2) + 3;
            dist = ((b0 & 0x60) << 3) + b1 + 1;
        }
        else if (b0 < 0xC0)
        {
            if (pos + 2 > n)
                break;
            b1 = src[pos];
            b2 = src[pos + 1];
            pos += 2;
            run = (b1 >> 6) & 3;
            length = (b0 & 0x3F) + 4;
            dist = ((b1 & 0x3F) << 8) + b2 + 1;
        }
        else if (b0 < 0xE0)
        {
            if (pos + 3 > n)
                break;
            b1 = src[pos];
            b2 = src[pos + 1];
            b3 = src[pos + 2];
            pos += 3;
            run = b0 & 3;
            length = ((b0 & 0x0C) << 6) + b3 + 5;
            dist = ((b0 & 0x10) << 12) + (b1 << 8) + b2 + 1;
        }
        else if (b0 < 0xFC)
            run = ((b0 & 0x1F) << 2) + 4;
        else
            run = b0 & 3;
        if (pos + run > n || at + run + length > size + 3 || dist > at + run)
            return false;
        memcpy(out.data() + at, src + pos, run);
        at += run;
        pos += run;
        for (size_t i = 0; i < length; i++, at++)
            out[at] = out[at - dist];
        if (b0 >= 0xFC)
            break;
    }
    if (at < size)
        return false;
    out.resize(size);
    return true;
}

// ---- DXT ---------------------------------------------------------------------------------------
void rgb565(uint16_t c, uint8_t *o)
{
    o[0] = (uint8_t)(((c >> 11) & 31) * 255 / 31);
    o[1] = (uint8_t)(((c >> 5) & 63) * 255 / 63);
    o[2] = (uint8_t)((c & 31) * 255 / 31);
}

// One 4x4 colour block; `opaque` forces four colours (DXT3/5).
void colour_block(const uint8_t *b, bool opaque, uint8_t px[16][4])
{
    uint16_t c0 = le16(b), c1 = le16(b + 2);
    uint8_t c[4][4] = {};
    rgb565(c0, c[0]);
    rgb565(c1, c[1]);
    c[0][3] = c[1][3] = c[2][3] = 255;
    c[3][3] = 255;
    for (int k = 0; k < 3; k++)
        if (c0 > c1 || opaque)
        {
            c[2][k] = (uint8_t)((2 * c[0][k] + c[1][k]) / 3);
            c[3][k] = (uint8_t)((c[0][k] + 2 * c[1][k]) / 3);
        }
        else
        {
            c[2][k] = (uint8_t)((c[0][k] + c[1][k]) / 2);
            c[3][k] = 0;
        }
    if (!(c0 > c1 || opaque))
        c[3][3] = 0;
    uint32_t bits = le32(b + 4);
    for (int i = 0; i < 16; i++)
        memcpy(px[i], c[(bits >> (2 * i)) & 3], 4);
}

bool dxt_decode(int type, const uint8_t *data, size_t n, int w, int h, Bytes &rgba)
{
    size_t block = type == 0x60 ? 8 : 16;
    size_t need = (size_t)((w + 3) / 4) * ((h + 3) / 4) * block;
    if (n < need)
        return false;
    rgba.assign((size_t)w * h * 4, 0);
    const uint8_t *b = data;
    for (int by = 0; by < (h + 3) / 4; by++)
        for (int bx = 0; bx < (w + 3) / 4; bx++, b += block)
        {
            uint8_t px[16][4];
            const uint8_t *colour = type == 0x60 ? b : b + 8;
            colour_block(colour, type != 0x60, px);
            if (type == 0x61)       // DXT3: 4-bit alpha
                for (int i = 0; i < 16; i++)
                    px[i][3] = (uint8_t)(((b[i / 2] >> ((i & 1) * 4)) & 15) * 17);
            else if (type == 0x62)  // DXT5: interpolated alpha
            {
                uint8_t a[8] = {b[0], b[1]};
                if (a[0] > a[1])
                    for (int k = 1; k < 7; k++)
                        a[k + 1] = (uint8_t)(((7 - k) * a[0] + k * a[1]) / 7);
                else
                {
                    for (int k = 1; k < 5; k++)
                        a[k + 1] = (uint8_t)(((5 - k) * a[0] + k * a[1]) / 5);
                    a[6] = 0;
                    a[7] = 255;
                }
                uint64_t bits = 0;
                for (int k = 0; k < 6; k++)
                    bits |= (uint64_t)b[2 + k] << (8 * k);
                for (int i = 0; i < 16; i++)
                    px[i][3] = a[(bits >> (3 * i)) & 7];
            }
            for (int i = 0; i < 16; i++)
            {
                int x = bx * 4 + i % 4, y = by * 4 + i / 4;
                if (x < w && y < h)
                    memcpy(&rgba[((size_t)y * w + x) * 4], px[i], 4);
            }
        }
    return true;
}

// ---- The archive -------------------------------------------------------------------------------
struct Archive
{
    bool opened = false, ok = false;
    std::ifstream file;
    std::map<std::string, Entry> dir;
};

Archive &screens()
{
    static Archive a;
    if (!a.opened)
    {
        a.opened = true;
        a.file.open("game/screens/screens.viv", std::ios::binary);
        if (a.file)
        {
            uint8_t head[16];
            a.file.read((char *)head, 16);
            uint32_t dir_end = a.file ? be32(head + 12) : 0;   // BIG: header + directory size
            if (dir_end > 16 && dir_end < (32u << 20))
            {
                Bytes d(dir_end);
                a.file.seekg(0);
                a.file.read((char *)d.data(), dir_end);
                a.ok = a.file && big_directory(d.data(), d.size(), a.dir);
            }
        }
    }
    return a;
}

bool read_entry(Archive &a, const std::string &name, Bytes &out)
{
    auto it = a.dir.find(name);
    if (it == a.dir.end() || it->second.size > (64u << 20))
        return false;
    out.resize(it->second.size);
    a.file.clear();
    a.file.seekg(it->second.offset);
    a.file.read((char *)out.data(), (std::streamsize)out.size());
    return (bool)a.file;
}

bool fighter_card(int id, Bytes &rgba)
{
    Archive &a = screens();
    if (!a.ok)
        return false;
    char name[64];
    snprintf(name, sizeof name, "screens/portrait/%d_2p.big", id + 1);
    Bytes packed, big;
    if (!read_entry(a, name, packed) || !refpack(packed.data(), packed.size(), big))
        return false;
    std::map<std::string, Entry> inner;
    if (!big_directory(big.data(), big.size(), inner))
        return false;
    for (auto &[n, e] : inner)
    {
        if (n.size() < 4 || n.compare(n.size() - 4, 4, ".xsh") || (uint64_t)e.offset + e.size > big.size())
            continue;
        const uint8_t *x = big.data() + e.offset;
        size_t xn = e.size;
        if (xn < 24 || memcmp(x, "SHPX", 4) || le32(x + 8) < 1)
            return false;
        uint32_t off = le32(x + 20);
        if ((uint64_t)off + 16 > xn)
            return false;
        int type = x[off], w = le16(x + off + 4), h = le16(x + off + 6);
        if ((type != 0x60 && type != 0x61 && type != 0x62) || w < CARD_W || h < CARD_H)
            return false;
        Bytes atlas;
        if (!dxt_decode(type, x + off + 16, xn - off - 16, w, h, atlas))
            return false;
        rgba.resize((size_t)CARD_W * CARD_H * 4);
        for (int y = 0; y < CARD_H; y++)
            memcpy(&rgba[(size_t)y * CARD_W * 4], &atlas[(size_t)y * w * 4], (size_t)CARD_W * 4);
        return true;
    }
    return false;
}
} // namespace

const LauncherImage *launcher_game_fighter(int id)
{
    static std::map<int, bool> missing;
    if (id <= 0 || missing.count(id))
        return nullptr;
    char key[48];
    snprintf(key, sizeof key, "game:fighters/%d", id);
    Bytes rgba;
    if (const LauncherImage *img = launcher_image_cached(key))
        return img;
    if (!fighter_card(id, rgba))
    {
        missing[id] = true;
        return nullptr;
    }
    return launcher_image_rgba(key, rgba.data(), CARD_W, CARD_H);
}
