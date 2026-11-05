/* Uninstalling a launcher MetalSharp manages (launcher_uninstall.h). Everything removed is inside the
 * MetalSharp home and reached without following a link, except the one games folder that
 * library_locations.c validates; trees are removed without following links (as rm -rf does). */
#include "metalsharp_backend/launcher_uninstall.h"
#include "metalsharp_backend/battlenet.h"
#include "metalsharp_backend/epic.h"
#include "metalsharp_backend/gog.h"
#include "metalsharp_backend/json.h"
#include "metalsharp_backend/json_writer.h"
#include "metalsharp_backend/library_locations.h"
#include "metalsharp_backend/rockstar.h"
#include "metalsharp_backend/store_launcher.h"
#include "metalsharp_backend/ubisoft.h"
#include "metalsharp_backend/wine_runtime.h"
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    const char* id;
    const char* label;
} uninstall_launcher;

static const uninstall_launcher LAUNCHERS[] = {
    {"ubisoft", "Ubisoft Connect"}, {"battlenet", "Battle.net"}, {"rockstar", "the Rockstar Games Launcher"},
    {"gog", "GOG"},                 {"epic", "Epic Games"},
};

/* The Rockstar Games Launcher's own folders in prefix-steam (Steam and the games stay). */
static const char* const ROCKSTAR_FOLDERS[] = {
    "prefix-steam/drive_c/Program Files/Rockstar Games/Launcher",
    "prefix-steam/drive_c/Program Files/Rockstar Games/Social Club",
    "prefix-steam/drive_c/Program Files (x86)/Rockstar Games/Launcher",
    "prefix-steam/drive_c/Program Files (x86)/Rockstar Games/Social Club",
    "prefix-steam/drive_c/ProgramData/Rockstar Games/Launcher",
};
/* Removed only when the launcher's folders were all they held. */
static const char* const ROCKSTAR_PARENTS[] = {
    "prefix-steam/drive_c/Program Files/Rockstar Games",
    "prefix-steam/drive_c/Program Files (x86)/Rockstar Games",
    "prefix-steam/drive_c/ProgramData/Rockstar Games",
};

typedef struct {
    char home[PATH_MAX]; /* resolved */
    char** removed;
    size_t count;
    size_t capacity;
    char failed_path[PATH_MAX];
    char error[1200];
} uninstall_run;

static void sleep_ms(long ms) {
    struct timespec delay = {ms / 1000, (ms % 1000) * 1000000L};
    while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {
    }
}

static bool note_removed(uninstall_run* run, const char* path) {
    if (run->count == run->capacity) {
        size_t capacity = run->capacity ? run->capacity * 2 : 16;
        char** grown = realloc(run->removed, capacity * sizeof(*grown));
        if (!grown)
            return false;
        run->removed = grown;
        run->capacity = capacity;
    }
    if (!(run->removed[run->count] = strdup(path)))
        return false;
    run->count++;
    return true;
}

static bool fail(uninstall_run* run, const char* message, const char* path) {
    snprintf(run->failed_path, sizeof(run->failed_path), "%s", path ? path : "");
    snprintf(run->error, sizeof(run->error), "%s%s%s", message, path ? ": " : "", path ? path : "");
    return false;
}

/* Removes path and what it holds without following a link (a link loses only itself) or crossing
 * into another mounted volume. A folder MetalSharp cannot write into is made writable first. */
static bool remove_tree(const char* path, dev_t device) {
    struct stat st;
    DIR* dir;
    struct dirent* entry;
    bool ok = true;
    if (lstat(path, &st) != 0)
        return errno == ENOENT;
    if (!S_ISDIR(st.st_mode))
        return unlink(path) == 0 || errno == ENOENT;
    if (st.st_dev != device)
        return false;
    if (access(path, W_OK | X_OK | R_OK) != 0)
        (void)chmod(path, (st.st_mode & 07777) | S_IRWXU);
    if (!(dir = opendir(path)))
        return false;
    while ((entry = readdir(dir)) != NULL) {
        char child[PATH_MAX];
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        if ((size_t)snprintf(child, sizeof(child), "%s/%s", path, entry->d_name) >= sizeof(child) ||
            !remove_tree(child, device))
            ok = false;
    }
    closedir(dir);
    return ok && (rmdir(path) == 0 || errno == ENOENT);
}

