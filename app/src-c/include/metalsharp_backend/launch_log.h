#ifndef METALSHARP_BACKEND_LAUNCH_LOG_H
#define METALSHARP_BACKEND_LAUNCH_LOG_H
#include <stdbool.h>
#include <stddef.h>

/* A log file per game launch (#834): logs/launches/<date>_<time>-<appid>-<route>.log. It starts with the Mac (chip,
 * macOS, memory), the runtime's versions (MetalSharp, MetalSharp Wine and its renderers) and the prefix's Wine settings
 * (Windows version, DPI); the launch summary and Wine's own errors follow, and how the game ended closes it. */

/* Creates the launch's log and writes its header; when the game's last launch was on another route, says how that one
 * ended, there and in the day's log. `path` receives the log's path. Called before the game is started. */
bool ms_launch_log_begin(const char* home, unsigned appid, const char* route, const char* runtime, char* path,
                         size_t path_size);

/* How a launch ended, for its log, the day's log and the game's next launch on another route. `exited` is whether the
 * launched process's status is known (`status` as waitpid(2) gives it); `stopped` whether MetalSharp stopped it.
 * Runs on a thread of its own: it groups the log's repeated lines, which may take a moment on a long one. */
void ms_launch_log_end(const char* home, const char* path, unsigned appid, const char* route, bool exited, int status,
                       bool stopped, unsigned long long ran_ms);

/* The log's repeated lines grouped (#834): a line Wine repeats (its thread id aside) is kept once, where it first
 * appears, with how many times it came. Rewrites `path` in place; for tests and ms_launch_log_end. */
bool ms_launch_log_group_repeats(const char* path);

/* The Windows version and DPI a prefix's registry sets, as "Windows 10 Pro (build 19045)" and "96 (100 %)"; empty
 * when not set. */
void ms_launch_log_prefix_settings(const char* prefix, char* windows, size_t windows_size, char* dpi, size_t dpi_size);
#endif
