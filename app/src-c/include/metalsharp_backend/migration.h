#ifndef METALSHARP_BACKEND_MIGRATION_BASIC_H
#define METALSHARP_BACKEND_MIGRATION_BASIC_H
#include <stdbool.h>
#include <stddef.h>
char* ms_migration_check_json(const char*);
char* ms_migration_start_json(const char*);
char* ms_migration_progress_json(const char*);
char* ms_migration_report_json(const char*);

/* What a migration does with an existing install, from the build running ("regular" or "fex"), the
 * build that last migrated it (setup.json runtime_variant; missing on 0.80.0 and earlier, which is
 * the regular build) and the version it was then (last_migrated_version):
 *   NONE            nothing to do
 *   FULL            regular -> regular: the regular build's migration, as before 1.0.0
 *   FEX_TO_REGULAR  FULL, and prefixes the FEX build converted get their x86_64 backup back first
 *                   (the x86_64 Wine cannot run a prefix MetalSharp Wine built)
 *   REGULAR_TO_FEX  the full migration, and every x86_64 prefix is converted to MetalSharp Wine
 *                   (0.80.0 or any regular build -> FEX 1.0.0 or later)
 *   FEX_UPDATE      FEX 1.0.0 or later -> a newer FEX build: the new MetalSharp Wine and the
 *                   app's runtime pieces only; user data stays in place and no prefix is converted
 * `installed` is a completed setup or an existing Steam prefix; `marker_needed` is the updater's
 * .post-update-migration handoff. */
typedef enum {
    MS_MIGRATION_PATH_NONE,
    MS_MIGRATION_PATH_FULL,
    MS_MIGRATION_PATH_FEX_TO_REGULAR,
    MS_MIGRATION_PATH_REGULAR_TO_FEX,
    MS_MIGRATION_PATH_FEX_UPDATE,
} ms_migration_path;
ms_migration_path ms_migration_path_for(const char* running_variant, const char* recorded_variant,
                                        const char* from_version, bool installed, bool marker_needed,
                                        bool runtime_ready);
const char* ms_migration_path_name(ms_migration_path path);

/* The FEX build's welcome, shown once after a REGULAR_TO_FEX migration: GET says whether it is due,
 * POST .../dismiss records that it was shown. Never due on the regular build. */
bool ms_migration_fex_welcome_due(ms_migration_path path);
char* ms_migration_fex_welcome_json(const char* home);
char* ms_migration_fex_welcome_dismiss_json(const char* home);
#endif