/* Whether a relative path has an empty, "." or ".." component. */
static bool dotted(const char* relative) {
    const char* part = relative;
    while (part) {
        const char* end = strchr(part, '/');
        size_t length = end ? (size_t)(end - part) : strlen(part);
        if (!length || (length == 1 && part[0] == '.') || (length == 2 && part[0] == '.' && part[1] == '.'))
            return true;
        part = end ? end + 1 : NULL;
    }
    return false;
}

/* <home>/<relative>, when every folder on the way is a real folder (no link) inside the home.
 * *exists says whether the last component is there (it may itself be a link, which goes alone). */
static bool home_path(const uninstall_run* run, const char* relative, char* out, size_t out_size, bool* exists) {
    char parent[PATH_MAX], resolved[PATH_MAX];
    struct stat st;
    size_t base;
    *exists = false;
    if (!relative || !relative[0] || dotted(relative) ||
        (size_t)snprintf(out, out_size, "%s/%s", run->home, relative) >= out_size)
        return false;
    base = strlen(run->home);
    for (char* p = out + base + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = '\0';
        bool present = lstat(out, &st) == 0;
        bool link = present && !S_ISDIR(st.st_mode);
        *p = '/';
        if (!present)
            return true; /* nothing there to remove */
        if (link)
            return false;
    }
    snprintf(parent, sizeof(parent), "%s", out);
    *strrchr(parent, '/') = '\0';
    if (!realpath(parent, resolved) || !ms_resolved_path_within(resolved, run->home))
        return false;
    *exists = lstat(out, &st) == 0;
    return true;
}

static bool remove_relative(uninstall_run* run, const char* relative) {
    char path[PATH_MAX];
    struct stat home_info;
    bool exists;
    if (!home_path(run, relative, path, sizeof(path), &exists))
        return fail(run, "Refusing to remove a path reached through a link or outside the MetalSharp folder", path);
    if (!exists)
        return true;
    if (stat(run->home, &home_info) != 0 || !remove_tree(path, home_info.st_dev))
        return fail(run, "Could not remove", path);
    return note_removed(run, path) || fail(run, "Out of memory", NULL);
}

/* <home>/bottles/<prefix>* (per-game bottles of one launcher). */
static bool remove_bottles(uninstall_run* run, const char* name_prefix) {
    char bottles[PATH_MAX];
    bool exists;
    DIR* dir;
    struct dirent* entry;
    char** names = NULL;
    size_t count = 0, capacity = 0;
    bool ok = true;
    if (!home_path(run, "bottles", bottles, sizeof(bottles), &exists))
        return fail(run, "Refusing to remove a path reached through a link or outside the MetalSharp folder", bottles);
    if (!exists || !(dir = opendir(bottles)))
        return true;
    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, name_prefix, strlen(name_prefix)) || !entry->d_name[strlen(name_prefix)])
            continue;
        if (count == capacity) {
            size_t grown_capacity = capacity ? capacity * 2 : 16;
            char** grown = realloc(names, grown_capacity * sizeof(*grown));
            if (!grown) {
                ok = false;
                break;
            }
            names = grown;
            capacity = grown_capacity;
        }
        if (!(names[count] = strdup(entry->d_name))) {
            ok = false;
            break;
        }
        count++;
    }
    closedir(dir);
    if (!ok)
        fail(run, "Out of memory", NULL);
    for (size_t i = 0; ok && i < count; i++) {
        char relative[PATH_MAX];
        snprintf(relative, sizeof(relative), "bottles/%s", names[i]);
        ok = remove_relative(run, relative);
    }
    for (size_t i = 0; i < count; i++)
        free(names[i]);
    free(names);
    return ok;
}

/* The folder's contents except keep (a shared cache), then the folder when it is empty. */
static bool remove_contents_except(uninstall_run* run, const char* relative, const char* keep) {
    char path[PATH_MAX];
    bool exists, ok = true, kept = false;
    DIR* dir;
    struct dirent* entry;
    struct stat st;
    if (!home_path(run, relative, path, sizeof(path), &exists))
        return fail(run, "Refusing to remove a path reached through a link or outside the MetalSharp folder", path);
    if (!exists)
        return true;
    if (lstat(path, &st) != 0 || !S_ISDIR(st.st_mode))
        return remove_relative(run, relative);
    if (!(dir = opendir(path)))
        return fail(run, "Could not read", path);
    while (ok && (entry = readdir(dir)) != NULL) {
        char child[PATH_MAX];
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        if (!strcmp(entry->d_name, keep)) {
            kept = true;
            continue;
        }
        snprintf(child, sizeof(child), "%s/%s", relative, entry->d_name);
        ok = remove_relative(run, child);
    }
    closedir(dir);
    if (ok && !kept && rmdir(path) == 0)
        (void)note_removed(run, path);
    return ok;
}

