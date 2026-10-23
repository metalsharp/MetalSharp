/* The fex build's runtime/wine facade, written into a scratch home whose
 * runtime/MetalSharp-Wine.app links to a real MetalSharp Wine app (argv[2]); without one, only
 * the pipeline mapping and path checks run. Built with -DMS_BACKEND_VARIANT="fex". */
#include "metalsharp_backend/wine_runtime.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;

#define EXPECT(condition)                                                                                              \
    do {                                                                                                               \
        if (!(condition)) {                                                                                            \
            fprintf(stderr, "%s:%d: expected %s\n", __FILE__, __LINE__, #condition);                                   \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

static void write_text(const char* path, const char* text) {
    FILE* file = fopen(path, "wb");
    if (file) {
        fputs(text, file);
        fclose(file);
    }
}

int main(int argc, char** argv) {
    char path[PATH_MAX], other[PATH_MAX], command[PATH_MAX * 3], output[256] = "";
    const char* home = argc > 1 ? argv[1] : NULL;
    const char* app = argc > 2 ? argv[2] : NULL;
    FILE* pipe;

    EXPECT(ms_wine_is_fex() && !strcmp(ms_wine_variant(), "fex"));
    EXPECT(!strcmp(ms_wine_route_pipeline("d3dmetal"), "dxmt"));
    EXPECT(!strcmp(ms_wine_route_pipeline("vkd3d"), "dxmt"));
    EXPECT(!strcmp(ms_wine_route_pipeline("m13"), "dxmt"));
    EXPECT(!strcmp(ms_wine_route_pipeline("d3d9"), "d3d9"));
    EXPECT(!strcmp(ms_wine_fex_graphics_backend("wine_bare"), "wine"));
    EXPECT(!strcmp(ms_wine_fex_graphics_backend("dxvk"), "dxvk"));
    EXPECT(!strcmp(ms_wine_fex_graphics_backend("dxmt_32"), "dxmt"));
    EXPECT(ms_wine_is_runtime_wine_path("/h", "/h/runtime/MetalSharp-Wine.app/Contents/MacOS/wine"));
    EXPECT(ms_wine_is_runtime_wine_path("/h", "/h/runtime/MetalSharp-Wine.app/Contents/lib/wine/aarch64-unix/wine"));
    EXPECT(ms_wine_is_runtime_wine_path("/h", "/h/runtime/wine/lib/wine/x86_64-unix/wine"));
    EXPECT(!ms_wine_is_runtime_wine_path("/h", "/h/runtime/MetalSharp-Wine.app/Contents/MacOS/wineserver"));
    EXPECT(!ms_wine_is_runtime_wine_path("/h", "/other/runtime/wine/bin/wine"));
    EXPECT(ms_wine_server_path("/h", path, sizeof(path)) &&
           !strcmp(path, "/h/runtime/MetalSharp-Wine.app/Contents/MacOS/wineserver"));
    if (!home) {
        fprintf(stderr, "usage: %s <scratch home> [MetalSharp-Wine.app]\n", argv[0]);
        return 2;
    }

    /* A prefix the fex build converted gets its x86_64 backup back; one without a backup is
     * counted and left alone; an x86_64 prefix is not touched. */
    {
        static const char* const dirs[] = {"prefix-steam/drive_c/windows",
                                           "prefix-steam/.metalsharp-x86_64-backup/drive_c/windows",
                                           "prefix-ubisoft/drive_c/windows", "sharp-prefix/drive_c/windows"};
        char command[PATH_MAX * 2];
        for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
            snprintf(command, sizeof(command), "mkdir -p '%s/%s'", home, dirs[i]);
            EXPECT(system(command) == 0);
        }
        snprintf(path, sizeof(path), "%s/prefix-steam/system.reg", home);
        write_text(path, "arm64 registry\n");
        snprintf(path, sizeof(path), "%s/prefix-steam/drive_c/windows/arm64.txt", home);
        write_text(path, "arm64\n");
        snprintf(path, sizeof(path), "%s/prefix-steam/.metalsharp-wine-arch", home);
        write_text(path, "arm64\n");
        snprintf(path, sizeof(path), "%s/prefix-steam/.metalsharp-x86_64-backup/system.reg", home);
        write_text(path, "x86_64 registry\n");
        snprintf(path, sizeof(path), "%s/prefix-steam/.metalsharp-x86_64-backup/drive_c/windows/x86_64.txt", home);
        write_text(path, "x86_64\n");
        snprintf(path, sizeof(path), "%s/prefix-steam/.metalsharp-x86_64-backup/.complete", home);
        write_text(path, "x86_64\n");
        snprintf(path, sizeof(path), "%s/prefix-ubisoft/system.reg", home);
        write_text(path, "arm64 registry\n");
        snprintf(path, sizeof(path), "%s/prefix-ubisoft/.metalsharp-wine-arch", home);
        write_text(path, "arm64\n");
        snprintf(path, sizeof(path), "%s/sharp-prefix/system.reg", home);
        write_text(path, "x86_64 registry\n");
        EXPECT(ms_wine_restore_x86_64_prefixes(home) == 1);
        snprintf(path, sizeof(path), "%s/prefix-steam/drive_c/windows/x86_64.txt", home);
        EXPECT(access(path, R_OK) == 0);
        snprintf(path, sizeof(path), "%s/prefix-steam/drive_c/windows/arm64.txt", home);
        EXPECT(access(path, F_OK) != 0);
        snprintf(path, sizeof(path), "%s/prefix-steam", home);
        EXPECT(!ms_wine_prefix_is_arm64(path));
        snprintf(path, sizeof(path), "%s/prefix-steam/.metalsharp-x86_64-backup", home);
        EXPECT(access(path, F_OK) != 0);
        snprintf(path, sizeof(path), "%s/prefix-ubisoft", home);
        EXPECT(ms_wine_prefix_is_arm64(path));
        snprintf(command, sizeof(command), "rm -rf '%s/prefix-steam' '%s/prefix-ubisoft' '%s/sharp-prefix'", home, home,
                 home);
        EXPECT(system(command) == 0);
    }

    /* No app: refused, and nothing is created. */
    snprintf(path, sizeof(path), "%s/runtime", home);
    mkdir(path, 0755);
    EXPECT(!ms_wine_fex_write_facade(home, other, sizeof(other)));
    EXPECT(!ms_wine_fex_facade_ready(home));

    if (!app) {
        printf("wine runtime tests passed (no MetalSharp Wine app; facade not run)\n");
        return failures ? 1 : 0;
    }
    snprintf(path, sizeof(path), "%s/runtime/MetalSharp-Wine.app", home);
    EXPECT(symlink(app, path) == 0);

    /* A 0.80.0 x86_64 runtime is kept aside as runtime/wine-x86_64. */
    snprintf(path, sizeof(path), "%s/runtime/wine", home);
    mkdir(path, 0755);
    snprintf(path, sizeof(path), "%s/runtime/wine/x86_64-runtime", home);
    write_text(path, "x86_64\n");
    EXPECT(ms_wine_fex_write_facade(home, other, sizeof(other)));
    snprintf(path, sizeof(path), "%s/runtime/wine-x86_64/x86_64-runtime", home);
    EXPECT(access(path, R_OK) == 0);
    EXPECT(ms_wine_fex_facade_ready(home));
    /* Writing it again (an update) keeps the runtime that was set aside. */
    EXPECT(ms_wine_fex_write_facade(home, other, sizeof(other)));
    EXPECT(access(path, R_OK) == 0);

    /* The x86_64 runtime's paths do not reach MetalSharp Wine. */
    snprintf(command, sizeof(command),
             "DYLD_FALLBACK_LIBRARY_PATH=/nonexistent WINEDLLPATH=/nonexistent VK_ICD_FILENAMES=/nonexistent "
             "'%s/runtime/wine/bin/wine' --version 2>&1",
             home);
    pipe = popen(command, "r");
    if (pipe) {
        if (!fgets(output, sizeof(output), pipe))
            output[0] = '\0';
        pclose(pipe);
    }
    EXPECT(strstr(output, "MetalSharp Wine") != NULL);
    snprintf(path, sizeof(path), "%s/runtime/wine/bin/wineserver", home);
    EXPECT(access(path, X_OK) == 0);
    snprintf(path, sizeof(path), "%s/runtime/wine/lib/wine/aarch64-unix/wine", home);
    EXPECT(access(path, X_OK) == 0);
    if (failures)
        fprintf(stderr, "wine --version said: %s\n", output);
    else
        printf("wine runtime tests passed (%s)", output);
    return failures ? 1 : 0;
}
