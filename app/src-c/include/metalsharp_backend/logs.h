#ifndef METALSHARP_BACKEND_LOGS_H
#define METALSHARP_BACKEND_LOGS_H

#include <stdbool.h>
#include <stddef.h>

void ms_log_event(const char* metalsharp_home, const char* message);
/* Today's log (logs/<date>.log) into out; false when the path doesn't fit. */
bool ms_log_today_path(const char* metalsharp_home, char* out, size_t out_size);
/* The time as log lines show it ("2026-11-24 06:35:00 MST"). */
bool ms_log_stamp(char* out, size_t out_size);
void ms_issue_log(const char* metalsharp_home, const char* kind, const char* subject, const char* summary);
char* ms_logs_json(const char* metalsharp_home);
char* ms_logs_stream_json(const char* metalsharp_home, const char* query);
char* ms_crash_reports_json(const char* metalsharp_home);

#endif
