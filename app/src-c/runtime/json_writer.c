#include "metalsharp_backend/json_writer.h"

#include "metalsharp_backend/json.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool reserve(ms_json_writer* writer, size_t additional) {
    size_t required;
    size_t capacity;
    char* grown;
    if (writer->failed)
        return false;
    if (additional > SIZE_MAX - writer->length - 1) {
        writer->failed = true;
        return false;
    }
    required = writer->length + additional + 1;
    if (required <= writer->capacity)
        return true;
    capacity = writer->capacity == 0 ? 256 : writer->capacity;
    while (capacity < required) {
        if (capacity > SIZE_MAX / 2) {
            capacity = required;
            break;
        }
        capacity *= 2;
    }
    grown = (char*)realloc(writer->data, capacity);
    if (grown == NULL) {
        writer->failed = true;
        return false;
    }
    writer->data = grown;
    writer->capacity = capacity;
    return true;
}

static void append_bytes(ms_json_writer* writer, const char* data, size_t length) {
    if (!reserve(writer, length))
        return;
    memcpy(writer->data + writer->length, data, length);
    writer->length += length;
    writer->data[writer->length] = '\0';
}

static void append_literal(ms_json_writer* writer, const char* literal) {
    append_bytes(writer, literal, strlen(literal));
}

static bool before_value(ms_json_writer* writer) {
    if (writer->depth == 0) {
        if (writer->length != 0) {
            writer->failed = true;
            return false;
        }
        return true;
    }
    unsigned parent = writer->depth - 1;
    if (writer->object[parent] && !writer->expecting_value[parent]) {
        writer->failed = true;
        return false;
    }
    if (!writer->expecting_value[parent] && writer->has_item[parent])
        append_literal(writer, ",");
    writer->has_item[parent] = true;
    writer->expecting_value[parent] = false;
    return !writer->failed;
}

void ms_json_writer_init(ms_json_writer* writer) {
    memset(writer, 0, sizeof(*writer));
}

void ms_json_writer_dispose(ms_json_writer* writer) {
    if (writer == NULL)
        return;
    free(writer->data);
    memset(writer, 0, sizeof(*writer));
}

char* ms_json_writer_take(ms_json_writer* writer) {
    char* data;
    if (writer == NULL || writer->failed || writer->depth != 0 || writer->data == NULL) {
        if (writer != NULL)
            ms_json_writer_dispose(writer);
        return NULL;
    }
    data = writer->data;
    writer->data = NULL;
    writer->length = 0;
    writer->capacity = 0;
    return data;
}

void ms_json_writer_object_begin(ms_json_writer* writer) {
    if (!before_value(writer) || writer->depth >= 32) {
        writer->failed = true;
        return;
    }
    append_literal(writer, "{");
    writer->has_item[writer->depth] = false;
    writer->expecting_value[writer->depth] = false;
    writer->object[writer->depth] = true;
    writer->depth++;
}

void ms_json_writer_object_end(ms_json_writer* writer) {
    if (writer == NULL || writer->depth == 0) {
        if (writer != NULL)
            writer->failed = true;
        return;
    }
    if (!writer->object[writer->depth - 1] || writer->expecting_value[writer->depth - 1]) {
        writer->failed = true;
        return;
    }
    append_literal(writer, "}");
    writer->depth--;
}

void ms_json_writer_array_begin(ms_json_writer* writer) {
    if (!before_value(writer) || writer->depth >= 32) {
        writer->failed = true;
        return;
    }
    append_literal(writer, "[");
    writer->has_item[writer->depth] = false;
    writer->expecting_value[writer->depth] = false;
    writer->object[writer->depth] = false;
    writer->depth++;
}

void ms_json_writer_array_end(ms_json_writer* writer) {
    if (writer == NULL || writer->depth == 0 || writer->object[writer->depth - 1]) {
        if (writer != NULL)
            writer->failed = true;
        return;
    }
    append_literal(writer, "]");
    writer->depth--;
}

void ms_json_writer_key(ms_json_writer* writer, const char* key) {
    char* quoted;
    unsigned parent_index;
    if (writer == NULL || writer->depth == 0 || !writer->object[writer->depth - 1]) {
        if (writer != NULL)
            writer->failed = true;
        return;
    }
    parent_index = writer->depth - 1;
    if (writer->expecting_value[parent_index]) {
        writer->failed = true;
        return;
    }
    if (writer->has_item[parent_index])
        append_literal(writer, ",");
    quoted = ms_json_quote(key == NULL ? "" : key);
    if (quoted == NULL) {
        writer->failed = true;
        return;
    }
    append_literal(writer, quoted);
    append_literal(writer, ":");
    free(quoted);
    writer->has_item[parent_index] = true;
    writer->expecting_value[parent_index] = true;
}

void ms_json_writer_string(ms_json_writer* writer, const char* value) {
    char* quoted;
    if (!before_value(writer))
        return;
    quoted = ms_json_quote(value == NULL ? "" : value);
    if (quoted == NULL) {
        writer->failed = true;
        return;
    }
    append_literal(writer, quoted);
    free(quoted);
}

void ms_json_writer_bool(ms_json_writer* writer, bool value) {
    if (before_value(writer))
        append_literal(writer, value ? "true" : "false");
}

void ms_json_writer_u64(ms_json_writer* writer, unsigned long long value) {
    char buffer[32];
    int length;
    if (!before_value(writer))
        return;
    length = snprintf(buffer, sizeof(buffer), "%llu", value);
    if (length < 0 || (size_t)length >= sizeof(buffer)) {
        writer->failed = true;
        return;
    }
    append_bytes(writer, buffer, (size_t)length);
}

void ms_json_writer_i64(ms_json_writer* writer, long long value) {
    char buffer[32];
    int length;
    if (!before_value(writer))
        return;
    length = snprintf(buffer, sizeof(buffer), "%lld", value);
    if (length < 0 || (size_t)length >= sizeof(buffer)) {
        writer->failed = true;
        return;
    }
    append_bytes(writer, buffer, (size_t)length);
}

void ms_json_writer_double(ms_json_writer* writer, double value) {
    char buffer[64];
    int length;
    if (!before_value(writer))
        return;
    length = snprintf(buffer, sizeof(buffer), "%.9g", value);
    if (length < 0 || (size_t)length >= sizeof(buffer)) {
        writer->failed = true;
        return;
    }
    append_bytes(writer, buffer, (size_t)length);
}

void ms_json_writer_null(ms_json_writer* writer) {
    if (before_value(writer))
        append_literal(writer, "null");
}

void ms_json_writer_raw(ms_json_writer* writer, const char* json) {
    if (before_value(writer))
        append_literal(writer, json == NULL ? "null" : json);
}