/* An empty folder (rmdir leaves one with anything in it). */
static void remove_if_empty(uninstall_run* run, const char* relative) {
    char path[PATH_MAX];
    bool exists;
    if (home_path(run, relative, path, sizeof(path), &exists) && exists && rmdir(path) == 0)
        (void)note_removed(run, path);
}

/* ---- stopping ---- */

static bool json_flag(const char* raw, const char* key) {
    ms_json* json = raw ? ms_json_parse(raw, strlen(raw), NULL, 0) : NULL;
    bool value = false;
    if (json)
        (void)ms_json_as_bool(ms_json_object_get(json, key), &value);
    ms_json_free(json);
    return value;
}

static bool response_ok(const char* raw) {
    return json_flag(raw, "ok");
}

typedef struct {
    const char* prefix;
    const char* games;
} prefix_processes;

/* A Wine process of the prefix: its command or working folder is in it (or in its games folder). */
static bool match_prefix_process(pid_t pid, pid_t group, const char* command, void* context) {
    const prefix_processes* target = context;
    (void)group;
    return ms_store_contains_ci(command, target->prefix) || ms_store_process_cwd_within(pid, target->prefix) ||
           (target->games && target->games[0] && ms_store_process_cwd_within(pid, target->games));
}

/* Ubisoft Connect and Battle.net: their stop (wineserver -k on their own prefix), then up to 5 s for
 * the launcher and the games in that prefix to be gone. */
static bool stop_prefixed(const char* home, uninstall_run* run, const char* launcher,
                          char* (*status_json)(const char*), char* (*stop_json)(const char*, int*),
                          const char* prefix_name, const char* label) {
    char prefix[PATH_MAX];
    ms_library_location location;
    bool has_location = ms_library_location_get(home, launcher, &location, NULL);
    prefix_processes target = {prefix, has_location ? location.path : NULL};
    char* raw = status_json(home);
    int status = 500;
    bool installing = json_flag(raw, "installing");
    free(raw);
    if (installing) {
        snprintf(run->error, sizeof(run->error), "%s is installing. Wait for it to finish, then try again.", label);
        return false;
    }
    raw = stop_json(home, &status);
    free(raw);
    if (status != 200) {
        snprintf(run->error, sizeof(run->error),
                 "%s or one of its games is still running and could not be stopped. Quit it, then try again.",
                 label);
        return false;
    }
    snprintf(prefix, sizeof(prefix), "%s/%s", run->home, prefix_name);
    for (unsigned attempt = 0; attempt < 50; attempt++) {
        bool failed = false, running;
        size_t count = ms_store_wine_processes(home, match_prefix_process, &target, 0, NULL, &failed);
        raw = status_json(home);
        running = json_flag(raw, "running");
        free(raw);
        if (!failed && count == 0 && !running)
            return true;
        sleep_ms(100);
    }
    snprintf(run->error, sizeof(run->error),
             "%s or one of its games is still running and could not be stopped. Quit it, then try again.", label);
    return false;
}

/* Rockstar: its own stop, which ends the launcher, its helpers and the games it installed and never
 * Steam or a Steam copy of a game. */
static bool stop_rockstar(const char* home, uninstall_run* run) {
    char* raw = ms_rockstar_status_json(home);
    int status = 500;
    bool installing = json_flag(raw, "installing");
    free(raw);
    if (installing) {
        snprintf(run->error, sizeof(run->error),
                 "The Rockstar Games Launcher is installing. Wait for it to finish, then try again.");
        return false;
    }
    raw = ms_rockstar_stop_json(home, &status);
    free(raw);
    for (unsigned attempt = 0; status == 200 && attempt < 50; attempt++) {
        bool running;
        raw = ms_rockstar_status_json(home);
        running = json_flag(raw, "running");
        free(raw);
        if (!running)
            return true;
        sleep_ms(100);
    }
    snprintf(run->error, sizeof(run->error),
             "The Rockstar Games Launcher or one of its games is still running and could not be stopped. Quit it, "
             "then try again.");
    return false;
}

