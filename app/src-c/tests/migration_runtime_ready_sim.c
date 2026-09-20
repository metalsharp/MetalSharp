/* End-to-end check for migration.c's runtime_ready(): build the harness, then
 * drive it through tests/runtime-ready-sim.sh, which assembles a faithful
 * installed tree from the published bundle archives exactly the way the
 * installer does (extract runtime, stage graphics lanes, ad-hoc re-sign the
 * DXMT bridge, rewrite the runtime MoltenVK ICD, write the DXMT manifest) and
 * asserts runtime_ready() accepts it. This pins the contract that healthy
 * installs from the published `bundles` release never trip the "runtime
 * bundle is incomplete" false error.
 *
 * Modes:
 *   migration-runtime-ready-sim write-manifest <dxmt-dir>
 *       Write metalsharp-dxmt-runtime.json the way setup.c's
 *       write_dxmt_manifest does, using this build's MIGRATION_VERSION.
 *   migration-runtime-ready-sim check <home>
 *       Exit 0 when runtime_ready() accepts the tree.
 */
#include "../runtime/migration.c"
#include <stdio.h>

/* migration.c links neither setup.c nor steam_actions.c in this harness; the
 * quarantine wipe does not affect content hashes and Steam wrapper deployment
 * is out of scope for the layout contract under test. */
void ms_clear_quarantine_tree(const char* path) {
    (void)path;
}

bool ms_steam_wrappers_ensure(const char* home) {
    (void)home;
    return true;
}

char* ms_setup_install_all_json(const char* home, int* status) {
    (void)home;
    (void)status;
    return NULL;
}

char* ms_steam_stop_json(const char* home, int* status) {
    (void)home;
    (void)status;
    return strdup("{\"ok\":true,\"running\":false}");
}

static int write_manifest(const char* dxmt_dir) {
    char* path = path_join(dxmt_dir, "metalsharp-dxmt-runtime.json");
    char json[256];
    FILE* file;
    int ok;
    if (!path)
        return 1;
    snprintf(json, sizeof(json),
             "{\"schema\":\"metalsharp.dxmt-runtime.v2\",\"version\":\"%s-dxmt-v0.80-baseline-v1\","
             "\"source\":\"bundled:metalsharp-graphics-dll.tar.zst\"}",
             MIGRATION_VERSION);
    file = fopen(path, "wb");
    ok = file != NULL && fputs(json, file) >= 0;
    if (file)
        ok = fclose(file) == 0 && ok;
    if (!ok)
        fprintf(stderr, "failed to write %s\n", path);
    free(path);
    return ok ? 0 : 1;
}

int main(int argc, char** argv) {
    if (argc == 3 && strcmp(argv[1], "write-manifest") == 0)
        return write_manifest(argv[2]);
    if (argc == 3 && strcmp(argv[1], "check") == 0) {
        bool ready = runtime_ready(argv[2]);
        printf("runtime_ready(%s) -> %s\n", argv[2], ready ? "ready" : "NOT ready");
        return ready ? 0 : 1;
    }
    fprintf(stderr, "usage: %s write-manifest <dxmt-dir> | check <home>\n", argv[0]);
    return 2;
}
