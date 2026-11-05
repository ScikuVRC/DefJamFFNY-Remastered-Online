#pragma once
/* The launcher's updater for a portable copy (setup/update.py): a private update channel,
 * [update] url in settings.ini, checked at start; Update downloads, verifies and installs,
 * Restart starts the new version. */
#ifdef __cplusplus
#include <string>
namespace pcui {
struct UpdateView {
    std::string state;      // "", "current", "available", "installed", "error"
    std::string version, installed, notes, message;
    unsigned long long size = 0;
};
bool update_configured();                           // an address is set and this copy has the helper
bool update_start(const std::string &command);      // "check" or "install"
bool update_poll();                                 // true once a job has ended
bool update_busy();
void update_cancel();
const UpdateView &update_view();
const std::string &update_status();
void update_check_once();                           // the launcher's start-up check
bool update_restart();                              // start the new version; the caller then quits
}
#endif
