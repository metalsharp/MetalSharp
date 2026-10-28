/* A finished setup records the build that set the home up, so the next start doesn't migrate a fresh
 * install (built per variant: a fresh FEX setup used to read as a 0.80.x x86_64 one and get the full
 * regular -> FEX migration, with the "moved from x86_64" welcome, at its second launch). */
#include "../runtime/setup.c"
#include "../runtime/migration.c"
#include <assert.h>

/* migration.c's Steam and Ubisoft helpers, which these tests don't reach. */
char* ms_steam_stop_json(const char* home, int* status) {
    (void)home, (void)status;
    return NULL;
}
char* ms_ubisoft_stop_json(const char* home, int* status) {
    (void)home, (void)status;
    return NULL;
}
bool ms_steam_usability_ensure(const char* home) {
    (void)home;
    return true;
}
bool ms_steam_wrappers_ensure(const char* home) {
    (void)home;
    return true;
}

static char* recorded(const char* home, const char* key) {
    char* path = join_path(home, "setup.json");
    char* text = read_text(path);
    char error[96];
    ms_json* setup = text ? ms_json_parse(text, strlen(text), error, sizeof(error)) : NULL;
    char* value = setup ? json_str(setup, key) : NULL;
    ms_json_free(setup);
    free(text);
    free(path);
    return value;
}

static void write_text_file(const char* path, const char* text) {
    FILE* file = fopen(path, "w");
    assert(file && fputs(text, file) >= 0 && fclose(file) == 0);
}

static char* fresh_home(const char* name) {
    char path[256], command[512];
    snprintf(path, sizeof(path), "/tmp/metalsharp-setup-variant-%s-%ld", name, (long)getpid());
    snprintf(command, sizeof(command), "rm -rf '%s' && mkdir -p '%s'", path, path);
    assert(system(command) == 0);
    return strdup(path);
}

static void test_bundled_archives(const char* home) {
    static const char* const names[] = {"metalsharp-runtime.tar.zst",       "metalsharp-graphics-dll.tar.zst",
                                        "metalsharp-assets.tar.zst",        "fnalibs.tar.zst",
                                        "metalsharp-scripts-tools.tar.zst", "metalsharp-steam.tar.zst"};
    char *bundled = join_path(home, "app-resources/bundles"), *cache = join_path(home, "cache/bundles"),
         *legacy = join_path(home, "bundles");
    char* previous = getenv("METALSHARP_BUNDLE_DIR") ? strdup(getenv("METALSHARP_BUNDLE_DIR")) : NULL;
    assert(bundled && cache && legacy && mkdir_p(bundled) && mkdir_p(cache) && mkdir_p(legacy));
    assert(setenv("METALSHARP_BUNDLE_DIR", bundled, 1) == 0);
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        char *cached = join_path(cache, names[i]), *old = join_path(legacy, names[i]),
             *loose = join_path(home, names[i]), *expected = join_path(bundled, names[i]), *actual;
        assert(cached && old && loose && expected);
        write_text_file(cached, "untrusted cached download");
        write_text_file(old, "untrusted legacy archive");
        write_text_file(loose, "untrusted loose archive");
        assert(find_bundle_archive(names[i]) == NULL);
        assert(!extract_split_bundle(home, names[i], NULL, 0));

        write_text_file(expected, "packaged archive");
        actual = find_bundle_archive(names[i]);
        assert(actual && !strcmp(actual, expected));
        free(actual);
        assert(unlink(expected) == 0);
        assert(find_bundle_archive(names[i]) == NULL);
        free(cached);
        free(old);
        free(loose);
        free(expected);
    }
    assert(previous ? setenv("METALSHARP_BUNDLE_DIR", previous, 1) == 0 : unsetenv("METALSHARP_BUNDLE_DIR") == 0);
    free(previous);
    free(bundled);
    free(cache);
    free(legacy);
}

int main(void) {
    static const char finished[] = "{\"step\":2,\"deviceName\":\"Mac\",\"completed\":true}";
    char* home = fresh_home("fresh");
    char *variant, *version, *json, path[512], command[600];

    test_bundled_archives(home);

    /* A fresh setup of this build, finished in the wizard: the second launch migrates nothing. */
    json = ms_setup_save_json(home, (const unsigned char*)finished, strlen(finished), NULL);
    free(json);
    variant = recorded(home, "runtime_variant");
    version = recorded(home, "last_migrated_version");
    assert(variant && !strcmp(variant, ms_wine_variant()));
    assert(version && !strcmp(version, MS_BACKEND_VERSION));
    assert(ms_migration_path_for(ms_wine_variant(), ms_wine_recorded_variant(home, variant), version, true, false,
                                 true) == MS_MIGRATION_PATH_NONE);
    assert(!ms_migration_fex_welcome_due(ms_migration_path_for(
        ms_wine_variant(), ms_wine_recorded_variant(home, variant), version, true, false, true)));
    free(variant);
    free(version);
    snprintf(command, sizeof(command), "rm -rf '%s'", home);
    assert(system(command) == 0);
    free(home);

    /* Without runtime_variant, a Steam prefix MetalSharp Wine built reads as the FEX build's. */
    home = fresh_home("marker");
    snprintf(path, sizeof(path), "%s/prefix-steam", home);
    assert(mkdir_p(path));
    assert(!strcmp(ms_wine_recorded_variant(home, NULL), "regular"));
    snprintf(path, sizeof(path), "%s/prefix-steam/.metalsharp-wine-arch", home);
    write_text_file(path, "arm64\n");
    assert(!strcmp(ms_wine_recorded_variant(home, NULL), "fex"));
    assert(!strcmp(ms_wine_recorded_variant(home, "regular"), "regular"));
    assert(ms_migration_path_for("fex", ms_wine_recorded_variant(home, NULL), NULL, true, false, true) ==
           MS_MIGRATION_PATH_NONE);
    snprintf(command, sizeof(command), "rm -rf '%s'", home);
    assert(system(command) == 0);
    free(home);

    /* Setup over the other build's Steam prefix records nothing: that install still has to move, which
     * migration does (and then records the build). */
    home = fresh_home("other");
    snprintf(path, sizeof(path), "%s/prefix-steam", home);
    assert(mkdir_p(path));
    snprintf(path, sizeof(path), "%s/prefix-steam/system.reg", home);
    write_text_file(path, "WINE REGISTRY Version 2\n");
    if (!ms_wine_is_fex()) {
        snprintf(path, sizeof(path), "%s/prefix-steam/.metalsharp-wine-arch", home);
        write_text_file(path, "arm64\n");
    }
    json = ms_setup_save_json(home, (const unsigned char*)finished, strlen(finished), NULL);
    free(json);
    variant = recorded(home, "runtime_variant");
    assert(!variant);
    assert(ms_migration_path_for(ms_wine_variant(), ms_wine_recorded_variant(home, NULL), NULL, true, false, true) ==
           (ms_wine_is_fex() ? MS_MIGRATION_PATH_REGULAR_TO_FEX : MS_MIGRATION_PATH_FEX_TO_REGULAR));
    snprintf(command, sizeof(command), "rm -rf '%s'", home);
    assert(system(command) == 0);
    free(home);

    printf("setup variant record tests passed (%s)\n", ms_wine_variant());
    return 0;
}
