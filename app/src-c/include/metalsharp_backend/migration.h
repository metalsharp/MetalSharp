#ifndef METALSHARP_BACKEND_MIGRATION_BASIC_H
#define METALSHARP_BACKEND_MIGRATION_BASIC_H
#include <stddef.h>
char* ms_migration_check_json(const char*);
char* ms_migration_start_json(const char*);
char* ms_migration_progress_json(const char*);
char* ms_migration_report_json(const char*);
#endif
