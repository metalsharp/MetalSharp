/* Library locations without real drives: a folder stands in for /Volumes (METALSHARP_VOLUMES_ROOT)
 * and HOME for the user's home. Volume filtering, the saved choice, the launcher folders, GOG and
 * Epic install requests, and the drive letter rule in fake prefixes. */
#include "metalsharp_backend/library_locations.h"
#include "metalsharp_backend/json.h"
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char base[PATH_MAX];

static char* at(const char* relative) {
    static char paths[16][PATH_MAX];
    static unsigned next;
    char* out = paths[next++ % 16];
    snprintf(out, PATH_MAX, "%s/%s", base, relative);
    return out;
}

static void make_dir(const char* relative) {
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s", at(relative));
    for (char* p = path + strlen(base) + 1; *p; p++)
        if (*p == '/') {
            *p = '\0';
            assert(mkdir(path, 0755) == 0 || errno == EEXIST);
            *p = '/';
        }
    assert(mkdir(path, 0755) == 0 || errno == EEXIST);
}

static void make_file(const char* relative) {
    FILE* file = fopen(at(relative), "wb");
    assert(file && fputs("x", file) >= 0 && fclose(file) == 0);
}

static ms_json* parse(const char* raw) {
    ms_json* json = raw ? ms_json_parse(raw, strlen(raw), NULL, 0) : NULL;
    assert(json);
    return json;
}

static bool string_is(const ms_json* object, const char* key, const char* expected) {
    char* text = NULL;
    bool match = ms_json_as_string(ms_json_object_get(object, key), &text) && !strcmp(text, expected);
    free(text);
    return match;
}

static bool bool_is(const ms_json* object, const char* key, bool expected) {
    bool value = !expected;
    return ms_json_as_bool(ms_json_object_get(object, key), &value) && value == expected;
}

static bool link_is(const char* relative, const char* expected) {
    char target[PATH_MAX];
    ssize_t length = readlink(at(relative), target, sizeof(target) - 1);
    if (length <= 0)
        return false;
    target[length] = '\0';
    return !strcmp(target, expected);
}

static char* set(const char* home, const char* launcher, const char* volume, int* status) {
    char body[2048];
    snprintf(body, sizeof(body), "{\"launcher\":\"%s\",\"volume\":\"%s\"}", launcher, volume);
    return ms_library_locations_set_json(home, body, strlen(body), status);
}

static void test_eligibility(void) {
    ms_library_volume_facts good = {.directory = true, .mount_root = true, .local = true, .writable = true,
                                    .fs_type = "apfs"};
    ms_library_volume_facts facts;
    assert(ms_library_volume_eligible("AverySSD", &good));
    assert(!ms_library_volume_eligible(".Spotlight", &good));
    assert(!ms_library_volume_eligible("Recovery", &good));
    assert(!ms_library_volume_eligible("Macintosh HD", &good));
    assert(!ms_library_volume_eligible("", &good));
    facts = good, facts.symlink = true;
    assert(!ms_library_volume_eligible("Macintosh HD - Data", &facts)); /* the boot volume's alias */
    facts = good, facts.boot_device = true;
    assert(!ms_library_volume_eligible("Data", &facts));
    facts = good, facts.mount_root = false;
    assert(!ms_library_volume_eligible("Leftover", &facts)); /* a stale folder in /Volumes */
    facts = good, facts.read_only = true;
    assert(!ms_library_volume_eligible("Some.dmg", &facts));
    facts = good, facts.hidden = true;
    assert(!ms_library_volume_eligible("Preboot2", &facts));
    facts = good, facts.local = false;
    assert(!ms_library_volume_eligible("NAS", &facts));
    facts = good, facts.fs_type = "smbfs";
    assert(!ms_library_volume_eligible("Share", &facts));
    facts = good, facts.writable = false;
    assert(!ms_library_volume_eligible("Locked", &facts));
    facts = good, facts.time_machine = true;
    assert(!ms_library_volume_eligible("Backup", &facts));
    facts = good, facts.metalsharp_image = true;
    assert(!ms_library_volume_eligible("MetalSharp", &facts));
}

