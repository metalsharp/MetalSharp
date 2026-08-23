#ifndef METALSHARP_BACKEND_JSON_WRITER_H
#define METALSHARP_BACKEND_JSON_WRITER_H

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    char* data;
    size_t length;
    size_t capacity;
    bool failed;
    unsigned depth;
    bool has_item[32];
    bool expecting_value[32];
    bool object[32];
} ms_json_writer;

void ms_json_writer_init(ms_json_writer* writer);
void ms_json_writer_dispose(ms_json_writer* writer);
char* ms_json_writer_take(ms_json_writer* writer);

void ms_json_writer_object_begin(ms_json_writer* writer);
void ms_json_writer_object_end(ms_json_writer* writer);
void ms_json_writer_array_begin(ms_json_writer* writer);
void ms_json_writer_array_end(ms_json_writer* writer);
void ms_json_writer_key(ms_json_writer* writer, const char* key);
void ms_json_writer_string(ms_json_writer* writer, const char* value);
void ms_json_writer_bool(ms_json_writer* writer, bool value);
void ms_json_writer_u64(ms_json_writer* writer, unsigned long long value);
void ms_json_writer_i64(ms_json_writer* writer, long long value);
void ms_json_writer_double(ms_json_writer* writer, double value);
void ms_json_writer_null(ms_json_writer* writer);
void ms_json_writer_raw(ms_json_writer* writer, const char* json);

#endif
