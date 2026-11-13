#ifndef METALSHARP_BACKEND_LINUX_STEAM_H
#define METALSHARP_BACKEND_LINUX_STEAM_H

/* Linux Steam (fex build, experimental, off unless Settings' "Steam client" is Linux): Valve's Linux Steam client in a
 * small Linux virtual machine, which MetalSharp-Wine.app carries as share/steamvm/metalsharp-steam. Its games run on
 * the Mac in MetalSharp Wine, in the Steam prefix (prefix-steam). MetalSharp drives it only through that command:
 *
 *   install           the one-time setup, as a background job ("==> <stage>" lines in logs/linux-steam-setup.log)
 *   start, show       the virtual machine and Steam, and Steam's window
 *   stop              Steam and the virtual machine
 *   status --json     {"installed","vm_running","steam_running","installing"}
 *   url steam://...   a link for the running client (Play)
 *
 * with METALSHARP_VM_DIR=<home>/linux-steam, METALSHARP_VM_RUN=<home>/run/linux-steam, METALSHARP_STEAM_PREFIX and
 * METALSHARP_GAME_PREFIX=<home>/prefix-steam and METALSHARP_WINE=MetalSharp Wine's wine (wineserver beside it).
 *
 * A Play MetalSharp starts names its graphics backend in <run>/launch-intent/<appid> (one line: mullion, dxvk, vkd3d
 * or wine), which the bridge daemon on the Mac prefers over the Steam compatibility tool's for that one launch. */

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

/* What Linux Steam installs games into, beside the Steam roots MetalSharp scans (relative to the data folder). */
#define MS_LINUX_STEAM_LIBRARY_STEAMAPPS "prefix-steam/drive_c/SteamLibrary/steamapps"

typedef struct {
    bool known; /* status --json answered */
    bool installed;
    bool vm_running;
    bool steam_running;
    bool installing;
} ms_linux_steam_state;

/* A program of Linux Steam's virtual machine (krunkit, gvproxy, metalsharp-vmd, the LinuxSteam window app), which
 * runs from MetalSharp-Wine.app: never taken for Wine, nor killed with it (it is stopped with metalsharp-steam). */
bool ms_linux_steam_vm_executable(const char* executable);

/* The metalsharp-steam command of the installed MetalSharp Wine
 * (<home>/runtime/MetalSharp-Wine.app/Contents/share/steamvm/metalsharp-steam). */
bool ms_linux_steam_command_path(const char* home, char* out, size_t out_size);
/* The fex build, with a MetalSharp Wine that carries Linux Steam. */
bool ms_linux_steam_available(const char* home);
/* Linux Steam is the Steam client: available, and chosen in Settings (steamClient "linux"). */
bool ms_linux_steam_mode(const char* home);

/* The bridge daemon's name for a fex lane: dxmt/dxmt_32 mullion, vkd3d, dxvk, wine_bare wine; NULL for others. */
const char* ms_linux_steam_intent_backend(const char* lane);
/* <home>/run/linux-steam/launch-intent/<appid> (directory 0700, file 0600) naming lane's backend. */
bool ms_linux_steam_write_intent(const char* home, unsigned appid, const char* lane);
void ms_linux_steam_clear_intent(const char* home, unsigned appid);

/* status --json, at most ~2 s old unless fresh. false when the command could not answer. */
bool ms_linux_steam_state_get(const char* home, ms_linux_steam_state* state, bool fresh);
/* The Steam client runs in the virtual machine (for ms_steam_process_running). */
bool ms_linux_steam_client_running(const char* home);
/* The setup runs (this backend's job, or one the command reports); stage gets its last "==> " line. */
bool ms_linux_steam_installing(const char* home, char* stage, size_t stage_size);
/* The last error of a background start or setup, cleared by the next /steam/launch (empty for none). */
void ms_linux_steam_last_error(char* out, size_t out_size);

/* POST /steam/launch in Linux mode: the setup when Linux Steam is not set up (a background job), else start and
 * show, in the background; replies at once. */
char* ms_linux_steam_launch_json(const char* home, int* status);
/* stop, bounded by seconds; true when neither the virtual machine nor Steam runs afterwards. */
bool ms_linux_steam_stop(const char* home, unsigned seconds);
/* stop only when the virtual machine or Steam runs (the command's run folder exists and status says so); costs
 * nothing when Linux Steam was never started. For quit, updates and migrations, whatever the Steam client setting. */
bool ms_linux_steam_stop_if_running(const char* home, unsigned seconds);
/* Linux Steam started when it is not running, then waited for (up to wait_seconds). NULL, or what failed. */
char* ms_linux_steam_ensure_running(const char* home, unsigned wait_seconds);
/* url <url>, with the launch intent for appid's lane (when lane names one) written first. NULL, or what failed;
 * pid gets the command's pid. */
char* ms_linux_steam_open_url(const char* home, unsigned appid, const char* lane, const char* url, pid_t* pid);

#endif
