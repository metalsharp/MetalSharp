#include "metalsharp_backend/rpcs4.h"
#include "metalsharp_backend/json_writer.h"

#include <stdlib.h>
#include <string.h>

static char* unsupported(const char* action) {
    ms_json_writer w;
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, false);
    ms_json_writer_key(&w, "provider");
    ms_json_writer_string(&w, "rpcs4");
    ms_json_writer_key(&w, "state");
    ms_json_writer_string(&w, "unsupported_upstream");
    ms_json_writer_key(&w, "action");
    ms_json_writer_string(&w, action ? action : "status");
    ms_json_writer_key(&w, "error");
    ms_json_writer_string(&w, "RPCS4 has no maintained macOS distribution or usable release");
    ms_json_writer_object_end(&w);
    return ms_json_writer_take(&w);
}

char* ms_rpcs4_status_json(const char* home) {
    ms_json_writer w;
    (void)home;
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, true);
    ms_json_writer_key(&w, "provider");
    ms_json_writer_string(&w, "rpcs4");
    ms_json_writer_key(&w, "name");
    ms_json_writer_string(&w, "RPCS4");
    ms_json_writer_key(&w, "platform");
    ms_json_writer_string(&w, "PlayStation 4");
    ms_json_writer_key(&w, "supported");
    ms_json_writer_bool(&w, false);
    ms_json_writer_key(&w, "installAvailable");
    ms_json_writer_bool(&w, false);
    ms_json_writer_key(&w, "state");
    ms_json_writer_string(&w, "unsupported_upstream");
    ms_json_writer_key(&w, "repository");
    ms_json_writer_string(&w, "https://github.com/xYaroslavGTx/rpcs4");
    ms_json_writer_key(&w, "lastUpstreamCommit");
    ms_json_writer_string(&w, "2016-05-18");
    ms_json_writer_key(&w, "reason");
    ms_json_writer_string(
        &w, "The candidate repository has one Windows-only skeleton commit, no releases, no CI, and no macOS runtime.");
    ms_json_writer_key(&w, "readinessGate");
    ms_json_writer_array_begin(&w);
    const char* gates[] = {"Maintained upstream", "Native macOS build",     "Versioned releases",
                           "Stable launch CLI",   "Documented game layout", "Integrity-verifiable artifacts",
                           "Boot evidence"};
    for (size_t i = 0; i < sizeof(gates) / sizeof(gates[0]); ++i)
        ms_json_writer_string(&w, gates[i]);
    ms_json_writer_array_end(&w);
    ms_json_writer_object_end(&w);
    return ms_json_writer_take(&w);
}

char* ms_rpcs4_games_json(const char* home) {
    ms_json_writer w;
    (void)home;
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, true);
    ms_json_writer_key(&w, "provider");
    ms_json_writer_string(&w, "rpcs4");
    ms_json_writer_key(&w, "games");
    ms_json_writer_array_begin(&w);
    ms_json_writer_array_end(&w);
    ms_json_writer_object_end(&w);
    return ms_json_writer_take(&w);
}

char* ms_rpcs4_action_json(const char* home, const char* action, const unsigned char* body, size_t length) {
    (void)home;
    (void)body;
    (void)length;
    return unsupported(action);
}
