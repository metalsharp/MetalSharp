#ifndef METALSHARP_BACKEND_JSON_H
#define METALSHARP_BACKEND_JSON_H

#include <stdbool.h>
#include <stddef.h>

/*
 * Small, bounded JSON reader used at the HTTP boundary.  It deliberately
 * exposes only the operations the backend needs; service code never receives
 * parser-owned pointers and every returned string is caller-owned.
 */
typedef struct ms_json ms_json;

typedef enum {
    MS_JSON_INVALID = 0,
    MS_JSON_NULL,
    MS_JSON_BOOL,
    MS_JSON_NUMBER,
    MS_JSON_STRING,
    MS_JSON_ARRAY,
    MS_JSON_OBJECT
} ms_json_type;

ms_json* ms_json_parse(const char* data, size_t length, char* error, size_t error_size);
void ms_json_free(ms_json* value);
ms_json_type ms_json_type_of(const ms_json* value);

const ms_json* ms_json_object_get(const ms_json* object, const char* key);
size_t ms_json_object_length(const ms_json* object);
const char* ms_json_object_key_at(const ms_json* object, size_t index);
const ms_json* ms_json_object_value_at(const ms_json* object, size_t index);
const ms_json* ms_json_array_get(const ms_json* array, size_t index);
size_t ms_json_array_length(const ms_json* array);

bool ms_json_as_bool(const ms_json* value, bool* out);
bool ms_json_as_number(const ms_json* value, double* out);
bool ms_json_as_i64(const ms_json* value, long long* out);
bool ms_json_as_string(const ms_json* value, char** out);

/* Encode a UTF-8 string as a JSON string, including its surrounding quotes. */
char* ms_json_quote(const char* value);
/* Compact serialization of a parsed value. The returned string is owned by
 * the caller and can be embedded as a raw value in another response. */
char* ms_json_stringify(const ms_json* value);

#endif