static bool stop_gog(const char* home, uninstall_run* run) {
    char* raw = ms_gog_action_json(home, "stop-all", NULL, 0);
    bool ok = response_ok(raw);
    free(raw);
    if (!ok)
        snprintf(run->error, sizeof(run->error),
                 "A GOG game is still running and could not be stopped. Quit it, then try again.");
    return ok;
}

/* An Epic download (epic/processes/<app>.pid) still running. */
static bool epic_download_running(const uninstall_run* run) {
    char path[PATH_MAX];
    DIR* dir;
    struct dirent* entry;
    bool running = false;
    snprintf(path, sizeof(path), "%s/epic/processes", run->home);
    if (!(dir = opendir(path)))
        return false;
    while (!running && (entry = readdir(dir)) != NULL) {
        size_t length = strlen(entry->d_name);
        char file[PATH_MAX];
        FILE* handle;
        long pid = 0;
        if (length <= 4 || strcmp(entry->d_name + length - 4, ".pid") ||
            (length > 11 && !strcmp(entry->d_name + length - 11, ".launch.pid")))
            continue;
        snprintf(file, sizeof(file), "%s/%s", path, entry->d_name);
        if ((handle = fopen(file, "rb")) != NULL) {
            if (fscanf(handle, "%ld", &pid) != 1)
                pid = 0;
            fclose(handle);
        }
        running = pid > 1 && pid <= INT_MAX && (kill((pid_t)pid, 0) == 0 || errno == EPERM);
    }
    closedir(dir);
    return running;
}

static bool epic_games_running(const char* home) {
    char* raw = ms_epic_running_json(home);
    ms_json* json = raw ? ms_json_parse(raw, strlen(raw), NULL, 0) : NULL;
    bool running = !json || ms_json_array_length(ms_json_object_get(json, "running")) > 0;
    ms_json_free(json);
    free(raw);
    return running;
}

static bool stop_epic(const char* home, uninstall_run* run) {
    if (epic_download_running(run)) {
        snprintf(run->error, sizeof(run->error),
                 "An Epic Games download is still running. Cancel it or wait for it to finish, then try again.");
        return false;
    }
    free(ms_epic_stop_all_json(home));
    for (unsigned attempt = 0; attempt < 50; attempt++) {
        if (!epic_games_running(home))
            return true;
        sleep_ms(100);
    }
    snprintf(run->error, sizeof(run->error),
             "An Epic Games game is still running and could not be stopped. Quit it, then try again.");
    return false;
}

/* ---- removing ---- */

static bool remove_ubisoft(uninstall_run* run) {
    /* cache/ubisoft-connect/artwork holds the Rockstar and Battle.net libraries' icons too. */
    return remove_relative(run, "prefix-ubisoft") && remove_contents_except(run, "cache/ubisoft-connect", "artwork") &&
           remove_bottles(run, "ubisoft_") && remove_relative(run, "config/game-executables/ubisoft");
}

static bool remove_battlenet(uninstall_run* run) {
    return remove_relative(run, "prefix-battlenet") && remove_relative(run, "cache/battlenet") &&
           remove_bottles(run, "battlenet_") && remove_relative(run, "config/game-executables/battlenet");
}

static bool remove_rockstar(const char* home, uninstall_run* run) {
    char drive[PATH_MAX];
    for (size_t i = 0; i < sizeof(ROCKSTAR_FOLDERS) / sizeof(ROCKSTAR_FOLDERS[0]); i++)
        if (!remove_relative(run, ROCKSTAR_FOLDERS[i]))
            return false;
    for (size_t i = 0; i < sizeof(ROCKSTAR_PARENTS) / sizeof(ROCKSTAR_PARENTS[0]); i++)
        remove_if_empty(run, ROCKSTAR_PARENTS[i]);
    if (!remove_relative(run, "cache/rockstar-launcher") || !remove_bottles(run, "rockstar_") ||
        !remove_relative(run, "config/game-executables/rockstar"))
        return false;
    /* R: (or the letter MetalSharp took instead), only while it links to the recorded folder. */
    if (ms_library_location_unmap_drive(home, "rockstar", drive, sizeof(drive)))
        (void)note_removed(run, drive);
    return true;
}

