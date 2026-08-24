#include "metalsharp_backend/emulators.h"
#include "metalsharp_backend/json_writer.h"

#include <stdlib.h>

char* ms_emulators_json(const char* home) {
    ms_json_writer w;
    (void)home;
    ms_json_writer_init(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "ok");
    ms_json_writer_bool(&w, true);
    ms_json_writer_key(&w, "providers");
    ms_json_writer_array_begin(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "id");
    ms_json_writer_string(&w, "rpcs3");
    ms_json_writer_key(&w, "name");
    ms_json_writer_string(&w, "RPCS3");
    ms_json_writer_key(&w, "platform");
    ms_json_writer_string(&w, "PlayStation 3");
    ms_json_writer_key(&w, "supported");
    ms_json_writer_bool(&w, true);
    ms_json_writer_object_end(&w);
    ms_json_writer_object_begin(&w);
    ms_json_writer_key(&w, "id");
    ms_json_writer_string(&w, "rpcs4");
    ms_json_writer_key(&w, "name");
    ms_json_writer_string(&w, "RPCS4");
    ms_json_writer_key(&w, "platform");
    ms_json_writer_string(&w, "PlayStation 4");
    ms_json_writer_key(&w, "supported");
    ms_json_writer_bool(&w, false);
    ms_json_writer_object_end(&w);
    ms_json_writer_array_end(&w);
    ms_json_writer_object_end(&w);
    return ms_json_writer_take(&w);
}
