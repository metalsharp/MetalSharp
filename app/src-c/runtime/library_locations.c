#ifdef __APPLE__
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE 1
#endif
#endif
#include "metalsharp_backend/library_locations.h"
#include "metalsharp_backend/json.h"
#include "metalsharp_backend/json_writer.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define LIBRARY_LOCATIONS_FILE "config/library-locations.json"
#define LIBRARY_MAX_VOLUMES    64

typedef struct {
    const char* id;
    const char* label;  /* for messages */
    const char* folder; /* the managed folder's name */
    const char* prefix; /* the prefix the launcher installs games from, or NULL (gog, epic) */
    char letter;        /* its drive letter there */
    unsigned flags;     /* MS_LIBRARY_DRIVE_* for that letter */
} library_launcher;

static const library_launcher LAUNCHERS[] = {
    {"ubisoft", "Ubisoft Connect", "MetalSharp Ubisoft Games", "prefix-ubisoft", 'y',
     MS_LIBRARY_DRIVE_LEGACY_VOLUME_ROOT | MS_LIBRARY_DRIVE_REPLACE_BROKEN},
    {"battlenet", "Battle.net", "MetalSharp Battle.net Games", "prefix-battlenet", 'y',
     MS_LIBRARY_DRIVE_LEGACY_VOLUME_ROOT | MS_LIBRARY_DRIVE_REPLACE_BROKEN},
    /* prefix-steam: Steam libraries on external drives take letters from D: up, and a broken link
     * there can be a library on a drive that is not connected, so only MetalSharp's own links go. */
    {"rockstar", "Rockstar", "MetalSharp Rockstar Games", "prefix-steam", 'r', 0},
    {"gog", "GOG", "MetalSharp GOG Games", NULL, 0, 0},
    {"epic", "Epic", "MetalSharp Epic Games", NULL, 0, 0},
};
#define LAUNCHER_COUNT (sizeof(LAUNCHERS) / sizeof(LAUNCHERS[0]))

typedef struct {
    bool present[LAUNCHER_COUNT];
    ms_library_location entries[LAUNCHER_COUNT];
} library_store;

static const library_launcher* find_launcher(const char* id) {
    for (size_t i = 0; id && i < LAUNCHER_COUNT; i++)
        if (!strcmp(LAUNCHERS[i].id, id))
            return &LAUNCHERS[i];
    return NULL;
}

static char* join(const char* a, const char* b) {
    size_t x, y;
    bool slash;
    char* out;
    if (!a || !b)
        return NULL;
    x = strlen(a);
    y = strlen(b);
    slash = x > 0 && a[x - 1] != '/';
    out = malloc(x + y + 2);
    if (out)
        snprintf(out, x + y + 2, "%s%s%s", a, slash ? "/" : "", b);
    return out;
}