static bool remove_gog(const char* home, uninstall_run* run) {
    char path[PATH_MAX];
    bool exists;
    char* raw;
    if (!home_path(run, "bottles/gog-prefix", path, sizeof(path), &exists))
        return fail(run, "Refusing to remove a path reached through a link or outside the MetalSharp folder", path);
    if (exists) {
        /* GOG's own Remove Prefix (the sign-in stays, as it does there). */
        raw = ms_gog_action_json(home, "remove-prefix", NULL, 0);
        free(raw);
        if (lstat(path, &(struct stat){0}) == 0)
            return fail(run, "Could not remove", path);
        if (!note_removed(run, path))
            return fail(run, "Out of memory", NULL);
    }
    return remove_relative(run, "config/game-executables/gog");
}

static bool remove_epic(uninstall_run* run) {
    const char* tool_override = getenv("METALSHARP_EPIC_LEGENDARY_BIN");
    if (!remove_bottles(run, "epic_") || !remove_relative(run, "epic/legendary") ||
        !remove_relative(run, "epic/library.json") || !remove_relative(run, "epic/processes") ||
        !remove_relative(run, "config/game-executables/epic"))
        return false;
    /* The tool goes only when that is what status reads (an override elsewhere is not MetalSharp's). */
    return (tool_override && tool_override[0]) || remove_relative(run, "tools/legendary");
}

/* ---- the route ---- */

static char* failure(const char* message, const uninstall_run* run, bool with_path) {
    ms_json_writer writer;
    ms_json_writer_init(&writer);
    ms_json_writer_object_begin(&writer);
    ms_json_writer_key(&writer, "ok");
    ms_json_writer_bool(&writer, false);
    ms_json_writer_key(&writer, "error");
    ms_json_writer_string(&writer, message);
    if (with_path && run) {
        ms_json_writer_key(&writer, "path");
        if (run->failed_path[0])
            ms_json_writer_string(&writer, run->failed_path);
        else
            ms_json_writer_null(&writer);
        ms_json_writer_key(&writer, "removed");
        ms_json_writer_array_begin(&writer);
        for (size_t i = 0; i < run->count; i++)
            ms_json_writer_string(&writer, run->removed[i]);
        ms_json_writer_array_end(&writer);
    }
    ms_json_writer_object_end(&writer);
    return ms_json_writer_take(&writer);
}