static void test_volumes(void) {
    ms_library_volume volumes[16];
    size_t count;
    char* internal = ms_library_internal_root();
    char* best;
    assert(internal && !strcmp(internal, at("user/Games")));
    free(internal);
    make_dir("volumes/AverySSD");
    make_dir("volumes/Zeta");
    make_dir("volumes/.hidden");
    make_dir("volumes/Recovery");
    make_dir("volumes/Backup/Backups.backupdb");
    make_dir("volumes/MetalSharp/MetalSharp.app");
    make_dir("volumes/Locked");
    assert(chmod(at("volumes/Locked"), 0555) == 0);
    make_file("volumes/notes.txt");
    assert(symlink("/", at("volumes/Macintosh HD 2")) == 0);
    count = ms_library_volumes(volumes, 16);
    assert(count == 3);
    assert(!strcmp(volumes[0].id, "internal") && !strcmp(volumes[0].name, "This Mac") && !volumes[0].external &&
           !strcmp(volumes[0].root, at("user/Games")) && volumes[0].free_bytes > 0);
    assert(!strcmp(volumes[1].id, at("volumes/AverySSD")) && !strcmp(volumes[1].name, "AverySSD") &&
           !strcmp(volumes[1].root, at("volumes/AverySSD")) && volumes[1].external);
    assert(!strcmp(volumes[2].name, "Zeta"));
    best = ms_library_best_external_volume();
    assert(best && (strstr(best, "/volumes/AverySSD") || strstr(best, "/volumes/Zeta")));
    free(best);
    assert(ms_library_volumes(volumes, 1) == 1);
}

static void test_set_and_get(const char* home) {
    int status = 0;
    char* raw = ms_library_locations_json(home);
    ms_json *root = parse(raw), *answer;
    const ms_json* locations = ms_json_object_get(root, "locations");
    char path[PATH_MAX];
    ms_library_location location;
    bool available = false;
    assert(bool_is(root, "ok", true) && ms_json_array_length(ms_json_object_get(root, "volumes")) == 3);
    assert(ms_json_type_of(ms_json_object_get(locations, "gog")) == MS_JSON_NULL &&
           ms_json_type_of(ms_json_object_get(locations, "rockstar")) == MS_JSON_NULL);
    ms_json_free(root);
    free(raw);

    /* This Mac: <home>/Games is created with the folder. */
    raw = set(home, "gog", "internal", &status);
    answer = parse(raw);
    snprintf(path, sizeof(path), "%s", at("user/Games/MetalSharp GOG Games"));
    assert(status == 200 && bool_is(answer, "ok", true) && string_is(answer, "path", path));
    assert(!ms_json_object_get(answer, "drive")); /* GOG has no prefix of its own */
    ms_json_free(answer);
    free(raw);
    {
        struct stat st;
        assert(stat(path, &st) == 0 && S_ISDIR(st.st_mode));
        assert(stat(at("ms-home/config/library-locations.json"), &st) == 0 && S_ISREG(st.st_mode));
    }
    assert(ms_library_location_get(home, "gog", &location, &available) && available &&
           !strcmp(location.volume, "internal") && !strcmp(location.path, path));
    assert(!ms_library_location_get(home, "epic", &location, &available));

    /* An external volume, by its mount point. */
    raw = set(home, "epic", at("volumes/AverySSD"), &status);
    answer = parse(raw);
    assert(status == 200 && string_is(answer, "path", at("volumes/AverySSD/MetalSharp Epic Games")));
    ms_json_free(answer);
    free(raw);

    raw = ms_library_locations_json(home);
    root = parse(raw);
    locations = ms_json_object_get(root, "locations");
    assert(string_is(ms_json_object_get(locations, "gog"), "volume", "internal") &&
           bool_is(ms_json_object_get(locations, "gog"), "available", true));
    assert(string_is(ms_json_object_get(locations, "epic"), "volume", at("volumes/AverySSD")) &&
           string_is(ms_json_object_get(locations, "epic"), "path", at("volumes/AverySSD/MetalSharp Epic Games")));
    assert(ms_json_type_of(ms_json_object_get(locations, "ubisoft")) == MS_JSON_NULL);
    ms_json_free(root);
    free(raw);

    /* Bad requests. */
    raw = set(home, "origin", "internal", &status);
    assert(status == 400 && strstr(raw, "\"ok\":false"));
    free(raw);
    raw = set(home, "gog", "/Volumes/NotConnected", &status);
    assert(status == 400);
    free(raw);
    raw = set(home, "gog", at("volumes/Recovery"), &status);
    assert(status == 400);
    free(raw);
    raw = ms_library_locations_set_json(home, "not json", 8, &status);
    assert(status == 400);
    free(raw);

    /* The folder can't be made: 409, and the saved choice is unchanged. */
    make_file("volumes/Zeta/MetalSharp GOG Games");
    raw = set(home, "gog", at("volumes/Zeta"), &status);
    assert(status == 409 && strstr(raw, "Could not create"));
    free(raw);
    assert(ms_library_location_get(home, "gog", &location, NULL) && !strcmp(location.volume, "internal"));

    assert(ms_library_is_managed_root(at("user/Games")));
    assert(ms_library_is_managed_root(path));
    assert(!ms_library_is_managed_root(at("user")));
    {
        char game[PATH_MAX];
        snprintf(game, sizeof(game), "%s/Some Game", path);
        assert(mkdir(game, 0755) == 0);
        assert(!ms_library_is_managed_root(game));
    }
}

