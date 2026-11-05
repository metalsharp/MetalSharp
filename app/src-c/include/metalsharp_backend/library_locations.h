#ifndef METALSHARP_BACKEND_LIBRARY_LOCATIONS_H
#define METALSHARP_BACKEND_LIBRARY_LOCATIONS_H

/* Where the game launchers MetalSharp manages put their games: one MetalSharp-named folder per
 * launcher ("MetalSharp GOG Games", ...) on the drive the user picks, this Mac (<user home>/Games)
 * or a mounted external volume (/Volumes/<Name>). The choice is kept in
 * <MetalSharp home>/config/library-locations.json.
 *
 * GOG and Epic install into the folder directly (a request's installPath defaults to it). Ubisoft
 * Connect, Battle.net and the Rockstar Games Launcher install games themselves, so the folder becomes
 * a drive letter in their prefix: Ubisoft Y: (prefix-ubisoft), Battle.net Y: (prefix-battlenet),
 * Rockstar R: (prefix-steam).
 *
 * Drive letter rule: a letter is MetalSharp's to replace when it is missing, a link to the folder
 * MetalSharp recorded for it last time, or a link to any "MetalSharp <X> Games" folder. In
 * prefix-ubisoft and prefix-battlenet, Y: as a link to a volume root (/Volumes/<Name>, the
 * automatic mapping MetalSharp made before library locations) or a broken link is MetalSharp's too.
 * Anything else (a folder, or a working link to another place) is the user's and is kept; the
 * folder then gets the first free letter from X: down to M:. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MS_LIBRARY_VOLUMES_ROOT_ENV "METALSHARP_VOLUMES_ROOT" /* tests: a folder standing in for /Volumes */

typedef struct {
    char id[1024];   /* "internal", or the volume's mount point */
    char name[256];  /* "This Mac", or the volume's name */
    char root[1024]; /* where the launcher folders go */
    bool external;
    uint64_t free_bytes;
} ms_library_volume;

/* What decides whether a /Volumes entry can hold games. */
typedef struct {
    bool symlink;          /* the boot volume's alias in /Volumes */
    bool directory;
    bool boot_device;      /* on the boot volume (or the user's data volume) */
    bool mount_root;       /* the entry is itself a mount point */
    bool read_only;        /* disk images, read-only media */
    bool hidden;           /* MNT_DONTBROWSE: system volumes (Preboot, VM, Update, ...) */
    bool local;            /* not a network share */
    bool writable;
    bool time_machine;     /* a Time Machine backup disk */
    bool metalsharp_image; /* the MetalSharp installer DMG */
    const char* fs_type;   /* statfs f_fstypename, or NULL */
} ms_library_volume_facts;

bool ms_library_volume_eligible(const char* name, const ms_library_volume_facts* facts);

/* /Volumes, or $METALSHARP_VOLUMES_ROOT. */
const char* ms_library_volumes_root(void);
/* <user home>/Games (malloc'd). */
char* ms_library_internal_root(void);
/* This Mac first, then the eligible mounted volumes; returns the count (at most max). */
size_t ms_library_volumes(ms_library_volume* volumes, size_t max);
/* The eligible external volume with the most free space (malloc'd), or NULL. */
char* ms_library_best_external_volume(void);

/* "MetalSharp GOG Games" for "gog"; NULL for an unknown launcher. */
const char* ms_library_folder_name(const char* launcher);
/* Whether path (resolved) is <user home>/Games or a launcher folder: never something to delete. */
bool ms_library_is_managed_root(const char* path);

typedef struct {
    char volume[1024];
    char path[1024];
    char drive[4];           /* "y:" once mapped, "" otherwise */
    char drive_target[1024]; /* what the drive was linked to */
} ms_library_location;

/* The saved location of launcher; *available when its volume is mounted. */
bool ms_library_location_get(const char* home, const char* launcher, ms_library_location* location,
                             bool* available);

typedef enum {
    MS_LIBRARY_DRIVE_FAILED = 0,
    MS_LIBRARY_DRIVE_CREATED,
    MS_LIBRARY_DRIVE_KEPT,     /* already the target */
    MS_LIBRARY_DRIVE_REPLACED, /* MetalSharp's earlier mapping */
    MS_LIBRARY_DRIVE_USER,     /* the user's mapping: left alone */
} ms_library_drive_result;

#define MS_LIBRARY_DRIVE_LEGACY_VOLUME_ROOT 1u /* a link to a volume root is MetalSharp's old mapping */
#define MS_LIBRARY_DRIVE_REPLACE_BROKEN     2u /* a broken link may be replaced */

/* prefix/dosdevices/<letter>: -> target, following the rule above. previous_target may be NULL. */
ms_library_drive_result ms_library_map_drive(const char* prefix, char letter, const char* target,
                                             const char* previous_target, unsigned flags);

/* Creates launcher's folder and maps its drive letter (for prefixed launchers whose prefix exists).
 * Returns whether a location is saved for launcher; a volume that is not mounted is skipped. */
bool ms_library_location_apply(const char* home, const char* launcher);

/* Uninstalling a launcher (launcher_uninstall.c). */
typedef enum {
    MS_LIBRARY_FOLDER_NONE = 0,  /* no location saved */
    MS_LIBRARY_FOLDER_ABSENT,    /* saved, valid, and the folder is not there */
    MS_LIBRARY_FOLDER_DELETABLE, /* the folder may be deleted */
    MS_LIBRARY_FOLDER_REFUSED,   /* it may not: error says why */
} ms_library_folder_check;

/* Whether launcher's saved folder may be deleted: its name is exactly "MetalSharp <Name> Games" for
 * launcher, its parent is the root it was saved on (<user home>/Games, or a /Volumes/<Drive> mount
 * point that is mounted now), it is a real folder (lstat: no link), it resolves to that same place,
 * and it does not hold the MetalSharp home. path gets the saved path (also when refused). */
ms_library_folder_check ms_library_location_check_deletable(const char* home, const char* launcher, char* path,
                                                            size_t path_size, char* error, size_t error_size);
/* Removes the drive letter MetalSharp mapped for launcher, only while it is still a link to the
 * recorded target, and forgets the mapping (the location stays saved). Returns whether a link went;
 * removed gets its path. */
bool ms_library_location_unmap_drive(const char* home, const char* launcher, char* removed, size_t removed_size);
/* Forgets launcher's saved location. */
bool ms_library_location_forget(const char* home, const char* launcher);

/* GET /library-locations. */
char* ms_library_locations_json(const char* home);
/* POST /library-locations/set. */
char* ms_library_locations_set_json(const char* home, const char* body, size_t body_length, int* status);

/* An install request for "gog" or "epic" with installPath filled in from the saved location when it
 * has none (malloc'd, *out_length). NULL with *status and *failure (a JSON body) when there is no
 * usable location (409, needs_location). */
char* ms_library_install_request(const char* home, const char* launcher, const char* body, size_t body_length,
                                 size_t* out_length, int* status, char** failure);

#endif
