#include "pc_launcher_profiles.h"
#include "netplay.h"
#include "recomp_settings.h"
#include "imgui.h"
#include <algorithm>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sys/stat.h>

namespace
{
// A profile file is a signed 0x8C-byte header and then the profile (scripts/save-import.py),
// which starts with its name. In the profile, checked against the game's own Select User ID
// screen (2026-11-03): cash, Story time in seconds, and Story progress, which that screen
// shows out of 69, rounded down (2 is 2%, 3 is 4%, 9 is 13%, 11 is 15%); a finished Story
// counts past it (73, 74) and shows 100%.
const size_t PAYLOAD = 0x8C;
const size_t CASH = 0x14, STORY_DONE = 0x28, STORY_TIME = 0x178;
const unsigned STORY_STEPS = 69;

// The created fighter's two pictures: 8-bit indexed and swizzled, each followed by its
// 256-colour BGRA palette.
struct Portrait
{
    size_t at;
    int size;
};
const Portrait FIGHTER = {0x97E, 128}, FACE = {0x17E, 32};
const size_t PROFILE_SIZE = FIGHTER.at + 128 * 128 + 1024;

ImGuiContext *g_owner = nullptr;
std::vector<LauncherProfile> g_profiles;

unsigned u32(const std::vector<unsigned char> &p, size_t at)
{
    return p[at] | p[at + 1] << 8 | p[at + 2] << 16 | (unsigned)p[at + 3] << 24;
}

// The Xbox's swizzle for a square power-of-two texture: the bits of x and y alternate, x's in
// the even places.
size_t swizzled(int x, int y, int size)
{
    size_t at = 0;
    for (int bit = 0; (1 << bit) < size; bit++)
        at |= (size_t)((x >> bit) & 1) << (2 * bit) | (size_t)((y >> bit) & 1) << (2 * bit + 1);
    return at;
}

const LauncherImage *portrait(const std::vector<unsigned char> &p, const Portrait &where, const std::string &key)
{
    const unsigned char *pixels = p.data() + where.at, *palette = pixels + where.size * where.size;
    std::vector<unsigned char> rgba((size_t)where.size * where.size * 4);
    bool any = false;
    for (int y = 0; y < where.size; y++)
        for (int x = 0; x < where.size; x++)
        {
            const unsigned char *c = palette + 4 * pixels[swizzled(x, y, where.size)];
            unsigned char *o = &rgba[((size_t)y * where.size + x) * 4];
            o[0] = c[2];
            o[1] = c[1];
            o[2] = c[0];
            o[3] = c[3];
            any |= c[3] != 0;
        }
    // A profile saved before the game took its snapshot has an empty one (docs/known-issues.md).
    return any ? launcher_image_rgba(key.c_str(), rgba.data(), where.size, where.size) : nullptr;
}

long long modified(const std::filesystem::path &path)
{
#if defined(_WIN32)
    struct _stat64 st;
    return _wstat64(path.c_str(), &st) == 0 ? (long long)st.st_mtime : 0;
#else
    struct stat st;
    return stat(path.c_str(), &st) == 0 ? (long long)st.st_mtime : 0;
#endif
}

void load()
{
    char root[1024], names[16][NETPLAY_PROFILE_LEN], folder[64];
    g_profiles.clear();
    if (!netplay_save_root(root, sizeof root))
        return;
    // netplay_profiles' order is the folders', which is the game's own (netplay.c, profile_user).
    int count = netplay_profiles(names, 16);
    for (int i = 0; i < count; i++)
    {
        LauncherProfile profile;
        profile.name = names[i];
        profile.user = i;
        if (netplay_save_folder(root, names[i], folder))
        {
            std::filesystem::path path =
                std::filesystem::u8path(root) / "UserData" / "45410049" / folder / std::filesystem::u8path(names[i]);
            std::ifstream in(path, std::ios::binary);
            std::vector<unsigned char> file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            profile.played = modified(path);
            if (file.size() >= PAYLOAD + PROFILE_SIZE)
            {
                std::vector<unsigned char> p(file.begin() + PAYLOAD, file.end());
                profile.story = (int)std::min<unsigned long long>(100, u32(p, STORY_DONE) * 100ull / STORY_STEPS);
                profile.cash = u32(p, CASH);
                profile.story_seconds = u32(p, STORY_TIME);
                profile.fighter = portrait(p, FIGHTER, "profile/" + profile.name + "/fighter");
                profile.face = portrait(p, FACE, "profile/" + profile.name + "/face");
            }
        }
        g_profiles.push_back(profile);
    }
}

bool local_time(time_t t, struct tm *out)
{
#if defined(_WIN32)
    return localtime_s(out, &t) == 0;
#else
    return localtime_r(&t, out) != nullptr;
#endif
}
} // namespace

const std::vector<LauncherProfile> &launcher_profiles()
{
    // The pictures are textures of one ImGui context; read everything again in a new one.
    if (g_owner != ImGui::GetCurrentContext())
    {
        g_owner = ImGui::GetCurrentContext();
        load();
    }
    return g_profiles;
}

const LauncherProfile *launcher_current_profile()
{
    char chosen[NETPLAY_PROFILE_LEN];
    recomp_settings_get_text("launcher", "profile", chosen, sizeof chosen, "");
    const LauncherProfile *latest = nullptr;
    for (const LauncherProfile &profile : launcher_profiles())
    {
        if (chosen[0] && profile.name == chosen)
            return &profile;
        if (!latest || profile.played > latest->played)
            latest = &profile;
    }
    return latest;
}

std::string launcher_played_text(long long played)
{
    struct tm now, then;
    if (played <= 0 || !local_time(time(nullptr), &now) || !local_time((time_t)played, &then))
        return "";
    struct tm midnight = now;
    midnight.tm_hour = midnight.tm_min = midnight.tm_sec = 0;
    long long today = (long long)mktime(&midnight), days = played >= today ? 0 : (today - played) / 86400 + 1;
    char text[32];
    if (days == 0)
        return then.tm_hour >= 18 ? "tonight" : "today";
    if (days == 1)
        return "yesterday";
    if (days < 7)
    {
        snprintf(text, sizeof text, "%lld days ago", days);
        return text;
    }
    strftime(text, sizeof text, "%d %b", &then);
    return text[0] == '0' ? text + 1 : text;
}
