/* Each build reads its own rule set: the regular build configs/mtsp-rules.toml, the Arm64 FEX build
 * configs/mtsp-rules-fex.toml (built twice, -DMS_BACKEND_VARIANT="regular" and "fex"). On FEX, every
 * rule's lane is the one the backend translated the regular rule to before the FEX build had a rule
 * set of its own, except the games verified on MetalSharp Wine. */
#include "metalsharp_backend/json.h"
#include "metalsharp_backend/mtsp.h"
#include "metalsharp_backend/steam.h"
#include "metalsharp_backend/steam_actions.h"
#include "metalsharp_backend/wine_runtime.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* mtsp.c's Steam helpers, which these tests don't reach. */
char* ms_steam_mtsp_inspect_json(const char* home, const unsigned char* body, size_t length, int* status, int mode) {
    (void)home, (void)body, (void)length, (void)status, (void)mode;
    return NULL;
}
char* ms_steam_game_dir(const char* home, unsigned appid) {
    (void)home, (void)appid;
    return NULL;
}
const char* ms_steam_detect_graphics_pipeline(const char* game_dir) {
    (void)game_dir;
    return NULL;
}

static char* rule_pipeline(const ms_json* rules, long long appid) {
    for (size_t i = 0; i < ms_json_array_length(rules); i++) {
        const ms_json* rule = ms_json_array_get(rules, i);
        long long id = 0;
        char* pipeline = NULL;
        if (ms_json_as_i64(ms_json_object_get(rule, "appid"), &id) && id == appid &&
            ms_json_as_string(ms_json_object_get(rule, "default_pipeline"), &pipeline))
            return pipeline;
    }
    return NULL;
}

static bool verified_on_fex(long long appid) {
    return appid == 2054970 || appid == 1174180 || appid == 2322010 || appid == 3527290;
}

int main(void) {
    char error[96], line[256];
    char* regular_raw = ms_mtsp_default_rules_json_raw();
    char* rules_raw = ms_mtsp_default_rules_json();
    ms_json* regular = regular_raw ? ms_json_parse(regular_raw, strlen(regular_raw), error, sizeof(error)) : NULL;
    ms_json* rules = rules_raw ? ms_json_parse(rules_raw, strlen(rules_raw), error, sizeof(error)) : NULL;
    const ms_json* regular_list = regular ? ms_json_object_get(regular, "rules") : NULL;
    const ms_json* list = rules ? ms_json_object_get(rules, "rules") : NULL;
    FILE* file = ms_mtsp_open_rules_file();
    bool own_build = false;
    assert(regular_list && list);

    /* Its own file, which says which build it is for. */
    assert(!strcmp(ms_wine_rules_file(), ms_wine_is_fex() ? "mtsp-rules-fex.toml" : "mtsp-rules.toml"));
    assert(file);
    while (fgets(line, sizeof(line), file))
        if (!strncmp(line, "build = ", 8))
            own_build = !strcmp(line, ms_wine_is_fex() ? "build = \"fex\"\n" : "build = \"regular\"\n");
    fclose(file);
    assert(own_build);

    if (!ms_wine_is_fex()) {
        char* rdr2 = rule_pipeline(list, 1174180);
        assert(rdr2 && !strcmp(rdr2, "d3dmetal"));
        assert(!rule_pipeline(list, 2054970));
        free(rdr2);
        printf("regular rule set tests passed\n");
    } else {
        /* The old runtime translation, rule by rule. */
        for (size_t i = 0; i < ms_json_array_length(regular_list); i++) {
            long long appid = 0;
            char *before = NULL, *after;
            const ms_json* rule = ms_json_array_get(regular_list, i);
            (void)ms_json_as_i64(ms_json_object_get(rule, "appid"), &appid);
            (void)ms_json_as_string(ms_json_object_get(rule, "default_pipeline"), &before);
            after = rule_pipeline(list, appid);
            assert(after);
            if (!verified_on_fex(appid)) {
                if (strcmp(after, ms_mtsp_fex_lane(before)))
                    fprintf(stderr, "appid %lld: %s -> %s, expected %s\n", appid, before, after,
                            ms_mtsp_fex_lane(before));
                assert(!strcmp(after, ms_mtsp_fex_lane(before)));
            }
            free(before);
            free(after);
        }
        /* Verified on MetalSharp Wine. */
        {
            static const struct {
                long long appid;
                const char* lane;
            } verified[] = {{2054970, "dxmt"}, {1174180, "dxmt"}, {2322010, "dxmt"}, {3527290, "vkd3d"}};
            for (size_t i = 0; i < sizeof(verified) / sizeof(verified[0]); i++) {
                char* lane = rule_pipeline(list, verified[i].appid);
                assert(lane && !strcmp(lane, verified[i].lane));
                free(lane);
            }
        }
        /* The lanes MetalSharp Wine has. */
        assert(!strcmp(ms_mtsp_fex_lane("d3dmetal"), "vkd3d") && !strcmp(ms_mtsp_fex_lane("d3d9"), "dxvk") &&
               !strcmp(ms_mtsp_fex_lane("m11_32"), "dxmt_32") && !strcmp(ms_mtsp_fex_lane("fna_arm64"), "dxmt"));
        printf("FEX rule set tests passed\n");
    }
    ms_json_free(regular);
    ms_json_free(rules);
    free(regular_raw);
    free(rules_raw);
    return 0;
}
