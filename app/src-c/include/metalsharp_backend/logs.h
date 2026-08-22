#ifndef METALSHARP_BACKEND_LOGS_H
#define METALSHARP_BACKEND_LOGS_H

#include <stddef.h>

void ms_log_event(const char* metalsharp_home, const char* message);
void ms_issue_log(const char* metalsharp_home, const char* kind, const char* subject, const char* summary);
char* ms_logs_json(const char* metalsharp_home);
char* ms_logs_stream_json(const char* metalsharp_home, const char* query);
char* ms_crash_reports_json(const char* metalsharp_home);

#endif