static bool is_dir(const char* path) {
    struct stat st;
    return path && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool mkdir_p(const char* path) {
    char* copy;
    bool ok;
    if (!path || !path[0] || !(copy = strdup(path)))
        return false;
    for (size_t i = 1; copy[i]; i++)
        if (copy[i] == '/') {
            copy[i] = '\0';
            if (mkdir(copy, 0755) != 0 && errno != EEXIST) {
                free(copy);
                return false;
            }
            copy[i] = '/';
        }
    ok = mkdir(copy, 0755) == 0 || errno == EEXIST;
    free(copy);
    return ok && is_dir(path);
}

static uint64_t free_bytes_of(const struct statfs* fs) {
    uint64_t block = fs->f_bsize > 0 ? (uint64_t)fs->f_bsize : 0;
    uint64_t blocks = (uint64_t)fs->f_bavail;
    return block && blocks > UINT64_MAX / block ? UINT64_MAX : blocks * block;
}

static const char* user_home(void) {
    const char* home = getenv("HOME");
    if (home && home[0] == '/')
        return home;
    struct passwd* entry = getpwuid(getuid());
    return entry && entry->pw_dir ? entry->pw_dir : NULL;
}

const char* ms_library_volumes_root(void) {
    const char* root = getenv(MS_LIBRARY_VOLUMES_ROOT_ENV);
    return root && root[0] == '/' ? root : "/Volumes";
}

char* ms_library_internal_root(void) {
    return join(user_home(), "Games");
}

const char* ms_library_folder_name(const char* launcher) {
    const library_launcher* entry = find_launcher(launcher);
    return entry ? entry->folder : NULL;
}

bool ms_library_volume_eligible(const char* name, const ms_library_volume_facts* facts) {
    static const char* const SYSTEM_NAMES[] = {"Recovery", "Preboot",  "VM",         "Update",
                                               "xarts",    "Hardware", "iSCPreboot", "Macintosh HD"};
    static const char* const NETWORK_TYPES[] = {"smbfs", "afpfs", "nfs", "webdav", "ftp", "cifs", "cddafs"};
    if (!name || !name[0] || name[0] == '.' || !facts)
        return false;
    for (size_t i = 0; i < sizeof(SYSTEM_NAMES) / sizeof(SYSTEM_NAMES[0]); i++)
        if (!strcasecmp(name, SYSTEM_NAMES[i]))
            return false;
    for (size_t i = 0; facts->fs_type && i < sizeof(NETWORK_TYPES) / sizeof(NETWORK_TYPES[0]); i++)
        if (!strcasecmp(facts->fs_type, NETWORK_TYPES[i]))
            return false;
    return !facts->symlink && facts->directory && !facts->boot_device && facts->mount_root && !facts->read_only &&
           !facts->hidden && facts->local && facts->writable && !facts->time_machine && !facts->metalsharp_image;
}

static bool contains_ci(const char* text, const char* needle) {
    size_t n = strlen(needle);
    for (; text && *text; text++)
        if (!strncasecmp(text, needle, n))
            return true;
    return false;
}

/* The facts about root/name. The fake root of the tests holds plain folders: each counts as a
 * writable mount of a local volume other than the boot volume (its permissions still count). */
static bool volume_facts(const char* path, const char* name, bool fake, ms_library_volume_facts* facts,
                         struct statfs* fs) {
    struct stat link_info, info, boot;
    char* probe;
    memset(facts, 0, sizeof(*facts));
    if (lstat(path, &link_info) != 0)
        return false;
    facts->symlink = S_ISLNK(link_info.st_mode);
    facts->directory = stat(path, &info) == 0 && S_ISDIR(info.st_mode);
    if (!facts->directory || statfs(path, fs) != 0)
        return true;
    facts->fs_type = fs->f_fstypename;
    facts->read_only = (fs->f_flags & MNT_RDONLY) != 0;
#ifdef MNT_DONTBROWSE
    facts->hidden = (fs->f_flags & MNT_DONTBROWSE) != 0;
#endif
    if (fake) {
        /* statfs describes the volume holding the test's folder (the data volume is nobrowse). */
        facts->mount_root = true;
        facts->local = true;
        facts->read_only = false;
        facts->hidden = false;
    } else {
        const char* home = user_home();
        facts->mount_root = !strcmp(fs->f_mntonname, path);
        facts->local = (fs->f_flags & MNT_LOCAL) != 0;
        facts->boot_device = (stat("/", &boot) == 0 && boot.st_dev == info.st_dev) ||
                             (home && stat(home, &boot) == 0 && boot.st_dev == info.st_dev);
    }
    facts->writable = access(path, W_OK | X_OK) == 0;
    probe = join(path, "Backups.backupdb");
    facts->time_machine = is_dir(probe) || contains_ci(name, "Time Machine") || !strncasecmp(name, "Backups of ", 11);
    free(probe);
    probe = join(path, "MetalSharp.app");
    facts->metalsharp_image = is_dir(probe);
    free(probe);
    return true;
}

size_t ms_library_volumes(ms_library_volume* volumes, size_t max) {
    const char* root = ms_library_volumes_root();
    bool fake = strcmp(root, "/Volumes") != 0;
    size_t count = 0;
    DIR* directory;
    struct dirent* entry;
    if (!volumes || !max)
        return 0;
    {
        char* internal = ms_library_internal_root();
        const char* home = user_home();
        struct statfs fs;
        memset(&volumes[0], 0, sizeof(volumes[0]));
        snprintf(volumes[0].id, sizeof(volumes[0].id), "internal");
        snprintf(volumes[0].name, sizeof(volumes[0].name), "This Mac");
        snprintf(volumes[0].root, sizeof(volumes[0].root), "%s", internal ? internal : "");
        volumes[0].free_bytes = home && statfs(home, &fs) == 0 ? free_bytes_of(&fs) : 0;
        free(internal);
        count = 1;
    }
    directory = opendir(root);
    if (!directory)
        return count;
    while (count < max && (entry = readdir(directory)) != NULL) {
        ms_library_volume_facts facts;
        struct statfs fs;
        char* path;
        if (entry->d_name[0] == '.')
            continue;
        path = join(root, entry->d_name);
        if (path && strlen(path) < sizeof(volumes[count].id) && volume_facts(path, entry->d_name, fake, &facts, &fs) &&
            ms_library_volume_eligible(entry->d_name, &facts)) {
            ms_library_volume* volume = &volumes[count++];
            memset(volume, 0, sizeof(*volume));
            snprintf(volume->id, sizeof(volume->id), "%s", path);
            snprintf(volume->name, sizeof(volume->name), "%s", entry->d_name);
            snprintf(volume->root, sizeof(volume->root), "%s", path);
            volume->external = true;
            volume->free_bytes = free_bytes_of(&fs);
        }
        free(path);
    }
    closedir(directory);
    /* A stable order: by name. */
    for (size_t i = 2; i < count; i++)
        for (size_t j = i; j > 1 && strcasecmp(volumes[j - 1].name, volumes[j].name) > 0; j--) {
            ms_library_volume swap = volumes[j];
            volumes[j] = volumes[j - 1];
            volumes[j - 1] = swap;
        }
    return count;
}

char* ms_library_best_external_volume(void) {
    ms_library_volume* volumes = calloc(LIBRARY_MAX_VOLUMES, sizeof(*volumes));
    size_t count = volumes ? ms_library_volumes(volumes, LIBRARY_MAX_VOLUMES) : 0;
    const ms_library_volume* best = NULL;
    char* result = NULL;
    for (size_t i = 0; i < count; i++)
        if (volumes[i].external && (!best || volumes[i].free_bytes > best->free_bytes))
            best = &volumes[i];
    if (best) {
        char resolved[PATH_MAX];
        result = strdup(realpath(best->root, resolved) ? resolved : best->root);
    }
    free(volumes);
    return result;
}

static bool find_volume(const char* id, ms_library_volume* out) {
    ms_library_volume* volumes = calloc(LIBRARY_MAX_VOLUMES, sizeof(*volumes));
    size_t count = volumes ? ms_library_volumes(volumes, LIBRARY_MAX_VOLUMES) : 0;
    bool found = false;
    for (size_t i = 0; id && i < count && !found; i++)
        if (!strcmp(volumes[i].id, id)) {
            *out = volumes[i];
            found = true;
        }
    free(volumes);
    return found;
}

static const char* basename_of(const char* path) {
    const char* slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

bool ms_library_is_managed_root(const char* path) {
    char resolved[PATH_MAX], candidate[PATH_MAX];
    char* internal;
    bool managed = false;
    if (!path || !realpath(path, resolved))
        return false;
    for (size_t i = 0; i < LAUNCHER_COUNT; i++)
        managed |= !strcmp(basename_of(resolved), LAUNCHERS[i].folder);
    internal = ms_library_internal_root();
    managed |= internal && realpath(internal, candidate) && !strcmp(candidate, resolved);
    free(internal);
    return managed;
}

/* ---- the saved choices ---- */

static char* store_path(const char* home) {
    return join(home, LIBRARY_LOCATIONS_FILE);
}

static void copy_field(const ms_json* object, const char* key, char* out, size_t size) {
    char* text = NULL;
    if (ms_json_as_string(ms_json_object_get(object, key), &text))
        snprintf(out, size, "%s", text);
    free(text);
}

static void store_load(const char* home, library_store* store) {
    char* path = store_path(home);
    FILE* file = path ? fopen(path, "rb") : NULL;
    char* text = NULL;
    long size;
    memset(store, 0, sizeof(*store));
    if (file && fseek(file, 0, SEEK_END) == 0 && (size = ftell(file)) > 0 && size < (1 << 20) &&
        fseek(file, 0, SEEK_SET) == 0 && (text = malloc((size_t)size + 1)) != NULL) {
        size_t read = fread(text, 1, (size_t)size, file);
        text[read] = '\0';
        ms_json* root = ms_json_parse(text, read, NULL, 0);
        const ms_json* locations = root ? ms_json_object_get(root, "locations") : NULL;
        for (size_t i = 0; locations && i < LAUNCHER_COUNT; i++) {
            const ms_json* entry = ms_json_object_get(locations, LAUNCHERS[i].id);
            ms_library_location* location = &store->entries[i];
            if (!entry || ms_json_type_of(entry) != MS_JSON_OBJECT)
                continue;
            copy_field(entry, "volume", location->volume, sizeof(location->volume));
            copy_field(entry, "path", location->path, sizeof(location->path));
            copy_field(entry, "drive", location->drive, sizeof(location->drive));
            copy_field(entry, "drive_target", location->drive_target, sizeof(location->drive_target));
            store->present[i] = location->volume[0] && location->path[0] == '/';
        }
        ms_json_free(root);
    }
    if (file)
        fclose(file);
    free(text);
    free(path);
}

static bool store_save(const char* home, const library_store* store) {
    char *path = store_path(home), *dir = join(home, "config"), *temp = NULL, *text;
    ms_json_writer writer;
    FILE* file;
    bool ok = false;
    ms_json_writer_init(&writer);
    ms_json_writer_object_begin(&writer);
    ms_json_writer_key(&writer, "version");
    ms_json_writer_u64(&writer, 1);
    ms_json_writer_key(&writer, "locations");
    ms_json_writer_object_begin(&writer);
    for (size_t i = 0; i < LAUNCHER_COUNT; i++) {
        const ms_library_location* location = &store->entries[i];
        if (!store->present[i])
            continue;
        ms_json_writer_key(&writer, LAUNCHERS[i].id);
        ms_json_writer_object_begin(&writer);
        ms_json_writer_key(&writer, "volume");
        ms_json_writer_string(&writer, location->volume);
        ms_json_writer_key(&writer, "path");
        ms_json_writer_string(&writer, location->path);
        if (location->drive[0]) {
            ms_json_writer_key(&writer, "drive");
            ms_json_writer_string(&writer, location->drive);
            ms_json_writer_key(&writer, "drive_target");
            ms_json_writer_string(&writer, location->drive_target);
        }
        ms_json_writer_object_end(&writer);
    }
    ms_json_writer_object_end(&writer);
    ms_json_writer_object_end(&writer);
    text = ms_json_writer_take(&writer);
    if (text && path && dir && mkdir_p(dir)) {
        size_t length = strlen(path) + 16;
        temp = malloc(length);
        if (temp) {
            snprintf(temp, length, "%s.tmp.%ld", path, (long)getpid());
            file = fopen(temp, "wb");
            if (file) {
                ok = fputs(text, file) >= 0 && fputc('\n', file) != EOF;
                ok = fflush(file) == 0 && fsync(fileno(file)) == 0 && ok;
                ok = fclose(file) == 0 && ok;
                ok = ok && rename(temp, path) == 0;
                if (!ok)
                    (void)unlink(temp);
            }
        }
    }
    free(text);
    free(temp);
    free(dir);
    free(path);
    return ok;
}

static bool location_available(const ms_library_location* location) {
    ms_library_volume volume;
    if (!strcmp(location->volume, "internal"))
        return true;
    return find_volume(location->volume, &volume);
}

bool ms_library_location_get(const char* home, const char* launcher, ms_library_location* location,
                             bool* available) {
    const library_launcher* entry = find_launcher(launcher);
    library_store store;
    size_t index;
    if (!entry || !home)
        return false;
    index = (size_t)(entry - LAUNCHERS);
    store_load(home, &store);
    if (!store.present[index])
        return false;
    if (location)
        *location = store.entries[index];
    if (available)
        *available = location_available(&store.entries[index]);
    return true;
}

/* ---- drive letters ---- */

/* A link to a mount point right under the volumes root: /Volumes/<Name>. */
static bool volume_root_link(const char* target) {
    const char* root = ms_library_volumes_root();
    size_t n = strlen(root);
    char resolved_root[PATH_MAX];
    if (!strncmp(target, root, n) && target[n] == '/' && target[n + 1] && !strchr(target + n + 1, '/'))
        return true;
    /* tests' fake roots live under /var -> /private/var */
    if (realpath(root, resolved_root)) {
        n = strlen(resolved_root);
        if (!strncmp(target, resolved_root, n) && target[n] == '/' && target[n + 1] && !strchr(target + n + 1, '/'))
            return true;
    }
    return !strncmp(target, "/Volumes/", 9) && target[9] && !strchr(target + 9, '/');
}

static bool same_path(const char* a, const char* b) {
    char x[PATH_MAX], y[PATH_MAX];
    if (!a || !b || !a[0] || !b[0])
        return false;
    if (!strcmp(a, b))
        return true;
    return realpath(a, x) && realpath(b, y) && !strcmp(x, y);
}

static bool managed_folder_link(const char* target) {
    char copy[PATH_MAX];
    size_t n;
    snprintf(copy, sizeof(copy), "%s", target);
    n = strlen(copy);
    while (n > 1 && copy[n - 1] == '/')
        copy[--n] = '\0';
    for (size_t i = 0; i < LAUNCHER_COUNT; i++)
        if (!strcmp(basename_of(copy), LAUNCHERS[i].folder))
            return true;
    return false;
}

ms_library_drive_result ms_library_map_drive(const char* prefix, char letter, const char* target,
                                             const char* previous_target, unsigned flags) {
    char name[4], existing[PATH_MAX], resolved[PATH_MAX];
    char *dosdevices = join(prefix, "dosdevices"), *drive = NULL;
    ms_library_drive_result result = MS_LIBRARY_DRIVE_FAILED;
    struct stat st;
    ssize_t length;
    letter = (char)tolower((unsigned char)letter);
    if (!dosdevices || !target || letter < 'd' || letter > 'y' || !mkdir_p(dosdevices))
        goto done;
    snprintf(name, sizeof(name), "%c:", letter);
    drive = join(dosdevices, name);
    if (!drive)
        goto done;
    if (lstat(drive, &st) != 0) {
        if (errno == ENOENT && symlink(target, drive) == 0)
            result = MS_LIBRARY_DRIVE_CREATED;
        goto done;
    }
    if (!S_ISLNK(st.st_mode) || (length = readlink(drive, existing, sizeof(existing) - 1)) <= 0) {
        result = MS_LIBRARY_DRIVE_USER;
        goto done;
    }
    existing[length] = '\0';
    if (same_path(existing, target)) {
        result = MS_LIBRARY_DRIVE_KEPT;
        goto done;
    }
    {
        bool broken = !realpath(drive, resolved);
        bool ours = (previous_target && previous_target[0] && !strcmp(existing, previous_target)) ||
                    managed_folder_link(existing) ||
                    ((flags & MS_LIBRARY_DRIVE_LEGACY_VOLUME_ROOT) && volume_root_link(existing)) ||
                    ((flags & MS_LIBRARY_DRIVE_REPLACE_BROKEN) && broken);
        if (!ours) {
            result = MS_LIBRARY_DRIVE_USER;
            goto done;
        }
    }
    if (unlink(drive) == 0 && symlink(target, drive) == 0)
        result = MS_LIBRARY_DRIVE_REPLACED;
done:
    free(dosdevices);
    free(drive);
    return result;
}

static bool drive_is_link_to(const char* prefix, const char* drive, const char* target) {
    char *dosdevices = join(prefix, "dosdevices"), *path = dosdevices ? join(dosdevices, drive) : NULL;
    char existing[PATH_MAX];
    ssize_t length = path ? readlink(path, existing, sizeof(existing) - 1) : -1;
    bool match = false;
    if (length > 0) {
        existing[length] = '\0';
        match = !strcmp(existing, target);
    }
    free(path);
    free(dosdevices);
    return match;
}

static void unmap_drive(const char* prefix, const char* drive) {
    char *dosdevices = join(prefix, "dosdevices"), *path = dosdevices ? join(dosdevices, drive) : NULL;
    if (path)
        (void)unlink(path);
    free(path);
    free(dosdevices);
}

/* Maps location's folder in launcher's prefix, updating location->drive and drive_target. */
static void apply_drive(const char* home, const library_launcher* launcher, ms_library_location* location) {
    char* prefix = launcher->prefix ? join(home, launcher->prefix) : NULL;
    char chosen = 0;
    char old_drive[4];
    char old_target[sizeof(location->drive_target)];
    if (!prefix || !is_dir(prefix)) {
        free(prefix);
        return;
    }
    snprintf(old_drive, sizeof(old_drive), "%s", location->drive);
    snprintf(old_target, sizeof(old_target), "%s", location->drive_target);
    ms_library_drive_result result =
        ms_library_map_drive(prefix, launcher->letter, location->path, old_target, launcher->flags);
    if (result != MS_LIBRARY_DRIVE_FAILED && result != MS_LIBRARY_DRIVE_USER)
        chosen = launcher->letter;
    /* The preferred letter is the user's: the first free letter, X: down to M:, or the one recorded. */
    for (char letter = 'x'; !chosen && result == MS_LIBRARY_DRIVE_USER && letter >= 'm'; letter--) {
        char name[4];
        char* path;
        struct stat st;
        bool usable;
        if (letter == launcher->letter)
            continue;
        snprintf(name, sizeof(name), "%c:", letter);
        path = join(prefix, "dosdevices");
        {
            char* drive = path ? join(path, name) : NULL;
            usable = drive && (lstat(drive, &st) != 0 || (old_drive[0] && !strcmp(old_drive, name)));
            free(drive);
        }
        free(path);
        if (usable) {
            ms_library_drive_result fallback = ms_library_map_drive(prefix, letter, location->path, old_target, 0);
            if (fallback != MS_LIBRARY_DRIVE_FAILED && fallback != MS_LIBRARY_DRIVE_USER)
                chosen = letter;
        }
    }
    if (chosen) {
        snprintf(location->drive, sizeof(location->drive), "%c:", chosen);
        snprintf(location->drive_target, sizeof(location->drive_target), "%s", location->path);
        /* A letter MetalSharp mapped before, now unused: take it back. */
        if (old_drive[0] && strcmp(old_drive, location->drive) && old_target[0] &&
            drive_is_link_to(prefix, old_drive, old_target))
            unmap_drive(prefix, old_drive);
    } else if (old_drive[0] && !drive_is_link_to(prefix, old_drive, location->path)) {
        location->drive[0] = '\0';
        location->drive_target[0] = '\0';
    }
    free(prefix);
}

/* The launcher folder on an existing root: on an external volume only the folder itself is made, so
 * nothing is ever created under /Volumes for a drive that is not there. */
static bool create_folder(const ms_library_location* location) {
    char root[1024];
    char* slash;
    snprintf(root, sizeof(root), "%s", location->path);
    slash = strrchr(root, '/');
    if (!slash || slash == root)
        return false;
    *slash = '\0';
    if (!strcmp(location->volume, "internal")) {
        if (!mkdir_p(root))
            return false;
    } else if (!is_dir(root))
        return false;
    if (mkdir(location->path, 0755) != 0 && errno != EEXIST)
        return false;
    return is_dir(location->path) && access(location->path, W_OK | X_OK) == 0;
}

static bool apply_launcher(const char* home, const library_launcher* launcher, library_store* store) {
    size_t index = (size_t)(launcher - LAUNCHERS);
    ms_library_location before;
    ms_library_location* location = &store->entries[index];
    if (!store->present[index] || !location_available(location) || !create_folder(location))
        return false;
    before = *location;
    if (launcher->prefix)
        apply_drive(home, launcher, location);
    if (memcmp(&before, location, sizeof(before)))
        (void)store_save(home, store);
    return true;
}

bool ms_library_location_apply(const char* home, const char* launcher) {
    const library_launcher* entry = find_launcher(launcher);
    library_store store;
    if (!entry || !home)
        return false;
    store_load(home, &store);
    if (!store.present[entry - LAUNCHERS])
        return false;
    (void)apply_launcher(home, entry, &store);
    return true;
}

/* ---- routes ---- */

static char* failure_json(const char* message, bool needs_location) {
    ms_json_writer writer;
    ms_json_writer_init(&writer);
    ms_json_writer_object_begin(&writer);
    ms_json_writer_key(&writer, "ok");
    ms_json_writer_bool(&writer, false);
    ms_json_writer_key(&writer, "error");
    ms_json_writer_string(&writer, message);
    if (needs_location) {
        ms_json_writer_key(&writer, "needs_location");
        ms_json_writer_bool(&writer, true);
    }
    ms_json_writer_object_end(&writer);
    return ms_json_writer_take(&writer);
}

static void write_drive(ms_json_writer* writer, const ms_library_location* location) {
    char drive[3];
    ms_json_writer_key(writer, "drive");
    if (location->drive[0]) {
        snprintf(drive, sizeof(drive), "%c:", toupper((unsigned char)location->drive[0]));
        ms_json_writer_string(writer, drive);
    } else
        ms_json_writer_null(writer);
}

char* ms_library_locations_json(const char* home) {
    ms_library_volume* volumes = calloc(LIBRARY_MAX_VOLUMES, sizeof(*volumes));
    size_t count = volumes ? ms_library_volumes(volumes, LIBRARY_MAX_VOLUMES) : 0;
    library_store store;
    ms_json_writer writer;
    if (!volumes)
        return NULL;
    store_load(home, &store);
    ms_json_writer_init(&writer);
    ms_json_writer_object_begin(&writer);
    ms_json_writer_key(&writer, "ok");
    ms_json_writer_bool(&writer, true);
    ms_json_writer_key(&writer, "volumes");
    ms_json_writer_array_begin(&writer);
    for (size_t i = 0; i < count; i++) {
        ms_json_writer_object_begin(&writer);
        ms_json_writer_key(&writer, "id");
        ms_json_writer_string(&writer, volumes[i].id);
        ms_json_writer_key(&writer, "name");
        ms_json_writer_string(&writer, volumes[i].name);
        ms_json_writer_key(&writer, "root");
        ms_json_writer_string(&writer, volumes[i].root);
        ms_json_writer_key(&writer, "external");
        ms_json_writer_bool(&writer, volumes[i].external);
        ms_json_writer_key(&writer, "free_bytes");
        ms_json_writer_u64(&writer, volumes[i].free_bytes);
        ms_json_writer_object_end(&writer);
    }
    ms_json_writer_array_end(&writer);
    ms_json_writer_key(&writer, "locations");
    ms_json_writer_object_begin(&writer);
    for (size_t i = 0; i < LAUNCHER_COUNT; i++) {
        const ms_library_location* location = &store.entries[i];
        bool available = false;
        ms_json_writer_key(&writer, LAUNCHERS[i].id);
        if (!store.present[i]) {
            ms_json_writer_null(&writer);
            continue;
        }
        if (!strcmp(location->volume, "internal"))
            available = true;
        for (size_t v = 1; v < count && !available; v++)
            available = !strcmp(volumes[v].id, location->volume);
        ms_json_writer_object_begin(&writer);
        ms_json_writer_key(&writer, "volume");
        ms_json_writer_string(&writer, location->volume);
        ms_json_writer_key(&writer, "path");
        ms_json_writer_string(&writer, location->path);
        ms_json_writer_key(&writer, "available");
        ms_json_writer_bool(&writer, available);
        if (LAUNCHERS[i].prefix)
            write_drive(&writer, location);
        ms_json_writer_object_end(&writer);
    }
    ms_json_writer_object_end(&writer);
    ms_json_writer_object_end(&writer);
    free(volumes);
    return ms_json_writer_take(&writer);
}

char* ms_library_locations_set_json(const char* home, const char* body, size_t body_length, int* status) {
    ms_json* request = body ? ms_json_parse(body, body_length, NULL, 0) : NULL;
    char *launcher_id = NULL, *volume_id = NULL, *result = NULL;
    const library_launcher* launcher;
    ms_library_volume volume;
    library_store store;
    ms_library_location* location;
    size_t index;
    *status = 400;
    if (!request || ms_json_type_of(request) != MS_JSON_OBJECT ||
        !ms_json_as_string(ms_json_object_get(request, "launcher"), &launcher_id) ||
        !(launcher = find_launcher(launcher_id))) {
        result = failure_json("launcher must be ubisoft, battlenet, rockstar, gog or epic", false);
        goto done;
    }
    if (!ms_json_as_string(ms_json_object_get(request, "volume"), &volume_id) || !find_volume(volume_id, &volume) ||
        !volume.root[0]) {
        result = failure_json("That drive isn't available. Choose This Mac or a connected drive.", false);
        goto done;
    }
    index = (size_t)(launcher - LAUNCHERS);
    store_load(home, &store);
    location = &store.entries[index];
    /* A change of drive keeps drive and drive_target: they say which letter MetalSharp may take back. */
    store.present[index] = true;
    snprintf(location->volume, sizeof(location->volume), "%s", volume.id);
    if ((size_t)snprintf(location->path, sizeof(location->path), "%s/%s", volume.root, launcher->folder) >=
        sizeof(location->path)) {
        result = failure_json("The folder path is too long", false);
        goto done;
    }
    if (!create_folder(location)) {
        char message[1200];
        *status = 409;
        snprintf(message, sizeof(message), "Could not create %s", location->path);
        result = failure_json(message, false);
        goto done;
    }
    if (!store_save(home, &store)) {
        *status = 500;
        result = failure_json("Could not save where games go", false);
        goto done;
    }
    (void)apply_launcher(home, launcher, &store);
    {
        ms_json_writer writer;
        ms_json_writer_init(&writer);
        ms_json_writer_object_begin(&writer);
        ms_json_writer_key(&writer, "ok");
        ms_json_writer_bool(&writer, true);
        ms_json_writer_key(&writer, "path");
        ms_json_writer_string(&writer, location->path);
        if (launcher->prefix)
            write_drive(&writer, location);
        ms_json_writer_object_end(&writer);
        result = ms_json_writer_take(&writer);
        *status = 200;
    }
done:
    ms_json_free(request);
    free(launcher_id);
    free(volume_id);
    return result;
}

char* ms_library_install_request(const char* home, const char* launcher, const char* body, size_t body_length,
                                 size_t* out_length, int* status, char** failure) {
    const library_launcher* entry = find_launcher(launcher);
    ms_json* request = body ? ms_json_parse(body, body_length, NULL, 0) : NULL;
    char* given = NULL;
    char* result = NULL;
    ms_library_location location;
    bool available = false;
    *failure = NULL;
    *status = 200;
    /* Not an object, or with an installPath: the handler takes it as before. */
    if (!entry || !request || ms_json_type_of(request) != MS_JSON_OBJECT ||
        (ms_json_as_string(ms_json_object_get(request, "installPath"), &given) && given[0])) {
        result = malloc(body_length + 1);
        if (result) {
            if (body_length)
                memcpy(result, body, body_length);
            result[body_length] = '\0';
            *out_length = body_length;
        }
        goto done;
    }
    *status = 409;
    if (!ms_library_location_get(home, launcher, &location, &available)) {
        char message[128];
        snprintf(message, sizeof(message), "Choose where %s games go first", entry->label);
        *failure = failure_json(message, true);
        goto done;
    }
    if (!available || !create_folder(&location)) {
        char message[1400];
        if (!available)
            snprintf(message, sizeof(message), "The drive for %s games (%s) isn't connected. Connect it or choose where %s games go",
                     entry->label, basename_of(location.volume), entry->label);
        else
            snprintf(message, sizeof(message), "Could not create %s", location.path);
        *failure = failure_json(message, true);
        goto done;
    }
    {
        ms_json_writer writer;
        ms_json_writer_init(&writer);
        ms_json_writer_object_begin(&writer);
        for (size_t i = 0; i < ms_json_object_length(request); i++) {
            const char* key = ms_json_object_key_at(request, i);
            char* value;
            if (!key || !strcmp(key, "installPath"))
                continue;
            value = ms_json_stringify(ms_json_object_value_at(request, i));
            if (!value)
                continue;
            ms_json_writer_key(&writer, key);
            ms_json_writer_raw(&writer, value);
            free(value);
        }
        ms_json_writer_key(&writer, "installPath");
        ms_json_writer_string(&writer, location.path);
        ms_json_writer_object_end(&writer);
        result = ms_json_writer_take(&writer);
        if (result) {
            *out_length = strlen(result);
            *status = 200;
        }
    }
done:
    free(given);
    ms_json_free(request);
    return result;
}
