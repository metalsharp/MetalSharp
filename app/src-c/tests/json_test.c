#include "metalsharp_backend/json.h"
#include "metalsharp_backend/json_writer.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void test_nested_values(void) {
    const char input[] =
        "{\"name\":\"Metal\\nSharp\",\"enabled\":true,\"count\":42,\"items\":[null,\"\\uD83D\\uDE80\"]}";
    char error[128];
    ms_json* root = ms_json_parse(input, strlen(input), error, sizeof(error));
    const ms_json* items;
    char* name = NULL;
    long long count = 0;
    bool enabled = false;

    assert(root != NULL);
    assert(error[0] == '\0');
    assert(ms_json_as_string(ms_json_object_get(root, "name"), &name));
    assert(strcmp(name, "Metal\nSharp") == 0);
    assert(ms_json_as_bool(ms_json_object_get(root, "enabled"), &enabled) && enabled);
    assert(ms_json_as_i64(ms_json_object_get(root, "count"), &count) && count == 42);
    items = ms_json_object_get(root, "items");
    assert(ms_json_array_length(items) == 2);
    assert(ms_json_type_of(ms_json_array_get(items, 0)) == MS_JSON_NULL);
    {
        char* rocket = NULL;
        assert(ms_json_as_string(ms_json_array_get(items, 1), &rocket));
        assert(strcmp(rocket, "\xF0\x9F\x9A\x80") == 0);
        free(rocket);
    }
    free(name);
    ms_json_free(root);
}

static void test_invalid_inputs(void) {
    const char* invalid[] = {
        "{\"duplicate\":1,\"duplicate\":2}", "[1,]", "{\"x\": 1} trailing", "\"unterminated", "\"\\uD800\"",
    };
    size_t i;
    for (i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        char error[128];
        ms_json* value = ms_json_parse(invalid[i], strlen(invalid[i]), error, sizeof(error));
        assert(value == NULL);
        assert(error[0] != '\0');
    }
}

static void test_parser_limits(void) {
    char deep[610];
    char error[128];
    ms_json* value;
    size_t i;
    for (i = 0; i < 300; ++i)
        deep[i] = '[';
    deep[300] = '0';
    for (i = 0; i < 300; ++i)
        deep[301 + i] = ']';
    deep[601] = '\0';
    value = ms_json_parse(deep, strlen(deep), error, sizeof(error));
    assert(value == NULL);
    assert(ms_json_parse("9223372036854775808", 19, error, sizeof(error)) == NULL);
    assert(ms_json_parse("-9223372036854775809", 20, error, sizeof(error)) == NULL);
    assert(ms_json_parse("9007199254740992", 16, error, sizeof(error)) == NULL);
}

static void test_writer(void) {
    ms_json_writer writer;
    char* json;
    ms_json_writer_init(&writer);
    ms_json_writer_object_begin(&writer);
    ms_json_writer_key(&writer, "ok");
    ms_json_writer_bool(&writer, true);
    ms_json_writer_key(&writer, "items");
    ms_json_writer_array_begin(&writer);
    ms_json_writer_string(&writer, "one");
    ms_json_writer_object_begin(&writer);
    ms_json_writer_key(&writer, "n");
    ms_json_writer_i64(&writer, -2);
    ms_json_writer_object_end(&writer);
    ms_json_writer_array_end(&writer);
    ms_json_writer_object_end(&writer);
    json = ms_json_writer_take(&writer);
    assert(json != NULL);
    assert(strcmp(json, "{\"ok\":true,\"items\":[\"one\",{\"n\":-2}]}") == 0);
    free(json);
}

static void test_quote(void) {
    char* quoted = ms_json_quote("line\n\"quote\"");
    assert(quoted != NULL);
    assert(strcmp(quoted, "\"line\\n\\\"quote\\\"\"") == 0);
    free(quoted);
}

int main(void) {
    test_nested_values();
    test_invalid_inputs();
    test_quote();
    test_parser_limits();
    test_writer();
    puts("json tests passed");
    return 0;
}
