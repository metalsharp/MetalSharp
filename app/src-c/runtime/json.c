#include "metalsharp_backend/json.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct ms_json {
    ms_json_type type;
    union {
        bool boolean;
        double number;
        char* string;
        struct {
            struct ms_json** items;
            size_t length;
        } array;
        struct {
            char** keys;
            struct ms_json** values;
            size_t length;
        } object;
    } value;
};

typedef struct {
    const char* data;
    size_t length;
    size_t offset;
    size_t depth;
    char* error;
    size_t error_size;
} parser;

static void set_error(parser* p, const char* message) {
    if (p->error != NULL && p->error_size > 0 && p->error[0] == '\0') {
        (void)snprintf(p->error, p->error_size, "at byte %zu: %s", p->offset, message);
    }
}

static bool ensure(parser* p, size_t count) {
    if (count <= p->length - p->offset) {
        return true;
    }
    set_error(p, "unexpected end of input");
    return false;
}

static void skip_ws(parser* p) {
    while (p->offset < p->length && (p->data[p->offset] == ' ' || p->data[p->offset] == '\t' ||
                                     p->data[p->offset] == '\r' || p->data[p->offset] == '\n')) {
        p->offset++;
    }
}

static struct ms_json* new_value(ms_json_type type) {
    struct ms_json* value = (struct ms_json*)calloc(1, sizeof(*value));
    if (value != NULL) {
        value->type = type;
    }
    return value;
}

void ms_json_free(ms_json* value) {
    size_t i;
    if (value == NULL) {
        return;
    }
    switch (value->type) {
    case MS_JSON_STRING:
        free(value->value.string);
        break;
    case MS_JSON_ARRAY:
        for (i = 0; i < value->value.array.length; ++i) {
            ms_json_free(value->value.array.items[i]);
        }
        free(value->value.array.items);
        break;
    case MS_JSON_OBJECT:
        for (i = 0; i < value->value.object.length; ++i) {
            free(value->value.object.keys[i]);
            ms_json_free(value->value.object.values[i]);
        }
        free(value->value.object.keys);
        free(value->value.object.values);
        break;
    default:
        break;
    }
    free(value);
}

static bool append_byte(char** buffer, size_t* length, size_t* capacity, unsigned char byte) {
    if (*length == *capacity) {
        size_t next = (*capacity == 0) ? 32 : (*capacity * 2);
        char* grown = (char*)realloc(*buffer, next);
        if (grown == NULL) {
            return false;
        }
        *buffer = grown;
        *capacity = next;
    }
    (*buffer)[(*length)++] = (char)byte;
    return true;
}

static bool append_utf8(char** buffer, size_t* length, size_t* capacity, unsigned codepoint) {
    if (codepoint <= 0x7f) {
        return append_byte(buffer, length, capacity, (unsigned char)codepoint);
    }
    if (codepoint <= 0x7ff) {
        return append_byte(buffer, length, capacity, (unsigned char)(0xc0 | (codepoint >> 6))) &&
               append_byte(buffer, length, capacity, (unsigned char)(0x80 | (codepoint & 0x3f)));
    }
    if (codepoint <= 0xffff) {
        return append_byte(buffer, length, capacity, (unsigned char)(0xe0 | (codepoint >> 12))) &&
               append_byte(buffer, length, capacity, (unsigned char)(0x80 | ((codepoint >> 6) & 0x3f))) &&
               append_byte(buffer, length, capacity, (unsigned char)(0x80 | (codepoint & 0x3f)));
    }
    if (codepoint <= 0x10ffff) {
        return append_byte(buffer, length, capacity, (unsigned char)(0xf0 | (codepoint >> 18))) &&
               append_byte(buffer, length, capacity, (unsigned char)(0x80 | ((codepoint >> 12) & 0x3f))) &&
               append_byte(buffer, length, capacity, (unsigned char)(0x80 | ((codepoint >> 6) & 0x3f))) &&
               append_byte(buffer, length, capacity, (unsigned char)(0x80 | (codepoint & 0x3f)));
    }
    return false;
}