static void test_install_requests(const char* home) {
    const char* request = "{\"productId\":\"1207658924\",\"platform\":\"windows\",\"language\":\"en-US\"}";
    size_t length = 0;
    int status = 0;
    char* failure = NULL;
    char* body;
    ms_json* parsed;

    /* GOG goes to the folder on This Mac; the request's other fields stay. */
    body = ms_library_install_request(home, "gog", request, strlen(request), &length, &status, &failure);
    parsed = parse(body);
    assert(status == 200 && !failure && length == strlen(body));
    assert(string_is(parsed, "installPath", at("user/Games/MetalSharp GOG Games")) &&
           string_is(parsed, "productId", "1207658924") && string_is(parsed, "language", "en-US"));
    ms_json_free(parsed);
    free(body);

    /* An empty installPath counts as none. */
    request = "{\"appName\":\"Fortnite\",\"installPath\":\"\"}";
    body = ms_library_install_request(home, "epic", request, strlen(request), &length, &status, &failure);
    parsed = parse(body);
    assert(status == 200 && string_is(parsed, "installPath", at("volumes/AverySSD/MetalSharp Epic Games")) &&
           string_is(parsed, "appName", "Fortnite"));
    ms_json_free(parsed);
    free(body);

    /* A folder the user picked is kept as it was sent. */
    request = "{\"appName\":\"Fortnite\",\"installPath\":\"/Users/someone/Epic\"}";
    body = ms_library_install_request(home, "epic", request, strlen(request), &length, &status, &failure);
    assert(body && status == 200 && !strcmp(body, request) && length == strlen(request));
    free(body);

    /* No location yet: 409 needs_location. */
    {
        char other_home[PATH_MAX];
        snprintf(other_home, sizeof(other_home), "%s", at("empty-home"));
        make_dir("empty-home");
        request = "{\"appName\":\"Fortnite\"}";
        body = ms_library_install_request(other_home, "epic", request, strlen(request), &length, &status, &failure);
        assert(!body && status == 409 && failure);
        parsed = parse(failure);
        assert(bool_is(parsed, "ok", false) && bool_is(parsed, "needs_location", true) &&
               string_is(parsed, "error", "Choose where Epic games go first"));
        ms_json_free(parsed);
        free(failure);
        body = ms_library_install_request(other_home, "gog", "{}", 2, &length, &status, &failure);
        assert(!body && status == 409 && failure && strstr(failure, "Choose where GOG games go first"));
        free(failure);
    }

    /* The saved drive is gone: unavailable, and installs ask again. Nothing is created for it. */
    assert(rename(at("volumes/AverySSD"), at("unplugged-AverySSD")) == 0);
    {
        char* raw = ms_library_locations_json(home);
        ms_json* root = parse(raw);
        const ms_json* epic = ms_json_object_get(ms_json_object_get(root, "locations"), "epic");
        assert(bool_is(epic, "available", false));
        ms_json_free(root);
        free(raw);
    }
    request = "{\"appName\":\"Fortnite\"}";
    body = ms_library_install_request(home, "epic", request, strlen(request), &length, &status, &failure);
    assert(!body && status == 409 && failure && strstr(failure, "isn't connected") &&
           strstr(failure, "\"needs_location\":true"));
    free(failure);
    assert(ms_library_location_apply(home, "epic")); /* saved, skipped */
    assert(access(at("volumes/AverySSD"), F_OK) != 0);
    assert(rename(at("unplugged-AverySSD"), at("volumes/AverySSD")) == 0);
}

