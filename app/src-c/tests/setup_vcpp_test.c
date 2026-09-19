/* Regression tests for the async VC++ installer progress file contract:
 * write/read round-trip, wine_pid liveness fields, and DLL presence checks
 * against a fake prefix. Guards the wizard's installer-detection flow — the
 * "installer completed but was not detected / had to run twice" bug class. */
#include "../runtime/setup.c"
#include <assert.h>
#include <limits.h>
#include <string.h>
#include <unistd.h>

static void fixture(const char* home, const char* relative, const char* bytes) {
    char* path = join_path(home, relative);
    char* parent = strdup(path);
    *strrchr(parent, '/') = '\0';
    assert(mkdir_p(parent));
    FILE* f = fopen(path, "wb");
    assert(f);
    assert(fputs(bytes, f) >= 0);
    assert(fclose(f) == 0);
    free(parent);
    free(path);
}

int main(int argc, char** argv) {
    const char* home = argc > 1 ? argv[1] : "/tmp/ms-vcpp-test-home";
    char state[16] = {0};
    char* status_json;
    char dll_payload[20001];
    memset(dll_payload, 'x', sizeof(dll_payload) - 1);
    dll_payload[sizeof(dll_payload) - 1] = '\0';

    /* vcpp_dlls_present: nothing installed in a fresh home. */
    assert(!vcpp_dlls_present(home, false));
    assert(!vcpp_dlls_present(home, true));

    /* x64: three DLLs in system32, each must be non-trivially sized. */
    fixture(home, "prefix-steam/drive_c/windows/system32/vcruntime140.dll", dll_payload);
    assert(!vcpp_dlls_present(home, false)); /* still missing the other two */
    fixture(home, "prefix-steam/drive_c/windows/system32/vcruntime140_1.dll", dll_payload);
    fixture(home, "prefix-steam/drive_c/windows/system32/msvcp140.dll", dll_payload);
    assert(vcpp_dlls_present(home, false));
    assert(!vcpp_dlls_present(home, true)); /* syswow64 untouched */

    /* Zero-byte stub DLLs must not count as installed. */
    fixture(home, "prefix-steam/drive_c/windows/syswow64/vcruntime140.dll", "");
    assert(!vcpp_dlls_present(home, true));

    /* Progress file round-trip without wine_pid. */
    write_vcpp_progress(home, "x64", "preparing", NULL);
    assert(read_vcpp_progress_state(home, state, sizeof(state)) == 0);
    assert(!strcmp(state, "preparing"));

    /* Progress file round-trip with wine_pid (orphan-detection contract). */
    write_vcpp_progress_ex(home, "x86", "running", NULL, 424242);
    assert(read_vcpp_progress_state(home, state, sizeof(state)) == 424242);
    assert(!strcmp(state, "running"));

    /* Error state round-trips and survives the JSON parser. */
    write_vcpp_progress(home, "x64", "error", "MetalSharp Wine not found");
    assert(read_vcpp_progress_state(home, state, sizeof(state)) == 0);
    assert(!strcmp(state, "error"));

    /* The status endpoint parses the same file into wizard-facing JSON. */
    write_vcpp_progress_ex(home, "x64", "complete", NULL, 424242);
    status_json = ms_setup_vcpp_status_json(home);
    assert(status_json);
    assert(strstr(status_json, "\"x64_installed\":true"));
    assert(strstr(status_json, "\"installing\":false"));
    assert(strstr(status_json, "\"status\":\"complete\""));
    free(status_json);

    /* Out-of-range pids from a corrupt progress file are dead, never
     * truncated into an unrelated live process. */
    assert(!vcpp_pid_alive(0));
    assert(!vcpp_pid_alive(-1));
    assert(!vcpp_pid_alive((long long)INT_MAX + 12345));
    assert(!vcpp_pid_alive((long long)INT_MAX)); /* beyond Darwin's pid ceiling */

    /* A non-terminal state whose recorded wine pid is STILL ALIVE (our own
     * pid is conveniently live) must report installing and leave the file
     * untouched — the install endpoint's orphan gate depends on the recorded
     * wine_pid surviving status polls after a backend restart. */
    write_vcpp_progress_ex(home, "x86", "running", NULL, (pid_t)getpid());
    status_json = ms_setup_vcpp_status_json(home);
    assert(status_json);
    assert(strstr(status_json, "\"installing\":true"));
    assert(strstr(status_json, "\"status\":\"running\""));
    free(status_json);
    assert(read_vcpp_progress_state(home, state, sizeof(state)) == (long long)getpid());
    assert(!strcmp(state, "running"));

    /* A non-terminal state with a dead/reaped worker is corrected to a
     * terminal state (backend-restart staleness contract). */
    write_vcpp_progress_ex(home, "x64", "running", NULL, 424242);
    status_json = ms_setup_vcpp_status_json(home);
    assert(status_json);
    /* pid 424242 is not a live process owned by us: must not stay "running". */
    assert(!strstr(status_json, "\"status\":\"running\""));
    free(status_json);

    /* Malformed progress file must not crash or wedge the endpoint. */
    fixture(home, "vcpp_progress.json", "{not json");
    status_json = ms_setup_vcpp_status_json(home);
    assert(status_json && strstr(status_json, "\"ok\":true"));
    free(status_json);

    printf("vcpp installer progress regression tests passed\n");
    return 0;
}
