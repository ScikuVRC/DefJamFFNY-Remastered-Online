/* The launcher's updater (pc_update.h): runs setup/update.py with the copy's bundled Python, as
 * the Mods page runs mods.py, and reads what it reports from <data>/updater (view.ini,
 * progress.ini). Windows only: the portable copies it serves are Windows ones. */
#define _CRT_SECURE_NO_WARNINGS
#include "pc_update.h"
#include "pc_ui_model.h"
#include "pc_ui_actions.h"
#include "recomp_settings.h"
#include <cstdlib>
#include <fstream>
#include <map>
#if defined(_WIN32)
#include <windows.h>
#endif
#ifndef DEFJAM_VERSION
#define DEFJAM_VERSION "0.0.0"
#endif

namespace pcui {
static UpdateView s_view;
static std::string s_status;
static bool s_running = false, s_checked = false;
#if defined(_WIN32)
static HANDLE s_process = nullptr;

static fs::path install_dir()
{
    wchar_t path[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(nullptr, path, (DWORD)(sizeof path / sizeof path[0]));
    if (!n || n >= sizeof path / sizeof path[0]) return {};
    return fs::path(path).parent_path();
}

static std::wstring wide(const std::string &s)
{
    int count = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring out(count, 0);
    if (count) MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), count);
    return out;
}

static std::wstring quote(const std::wstring &s)
{
    std::wstring out = L"\"";
    size_t slashes = 0;
    for (wchar_t c : s) {
        if (c == L'\\') { slashes++; continue; }
        out.append(slashes * (c == L'"' ? 2 : 1), L'\\');
        slashes = 0;
        if (c == L'"') out += L'\\';
        out += c;
    }
    out.append(slashes * 2, L'\\');
    return out + L'"';
}
#endif

static std::string url()
{
    char text[1024] = "";
    recomp_settings_get_text("update", "url", text, sizeof text, "");
    return text;
}

static fs::path state_dir() { return fs::u8path(folder_path(Folder::Data)) / "updater"; }

static std::map<std::string, std::string> section(const fs::path &path, const char *name)
{
    std::map<std::string, std::string> out;
    std::ifstream in(path);
    std::string line, current;
    for (int count = 0; count < 1000 && std::getline(in, line); count++) {
        if (!line.empty() && line.front() == '[' && line.back() == ']') current = line.substr(1, line.size() - 2);
        else if (current == name) {
            auto eq = line.find('=');
            if (eq != std::string::npos) out[line.substr(0, eq)] = line.substr(eq + 1);
        }
    }
    return out;
}

static void read_view()
{
    auto v = section(state_dir() / "view.ini", "update");
    s_view.state = v["state"];
    s_view.version = v["version"];
    s_view.installed = v["installed"];
    s_view.notes = v["notes"];
    s_view.message = v["message"];
    s_view.size = strtoull(v["size"].c_str(), nullptr, 10);
    /* A view written by an older version of the program describes that one. */
    if (s_view.installed != DEFJAM_VERSION && s_view.state != "installed") s_view = UpdateView();
    if (s_view.state == "installed" && s_view.version == DEFJAM_VERSION) s_view = UpdateView();
}

bool update_configured()
{
#if defined(_WIN32)
    std::error_code error;
    fs::path dir = install_dir();
    return !url().empty() && fs::is_regular_file(dir / "setup" / "update.py", error)
           && fs::is_regular_file(dir / "python" / "python.exe", error);
#else
    return false;
#endif
}

bool update_start(const std::string &command)
{
#if defined(_WIN32)
    if (s_running || !update_configured() || (command != "check" && command != "install")) return false;
    fs::path dir = install_dir();
    std::error_code error;
    fs::create_directories(state_dir(), error);
    fs::remove(state_dir() / "progress.ini", error);
    std::wstring cmd = quote((dir / "python" / "python.exe").wstring()) + L" " + quote((dir / "setup" / "update.py").wstring())
                       + L" " + wide(command) + L" --install-dir " + quote(dir.wstring()) + L" --data-dir "
                       + quote(wide(folder_path(Folder::Data))) + L" --runtime " + wide(DEFJAM_VERSION) + L" --url "
                       + quote(wide(url()));
    STARTUPINFOW startup = {};
    startup.cb = sizeof startup;
    PROCESS_INFORMATION child = {};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child)) {
        s_status = "Could not start the updater";
        return false;
    }
    CloseHandle(child.hThread);
    s_process = child.hProcess;
    s_running = true;
    s_status = command == "check" ? "Checking for an update..." : "Updating...";
    return true;
#else
    (void)command;
    return false;
#endif
}

bool update_poll()
{
#if defined(_WIN32)
    if (!s_running) return false;
    auto p = section(state_dir() / "progress.ini", "progress");
    if (!p["message"].empty()) {
        s_status = p["message"];
        unsigned long long done = strtoull(p["done"].c_str(), nullptr, 10), total = strtoull(p["total"].c_str(), nullptr, 10);
        if (total) s_status += " (" + std::to_string(done * 100 / total) + "%)";
    }
    DWORD code;
    if (!GetExitCodeProcess(s_process, &code) || code == STILL_ACTIVE) return false;
    CloseHandle(s_process);
    s_process = nullptr;
    s_running = false;
    read_view();
    if (!s_view.message.empty()) s_status = s_view.message;
    return true;
#else
    return false;
#endif
}

bool update_busy() { return s_running; }

void update_cancel()
{
    if (!s_running) return;
    std::ofstream cancel(state_dir() / "cancel");
    cancel << "cancel\n";
}

const UpdateView &update_view()
{
    static bool loaded = false;
    if (!loaded) { read_view(); loaded = true; }
    return s_view;
}

const std::string &update_status() { return s_status; }

void update_check_once()
{
    if (s_checked) return;
    s_checked = true;
    update_start("check");
}

bool update_restart()
{
#if defined(_WIN32)
    wchar_t path[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(nullptr, path, (DWORD)(sizeof path / sizeof path[0]));
    if (!n) return false;
    std::wstring cmd = GetCommandLineW();
    STARTUPINFOW startup = {};
    startup.cb = sizeof startup;
    PROCESS_INFORMATION child = {};
    /* The same command line and environment (Play.bat's DEFJAM_DATA), from the copy's folder;
     * the program file at this path is the new one now. */
    if (!CreateProcessW(path, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, install_dir().c_str(), &startup, &child))
        return false;
    CloseHandle(child.hThread);
    CloseHandle(child.hProcess);
    return true;
#else
    return false;
#endif
}
} // namespace pcui