static int hex_value(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static bool read_hex4(parser* p, unsigned* out) {
    unsigned value = 0;
    size_t i;
    if (!ensure(p, 4))
        return false;
    for (i = 0; i < 4; ++i) {
        int digit = hex_value(p->data[p->offset++]);
        if (digit < 0) {
            set_error(p, "invalid unicode escape");
            return false;
        }
        value = (value << 4) | (unsigned)digit;
    }
    *out = value;
    return true;
}

static char* parse_string(parser* p) {
    char* buffer = NULL;
    size_t length = 0;
    size_t capacity = 0;
    unsigned pending_high = 0;

    if (!ensure(p, 1) || p->data[p->offset++] != '"') {
        set_error(p, "expected string");
        return NULL;
    }
    while (p->offset < p->length) {
        unsigned char c = (unsigned char)p->data[p->offset++];
        if (c == '"') {
            if (pending_high != 0) {
                set_error(p, "unpaired unicode surrogate");
                free(buffer);
                return NULL;
            }
            if (!append_byte(&buffer, &length, &capacity, 0)) {
                free(buffer);
                return NULL;
            }
            return buffer;
        }
        if (c < 0x20) {
            set_error(p, "control character in string");
            free(buffer);
            return NULL;
        }
        if (c != '\\') {
            if (!append_byte(&buffer, &length, &capacity, c)) {
                free(buffer);
                return NULL;
            }
            continue;
        }
        if (!ensure(p, 1)) {
            free(buffer);
            return NULL;
        }
        c = (unsigned char)p->data[p->offset++];
        switch (c) {
        case '"':
        case '\\':
        case '/':
            if (!append_byte(&buffer, &length, &capacity, c))
                goto oom;
            break;
        case 'b':
            if (!append_byte(&buffer, &length, &capacity, '\b'))
                goto oom;
            break;
        case 'f':
            if (!append_byte(&buffer, &length, &capacity, '\f'))
                goto oom;
            break;
        case 'n':
            if (!append_byte(&buffer, &length, &capacity, '\n'))
                goto oom;
            break;
        case 'r':
            if (!append_byte(&buffer, &length, &capacity, '\r'))
                goto oom;
            break;
        case 't':
            if (!append_byte(&buffer, &length, &capacity, '\t'))
                goto oom;
            break;
        case 'u': {
            unsigned codepoint;
            if (!read_hex4(p, &codepoint))
                goto fail;
            if (codepoint >= 0xd800 && codepoint <= 0xdbff) {
                if (pending_high != 0 || !ensure(p, 2) || p->data[p->offset] != '\\' || p->data[p->offset + 1] != 'u') {
                    set_error(p, "high surrogate is not followed by low surrogate");
                    goto fail;
                }
                p->offset += 2;
                if (!read_hex4(p, &pending_high) || pending_high < 0xdc00 || pending_high > 0xdfff) {
                    set_error(p, "invalid low surrogate");
                    goto fail;
                }
                codepoint = 0x10000 + ((codepoint - 0xd800) << 10) + (pending_high - 0xdc00);
                pending_high = 0;
            } else if (codepoint >= 0xdc00 && codepoint <= 0xdfff) {
                set_error(p, "unpaired low surrogate");
                goto fail;
            }
            if (!append_utf8(&buffer, &length, &capacity, codepoint))
                goto oom;
            break;
        }
        default:
            set_error(p, "invalid string escape");
            goto fail;
        }
    }
    set_error(p, "unterminated string");
fail:
    free(buffer);
    return NULL;
oom:
    set_error(p, "out of memory");
    free(buffer);
    return NULL;
}

static struct ms_json* parse_value(parser* p);

static struct ms_json* parse_number(parser* p) {
    size_t start = p->offset;
    char* end = NULL;
    double number;
    bool integer_token = true;
    struct ms_json* value;

    if (p->data[p->offset] == '-')
        p->offset++;
    if (p->offset >= p->length)
        goto invalid;
    if (p->data[p->offset] == '0') {
        p->offset++;
    } else if (p->data[p->offset] >= '1' && p->data[p->offset] <= '9') {
        while (p->offset < p->length && isdigit((unsigned char)p->data[p->offset]))
            p->offset++;
    } else {
        goto invalid;
    }
    if (p->offset < p->length && p->data[p->offset] == '.') {
        integer_token = false;
        p->offset++;
        if (p->offset >= p->length || !isdigit((unsigned char)p->data[p->offset]))
            goto invalid;
        while (p->offset < p->length && isdigit((unsigned char)p->data[p->offset]))
            p->offset++;
    }
    if (p->offset < p->length && (p->data[p->offset] == 'e' || p->data[p->offset] == 'E')) {
        integer_token = false;
        p->offset++;
        if (p->offset < p->length && (p->data[p->offset] == '+' || p->data[p->offset] == '-'))
            p->offset++;
        if (p->offset >= p->length || !isdigit((unsigned char)p->data[p->offset]))
            goto invalid;
        while (p->offset < p->length && isdigit((unsigned char)p->data[p->offset]))
            p->offset++;
    }
    errno = 0;
    number = strtod(p->data + start, &end);
    if (errno == ERANGE || end != p->data + p->offset || !isfinite(number))
        goto invalid;
    if (integer_token) {
        char* integer_end = NULL;
        errno = 0;
        (void)strtoll(p->data + start, &integer_end, 10);
        if (errno == ERANGE || integer_end != p->data + p->offset || number > 9007199254740991.0 ||
            number < -9007199254740991.0)
            goto invalid;
    }
    value = new_value(MS_JSON_NUMBER);
    if (value == NULL)
        return NULL;
    value->value.number = number;
    return value;
invalid:
    set_error(p, "invalid number");
    return NULL;
}

static bool append_array_item(struct ms_json* array, struct ms_json* item) {
    size_t next = array->value.array.length + 1;
    struct ms_json** items = (struct ms_json**)realloc(array->value.array.items, next * sizeof(*items));
    if (items == NULL)
        return false;
    array->value.array.items = items;
    array->value.array.items[array->value.array.length++] = item;
    return true;
}

static struct ms_json* parse_array(parser* p) {
    struct ms_json* array = new_value(MS_JSON_ARRAY);
    if (array == NULL)
        return NULL;
    p->offset++;
    if (++p->depth > 256) {
        set_error(p, "maximum nesting depth exceeded");
        p->depth--;
        ms_json_free(array);
        return NULL;
    }
    skip_ws(p);
    if (p->offset < p->length && p->data[p->offset] == ']') {
        p->offset++;
        p->depth--;
        return array;
    }
    for (;;) {
        struct ms_json* item;
        skip_ws(p);
        item = parse_value(p);
        if (item == NULL)
            goto fail;
        if (!append_array_item(array, item)) {
            ms_json_free(item);
            set_error(p, "out of memory");
            goto fail;
        }
        skip_ws(p);
        if (p->offset >= p->length) {
            set_error(p, "unterminated array");
            goto fail;
        }
        if (p->data[p->offset] == ']') {
            p->offset++;
            p->depth--;
            return array;
        }
        if (p->data[p->offset++] != ',') {
            set_error(p, "expected comma in array");
            goto fail;
        }
    }
fail:
    p->depth--;
    ms_json_free(array);
    return NULL;
}

static bool append_object_item(struct ms_json* object, char* key, struct ms_json* value) {
    size_t next = object->value.object.length + 1;
    char** keys = (char**)realloc(object->value.object.keys, next * sizeof(*keys));
    struct ms_json** values;
    if (keys == NULL)
        return false;
    /* Publish the moved key allocation before the second realloc. If the
     * value allocation fails, ms_json_free can still safely release keys. */
    object->value.object.keys = keys;
    values = (struct ms_json**)realloc(object->value.object.values, next * sizeof(*values));
    if (values == NULL)
        return false;
    object->value.object.values = values;
    object->value.object.keys[object->value.object.length] = key;
    object->value.object.values[object->value.object.length++] = value;
    return true;
}

static struct ms_json* parse_object(parser* p) {
    struct ms_json* object = new_value(MS_JSON_OBJECT);
    if (object == NULL)
        return NULL;
    p->offset++;
    if (++p->depth > 256) {
        set_error(p, "maximum nesting depth exceeded");
        p->depth--;
        ms_json_free(object);
        return NULL;
    }
    skip_ws(p);
    if (p->offset < p->length && p->data[p->offset] == '}') {
        p->offset++;
        p->depth--;
        return object;
    }
    for (;;) {
        char* key;
        struct ms_json* value;
        size_t i;
        skip_ws(p);
        if (p->offset >= p->length || p->data[p->offset] != '"') {
            set_error(p, "expected object key");
            goto fail;
        }
        key = parse_string(p);
        if (key == NULL)
            goto fail;
        for (i = 0; i < object->value.object.length; ++i) {
            if (strcmp(object->value.object.keys[i], key) == 0) {
                free(key);
                set_error(p, "duplicate object key");
                goto fail;
            }
        }
        skip_ws(p);
        if (p->offset >= p->length || p->data[p->offset++] != ':') {
            free(key);
            set_error(p, "expected colon after object key");
            goto fail;
        }
        skip_ws(p);
        value = parse_value(p);
        if (value == NULL) {
            free(key);
            goto fail;
        }
        if (!append_object_item(object, key, value)) {
            free(key);
            ms_json_free(value);
            set_error(p, "out of memory");
            goto fail;
        }
        skip_ws(p);
        if (p->offset >= p->length) {
            set_error(p, "unterminated object");
            goto fail;
        }
        if (p->data[p->offset] == '}') {
            p->offset++;
            p->depth--;
            return object;
        }
        if (p->data[p->offset++] != ',') {
            set_error(p, "expected comma in object");
            goto fail;
        }
    }
fail:
    p->depth--;
    ms_json_free(object);
    return NULL;
}

static struct ms_json* parse_value(parser* p) {
    if (p->offset >= p->length) {
        set_error(p, "expected value");
        return NULL;
    }
    switch (p->data[p->offset]) {
    case 'n':
        if (p->length - p->offset >= 4 && memcmp(p->data + p->offset, "null", 4) == 0) {
            p->offset += 4;
            return new_value(MS_JSON_NULL);
        }
        break;
    case 't':
        if (p->length - p->offset >= 4 && memcmp(p->data + p->offset, "true", 4) == 0) {
            struct ms_json* value = new_value(MS_JSON_BOOL);
            p->offset += 4;
            if (value != NULL)
                value->value.boolean = true;
            return value;
        }
        break;
    case 'f':
        if (p->length - p->offset >= 5 && memcmp(p->data + p->offset, "false", 5) == 0) {
            struct ms_json* value = new_value(MS_JSON_BOOL);
            p->offset += 5;
            if (value != NULL)
                value->value.boolean = false;
            return value;
        }
        break;
    case '"': {
        struct ms_json* value = new_value(MS_JSON_STRING);
        if (value == NULL)
            return NULL;
        value->value.string = parse_string(p);
        if (value->value.string == NULL) {
            ms_json_free(value);
            return NULL;
        }
        return value;
    }
    case '[':
        return parse_array(p);
    case '{':
        return parse_object(p);
    default:
        if (p->data[p->offset] == '-' || isdigit((unsigned char)p->data[p->offset]))
            return parse_number(p);
        break;
    }
    set_error(p, "invalid value");
    return NULL;
}

ms_json* ms_json_parse(const char* data, size_t length, char* error, size_t error_size) {
    parser p;
    struct ms_json* value;
    if (error != NULL && error_size > 0)
        error[0] = '\0';
    if (data == NULL) {
        if (error != NULL && error_size > 0)
            (void)snprintf(error, error_size, "null input");
        return NULL;
    }
    p = (parser){data, length, 0, 0, error, error_size};
    skip_ws(&p);
    value = parse_value(&p);
    if (value == NULL)
        return NULL;
    skip_ws(&p);
    if (p.offset != p.length) {
        set_error(&p, "trailing data");
        ms_json_free(value);
        return NULL;
    }
    return value;
}

ms_json_type ms_json_type_of(const ms_json* value) {
    return value == NULL ? MS_JSON_INVALID : value->type;
}

const ms_json* ms_json_object_get(const ms_json* object, const char* key) {
    size_t i;
    if (object == NULL || object->type != MS_JSON_OBJECT || key == NULL)
        return NULL;
    for (i = 0; i < object->value.object.length; ++i) {
        if (strcmp(object->value.object.keys[i], key) == 0)
            return object->value.object.values[i];
    }
    return NULL;
}

size_t ms_json_object_length(const ms_json* object) {
    return (object != NULL && object->type == MS_JSON_OBJECT) ? object->value.object.length : 0;
}

const char* ms_json_object_key_at(const ms_json* object, size_t index) {
    if (object == NULL || object->type != MS_JSON_OBJECT || index >= object->value.object.length)
        return NULL;
    return object->value.object.keys[index];
}

const ms_json* ms_json_object_value_at(const ms_json* object, size_t index) {
    if (object == NULL || object->type != MS_JSON_OBJECT || index >= object->value.object.length)
        return NULL;
    return object->value.object.values[index];
}

const ms_json* ms_json_array_get(const ms_json* array, size_t index) {
    if (array == NULL || array->type != MS_JSON_ARRAY || index >= array->value.array.length)
        return NULL;
    return array->value.array.items[index];
}

size_t ms_json_array_length(const ms_json* array) {
    return (array != NULL && array->type == MS_JSON_ARRAY) ? array->value.array.length : 0;
}

bool ms_json_as_bool(const ms_json* value, bool* out) {
    if (value == NULL || value->type != MS_JSON_BOOL || out == NULL)
        return false;
    *out = value->value.boolean;
    return true;
}

bool ms_json_as_number(const ms_json* value, double* out) {
    if (value == NULL || value->type != MS_JSON_NUMBER || out == NULL)
        return false;
    *out = value->value.number;
    return true;
}

bool ms_json_as_i64(const ms_json* value, long long* out) {
    double number;
    if (out == NULL || !ms_json_as_number(value, &number) || number < (double)LLONG_MIN || number > (double)LLONG_MAX ||
        trunc(number) != number)
        return false;
    *out = (long long)number;
    return true;
}

bool ms_json_as_string(const ms_json* value, char** out) {
    char* copy;
    if (value == NULL || value->type != MS_JSON_STRING || out == NULL)
        return false;
    copy = strdup(value->value.string);
    if (copy == NULL)
        return false;
    *out = copy;
    return true;
}

typedef struct {
    char* data;
    size_t length;
    size_t capacity;
    bool failed;
} stringify_buffer;

static void stringify_append(stringify_buffer* buffer, const char* data, size_t length) {
    size_t required;
    size_t capacity;
    char* grown;
    if (buffer->failed || length > SIZE_MAX - buffer->length - 1) {
        buffer->failed = true;
        return;
    }
    required = buffer->length + length + 1;
    if (required > buffer->capacity) {
        capacity = buffer->capacity == 0 ? 256 : buffer->capacity;
        while (capacity < required) {
            if (capacity > SIZE_MAX / 2) {
                capacity = required;
                break;
            }
            capacity *= 2;
        }
        grown = (char*)realloc(buffer->data, capacity);
        if (grown == NULL) {
            buffer->failed = true;
            return;
        }
        buffer->data = grown;
        buffer->capacity = capacity;
    }
    memcpy(buffer->data + buffer->length, data, length);
    buffer->length += length;
    buffer->data[buffer->length] = '\0';
}

static void stringify_literal(stringify_buffer* buffer, const char* literal) {
    stringify_append(buffer, literal, strlen(literal));
}

static void stringify_value(stringify_buffer* buffer, const ms_json* value) {
    char number[64];
    size_t i;
    int length;
    if (value == NULL) {
        stringify_literal(buffer, "null");
        return;
    }
    switch (value->type) {
    case MS_JSON_NULL:
        stringify_literal(buffer, "null");
        break;
    case MS_JSON_BOOL:
        stringify_literal(buffer, value->value.boolean ? "true" : "false");
        break;
    case MS_JSON_NUMBER:
        length = snprintf(number, sizeof(number), "%.17g", value->value.number);
        if (length < 0 || (size_t)length >= sizeof(number))
            buffer->failed = true;
        else
            stringify_append(buffer, number, (size_t)length);
        break;
    case MS_JSON_STRING: {
        char* quoted = ms_json_quote(value->value.string);
        if (quoted == NULL)
            buffer->failed = true;
        else {
            stringify_literal(buffer, quoted);
            free(quoted);
        }
        break;
    }
    case MS_JSON_ARRAY:
        stringify_literal(buffer, "[");
        for (i = 0; i < value->value.array.length; ++i) {
            if (i != 0)
                stringify_literal(buffer, ",");
            stringify_value(buffer, value->value.array.items[i]);
        }
        stringify_literal(buffer, "]");
        break;
    case MS_JSON_OBJECT:
        stringify_literal(buffer, "{");
        for (i = 0; i < value->value.object.length; ++i) {
            char* quoted = ms_json_quote(value->value.object.keys[i]);
            if (i != 0)
                stringify_literal(buffer, ",");
            if (quoted == NULL) {
                buffer->failed = true;
            } else {
                stringify_literal(buffer, quoted);
                free(quoted);
            }
            stringify_literal(buffer, ":");
            stringify_value(buffer, value->value.object.values[i]);
        }
        stringify_literal(buffer, "}");
        break;
    default:
        buffer->failed = true;
        break;
    }
}

char* ms_json_stringify(const ms_json* value) {
    stringify_buffer buffer = {0};
    stringify_value(&buffer, value);
    if (buffer.failed || buffer.data == NULL) {
        free(buffer.data);
        return NULL;
    }
    return buffer.data;
}

char* ms_json_quote(const char* value) {
    size_t i;
    size_t length = 2;
    char* out;
    size_t offset = 0;
    if (value == NULL)
        value = "";
    for (i = 0; value[i] != '\0'; ++i) {
        switch (value[i]) {
        case '"':
        case '\\':
        case '\b':
        case '\f':
        case '\n':
        case '\r':
        case '\t':
            length += 2;
            break;
        default:
            length += ((unsigned char)value[i] < 0x20) ? 6 : 1;
            break;
        }
    }
    out = (char*)malloc(length + 1);
    if (out == NULL)
        return NULL;
    out[offset++] = '"';
    for (i = 0; value[i] != '\0'; ++i) {
        unsigned char c = (unsigned char)value[i];
        switch (c) {
        case '"':
            out[offset++] = '\\';
            out[offset++] = '"';
            break;
        case '\\':
            out[offset++] = '\\';
            out[offset++] = '\\';
            break;
        case '\b':
            out[offset++] = '\\';
            out[offset++] = 'b';
            break;
        case '\f':
            out[offset++] = '\\';
            out[offset++] = 'f';
            break;
        case '\n':
            out[offset++] = '\\';
            out[offset++] = 'n';
            break;
        case '\r':
            out[offset++] = '\\';
            out[offset++] = 'r';
            break;
        case '\t':
            out[offset++] = '\\';
            out[offset++] = 't';
            break;
        default:
            if (c < 0x20) {
                (void)snprintf(out + offset, 7, "\\u%04x", c);
                offset += 6;
            } else {
                out[offset++] = (char)c;
            }
            break;
        }
    }
    out[offset++] = '"';
    out[offset] = '\0';
    return out;
}