char* ms_launcher_uninstall_json(const char* home, const char* body, size_t body_length, int* status) {
    ms_json* request = body ? ms_json_parse(body, body_length, NULL, 0) : NULL;
    const ms_json* delete_value = request ? ms_json_object_get(request, "delete_games") : NULL;
    const uninstall_launcher* launcher = NULL;
    uninstall_run run;
    char* id = NULL;
    char* result = NULL;
    char games_path[PATH_MAX] = "", check_error[1200] = "";
    ms_library_folder_check games = MS_LIBRARY_FOLDER_NONE;
    bool delete_games = false, ok;
    struct stat home_info;
    memset(&run, 0, sizeof(run));
    *status = 400;
    if (!request || ms_json_type_of(request) != MS_JSON_OBJECT ||
        !ms_json_as_string(ms_json_object_get(request, "launcher"), &id)) {
        result = failure("launcher must be ubisoft, battlenet, rockstar, gog or epic", NULL, false);
        goto done;
    }
    for (size_t i = 0; i < sizeof(LAUNCHERS) / sizeof(LAUNCHERS[0]) && !launcher; i++)
        if (!strcmp(LAUNCHERS[i].id, id))
            launcher = &LAUNCHERS[i];
    if (!launcher) {
        result = failure("launcher must be ubisoft, battlenet, rockstar, gog or epic", NULL, false);
        goto done;
    }
    if (!delete_value || ms_json_type_of(delete_value) != MS_JSON_BOOL ||
        !ms_json_as_bool(delete_value, &delete_games)) {
        result = failure("delete_games must be true or false", NULL, false);
        goto done;
    }
    *status = 500;
    if (!home || !realpath(home, run.home) || stat(run.home, &home_info) != 0 || !S_ISDIR(home_info.st_mode) ||
        !strcmp(run.home, "/")) {
        result = failure("The MetalSharp folder is not available", NULL, false);
        goto done;
    }

    /* The games folder is checked before anything is stopped or removed. */
    if (delete_games) {
        games = ms_library_location_check_deletable(home, launcher->id, games_path, sizeof(games_path), check_error,
                                                    sizeof(check_error));
        if (games == MS_LIBRARY_FOLDER_REFUSED) {
            char message[1400];
            *status = 409;
            snprintf(message, sizeof(message),
                     "The games folder can't be deleted (%s). Nothing was removed; uninstall again keeping the "
                     "games folder, or fix the folder first.",
                     check_error);
            result = failure(message, NULL, false);
            goto done;
        }
    }

    if (!strcmp(launcher->id, "ubisoft"))
        ok = stop_prefixed(home, &run, "ubisoft", ms_ubisoft_status_json, ms_ubisoft_stop_json, "prefix-ubisoft",
                           "Ubisoft Connect");
    else if (!strcmp(launcher->id, "battlenet"))
        ok = stop_prefixed(home, &run, "battlenet", ms_battlenet_status_json, ms_battlenet_stop_json,
                           "prefix-battlenet", "Battle.net");
    else if (!strcmp(launcher->id, "rockstar"))
        ok = stop_rockstar(home, &run);
    else if (!strcmp(launcher->id, "gog"))
        ok = stop_gog(home, &run);
    else
        ok = stop_epic(home, &run);
    if (!ok) {
        *status = 409;
        result = failure(run.error, NULL, false);
        goto done;
    }

    if (!strcmp(launcher->id, "ubisoft"))
        ok = remove_ubisoft(&run);
    else if (!strcmp(launcher->id, "battlenet"))
        ok = remove_battlenet(&run);
    else if (!strcmp(launcher->id, "rockstar"))
        ok = remove_rockstar(home, &run);
    else if (!strcmp(launcher->id, "gog"))
        ok = remove_gog(home, &run);
    else
        ok = remove_epic(&run);
    /* The prefix that held the drive letter is gone: forget the mapping (the location stays). */
    if (ok && (!strcmp(launcher->id, "ubisoft") || !strcmp(launcher->id, "battlenet")))
        (void)ms_library_location_unmap_drive(home, launcher->id, NULL, 0);
    if (ok && delete_games && games == MS_LIBRARY_FOLDER_DELETABLE) {
        struct stat folder_info;
        /* Checked again right before: nothing may have changed it into a link meanwhile. */
        games = ms_library_location_check_deletable(home, launcher->id, games_path, sizeof(games_path), check_error,
                                                    sizeof(check_error));
        if (games == MS_LIBRARY_FOLDER_REFUSED)
            ok = fail(&run, check_error, NULL);
        else if (games == MS_LIBRARY_FOLDER_DELETABLE &&
                 (lstat(games_path, &folder_info) != 0 || !remove_tree(games_path, folder_info.st_dev)))
            ok = fail(&run, "Could not delete the games folder", games_path);
        else if (games == MS_LIBRARY_FOLDER_DELETABLE && !note_removed(&run, games_path))
            ok = fail(&run, "Out of memory", NULL);
    }
    if (ok && delete_games && games != MS_LIBRARY_FOLDER_NONE && !ms_library_location_forget(home, launcher->id))
        ok = fail(&run, "Could not forget the saved games folder", NULL);
    if (!ok) {
        result = failure(run.error, &run, true);
        goto done;
    }

    {
        ms_library_location location;
        bool kept = !delete_games && ms_library_location_get(home, launcher->id, &location, NULL);
        ms_json_writer writer;
        ms_json_writer_init(&writer);
        ms_json_writer_object_begin(&writer);
        ms_json_writer_key(&writer, "ok");
        ms_json_writer_bool(&writer, true);
        ms_json_writer_key(&writer, "removed");
        ms_json_writer_array_begin(&writer);
        for (size_t i = 0; i < run.count; i++)
            ms_json_writer_string(&writer, run.removed[i]);
        ms_json_writer_array_end(&writer);
        ms_json_writer_key(&writer, "kept_games_folder");
        if (kept)
            ms_json_writer_string(&writer, location.path);
        else
            ms_json_writer_null(&writer);
        ms_json_writer_object_end(&writer);
        result = ms_json_writer_take(&writer);
        *status = 200;
    }
done:
    for (size_t i = 0; i < run.count; i++)
        free(run.removed[i]);
    free(run.removed);
    free(id);
    ms_json_free(request);
    return result;
}