static void test_drive_letters(const char* home) {
    int status = 0;
    char* raw;
    ms_json* answer;
    ms_library_location location;

    /* Ubisoft: Y: from MetalSharp's old automatic mapping (a volume root) is replaced. */
    make_dir("ms-home/prefix-ubisoft/dosdevices");
    assert(symlink(at("volumes/Zeta"), at("ms-home/prefix-ubisoft/dosdevices/y:")) == 0);
    raw = set(home, "ubisoft", at("volumes/AverySSD"), &status);
    answer = parse(raw);
    assert(status == 200 && string_is(answer, "drive", "Y:"));
    ms_json_free(answer);
    free(raw);
    assert(link_is("ms-home/prefix-ubisoft/dosdevices/y:", at("volumes/AverySSD/MetalSharp Ubisoft Games")));
    /* Again (a launcher start): kept. Moved to This Mac: MetalSharp's own Y: follows. */
    assert(ms_library_location_apply(home, "ubisoft"));
    raw = set(home, "ubisoft", "internal", &status);
    assert(status == 200 && strstr(raw, "\"drive\":\"Y:\""));
    free(raw);
    assert(link_is("ms-home/prefix-ubisoft/dosdevices/y:", at("user/Games/MetalSharp Ubisoft Games")));

    /* Battle.net: the user's own Y: to another place stays; the folder takes X:. */
    make_dir("elsewhere");
    make_dir("ms-home/prefix-battlenet/dosdevices");
    assert(symlink(at("elsewhere"), at("ms-home/prefix-battlenet/dosdevices/y:")) == 0);
    raw = set(home, "battlenet", at("volumes/AverySSD"), &status);
    answer = parse(raw);
    assert(status == 200 && string_is(answer, "drive", "X:"));
    ms_json_free(answer);
    free(raw);
    assert(link_is("ms-home/prefix-battlenet/dosdevices/y:", at("elsewhere")));
    assert(link_is("ms-home/prefix-battlenet/dosdevices/x:", at("volumes/AverySSD/MetalSharp Battle.net Games")));
    /* The user removes their Y:: MetalSharp moves to Y: and takes X: back. */
    assert(unlink(at("ms-home/prefix-battlenet/dosdevices/y:")) == 0);
    assert(ms_library_location_apply(home, "battlenet"));
    assert(link_is("ms-home/prefix-battlenet/dosdevices/y:", at("volumes/AverySSD/MetalSharp Battle.net Games")));
    assert(access(at("ms-home/prefix-battlenet/dosdevices/x:"), F_OK) != 0 &&
           unlink(at("ms-home/prefix-battlenet/dosdevices/x:")) != 0);
    assert(ms_library_location_get(home, "battlenet", &location, NULL) && !strcmp(location.drive, "y:"));

    /* Rockstar: R: in prefix-steam; Steam's library letters are never touched. */
    {
        char* raw_locations;
        make_dir("ms-home/prefix-steam/dosdevices");
        assert(symlink(at("volumes/Zeta"), at("ms-home/prefix-steam/dosdevices/d:")) == 0);
        raw = set(home, "rockstar", at("volumes/AverySSD"), &status);
        assert(status == 200 && strstr(raw, "\"drive\":\"R:\""));
        free(raw);
        assert(link_is("ms-home/prefix-steam/dosdevices/r:", at("volumes/AverySSD/MetalSharp Rockstar Games")));
        assert(link_is("ms-home/prefix-steam/dosdevices/d:", at("volumes/Zeta")));
        raw_locations = ms_library_locations_json(home);
        answer = parse(raw_locations);
        assert(string_is(ms_json_object_get(ms_json_object_get(answer, "locations"), "rockstar"), "drive", "R:"));
        ms_json_free(answer);
        free(raw_locations);
    }

    /* The rule itself. */
    {
        char prefix[PATH_MAX], target[PATH_MAX];
        snprintf(prefix, sizeof(prefix), "%s", at("ms-home/rule-prefix"));
        snprintf(target, sizeof(target), "%s", at("user/Games/MetalSharp Rockstar Games"));
        make_dir("ms-home/rule-prefix/dosdevices/r:"); /* a real folder */
        assert(ms_library_map_drive(prefix, 'r', target, NULL, 0) == MS_LIBRARY_DRIVE_USER);
        assert(symlink("/nowhere/at/all", at("ms-home/rule-prefix/dosdevices/s:")) == 0);
        assert(ms_library_map_drive(prefix, 's', target, NULL, 0) == MS_LIBRARY_DRIVE_USER);
        assert(ms_library_map_drive(prefix, 's', target, NULL, MS_LIBRARY_DRIVE_REPLACE_BROKEN) ==
               MS_LIBRARY_DRIVE_REPLACED);
        assert(ms_library_map_drive(prefix, 's', target, NULL, 0) == MS_LIBRARY_DRIVE_KEPT);
        assert(symlink(at("volumes/Zeta"), at("ms-home/rule-prefix/dosdevices/t:")) == 0);
        assert(ms_library_map_drive(prefix, 't', target, NULL, 0) == MS_LIBRARY_DRIVE_USER);
        assert(ms_library_map_drive(prefix, 't', target, NULL, MS_LIBRARY_DRIVE_LEGACY_VOLUME_ROOT) ==
               MS_LIBRARY_DRIVE_REPLACED);
        assert(symlink(at("elsewhere"), at("ms-home/rule-prefix/dosdevices/u:")) == 0);
        assert(ms_library_map_drive(prefix, 'u', target, at("elsewhere"), 0) == MS_LIBRARY_DRIVE_REPLACED);
        assert(symlink(at("volumes/Zeta/MetalSharp GOG Games"), at("ms-home/rule-prefix/dosdevices/v:")) == 0);
        assert(ms_library_map_drive(prefix, 'v', target, NULL, 0) == MS_LIBRARY_DRIVE_REPLACED);
        assert(ms_library_map_drive(prefix, 'w', target, NULL, 0) == MS_LIBRARY_DRIVE_CREATED);
        assert(ms_library_map_drive(prefix, 'c', target, NULL, 0) == MS_LIBRARY_DRIVE_FAILED);
        assert(ms_library_map_drive(prefix, 'z', target, NULL, 0) == MS_LIBRARY_DRIVE_FAILED);
    }
}

int main(int argc, char** argv) {
    char home[PATH_MAX];
    assert(argc == 2);
    assert(realpath(argv[1], base));
    make_dir("user");
    make_dir("volumes");
    make_dir("ms-home");
    assert(setenv("HOME", at("user"), 1) == 0);
    assert(setenv(MS_LIBRARY_VOLUMES_ROOT_ENV, at("volumes"), 1) == 0);
    snprintf(home, sizeof(home), "%s", at("ms-home"));
    assert(!strcmp(ms_library_volumes_root(), at("volumes")));
    assert(!strcmp(ms_library_folder_name("battlenet"), "MetalSharp Battle.net Games") &&
           !ms_library_folder_name("steam"));
    test_eligibility();
    test_volumes();
    test_set_and_get(home);
    test_install_requests(home);
    test_drive_letters(home);
    assert(chmod(at("volumes/Locked"), 0755) == 0);
    printf("library locations test passed\n");
    return 0;
}
