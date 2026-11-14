#include "metalsharp_backend/game_registry.h"
#include "metalsharp_backend/wine_reg.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Wine's per-program keys (HKCU\Software\Wine\AppDefaults\<program>.exe) apply to a program whoever starts it, where
 * an environment variable reaches only the processes MetalSharp starts: Assetto Corsa's launcher starts the game
 * itself, with its own environment. */

/* Assetto Corsa (244210):
 *   AssettoCorsa.exe, its .NET launcher, keeps CEF3.dll in launcher\support and finds it from its own AssemblyResolve
 *   handler, which Mono runs too late: MetalSharp Wine's mscoree adds the AppDefaults Mono key's PrivatePath to the
 *   folders assemblies are found in.
 *   acs.exe, the game, calls D3DX11CreateShaderResourceViewFromFileW, which Wine's d3dx11_43 lacks; Microsoft's,
 *   from the DirectX June 2010 runtime the game installs, has it. */
static const ms_game_registry_value assetto_corsa[] = {
    {"HKCU\\Software\\Wine\\AppDefaults\\AssettoCorsa.exe\\Mono", "PrivatePath", "launcher\\support"},
    {"HKCU\\Software\\Wine\\AppDefaults\\acs.exe\\DllOverrides", "d3dx11_43", "native,builtin"},
};

static const struct {
    unsigned appid;
    const char* revision; /* changed when the values change, so prefixes that have the old ones get these */
    const ms_game_registry_value* values;
    size_t count;
} games[] = {
    {244210, "1", assetto_corsa, sizeof(assetto_corsa) / sizeof(assetto_corsa[0])},
};

static size_t game_index(unsigned appid) {
    for (size_t i = 0; i < sizeof(games) / sizeof(games[0]); i++)
        if (games[i].appid == appid)
            return i;
    return (size_t)-1;
}

const ms_game_registry_value* ms_game_registry_values(unsigned appid, size_t* count) {
    size_t i = game_index(appid);
    if (count)
        *count = i == (size_t)-1 ? 0 : games[i].count;
    return i == (size_t)-1 ? NULL : games[i].values;
}

static bool marker_has(const char* marker, const char* revision) {
    char line[32] = {0};
    FILE* file = fopen(marker, "r");
    bool same;
    if (!file)
        return false;
    same = fgets(line, sizeof(line), file) && !strncmp(line, revision, strlen(revision)) &&
           (line[strlen(revision)] == '\n' || !line[strlen(revision)]);
    fclose(file);
    return same;
}

bool ms_game_registry_apply(const char* home, const char* prefix, unsigned appid) {
    size_t i = game_index(appid);
    char dir[PATH_MAX], marker[PATH_MAX], wine[PATH_MAX];
    struct stat st;
    bool ok = true;
    FILE* file;
    if (i == (size_t)-1)
        return true;
    if (!home || !prefix || !prefix[0] || stat(prefix, &st) != 0 || !S_ISDIR(st.st_mode))
        return false;
    if ((size_t)snprintf(dir, sizeof(dir), "%s/%s", prefix, MS_GAME_REGISTRY_MARKER_DIR) >= sizeof(dir) ||
        (size_t)snprintf(marker, sizeof(marker), "%s/%u", dir, appid) >= sizeof(marker))
        return false;
    if (marker_has(marker, games[i].revision))
        return true;
    if ((size_t)snprintf(wine, sizeof(wine), "%s/runtime/wine/bin/wine", home) >= sizeof(wine) ||
        access(wine, X_OK) != 0)
        return false;
    for (size_t v = 0; v < games[i].count; v++) {
        char* const args[] = {"add", (char*)games[i].values[v].key,
                              "/v",  (char*)games[i].values[v].name,
                              "/t",  "REG_SZ",
                              "/d",  (char*)games[i].values[v].value,
                              "/f",  NULL};
        ok = ms_wine_reg(wine, prefix, args) && ok;
    }
    if (!ok || (mkdir(dir, 0755) != 0 && errno != EEXIST) || !(file = fopen(marker, "w")))
        return false;
    fprintf(file, "%s\n", games[i].revision);
    return fclose(file) == 0;
}
