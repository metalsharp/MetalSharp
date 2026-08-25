#include "metalsharp_backend/backend.h"

#include "metalsharp_backend/antidebug.h"
#include "metalsharp_backend/bottle_actions.h"
#include "metalsharp_backend/bottles.h"
#include "metalsharp_backend/cache.h"
#include "metalsharp_backend/config.h"
#include "metalsharp_backend/d3dmetal.h"
#include "metalsharp_backend/diagnostics.h"
#include "metalsharp_backend/emulators.h"
#include "metalsharp_backend/epic.h"
#include "metalsharp_backend/es_bridge.h"
#include "metalsharp_backend/game.h"
#include "metalsharp_backend/gamejolt.h"
#include "metalsharp_backend/gog.h"
#include "metalsharp_backend/integration.h"
#include "metalsharp_backend/json.h"
#include "metalsharp_backend/kernel_apc.h"
#include "metalsharp_backend/kernel_driver.h"
#include "metalsharp_backend/kernel_extra.h"
#include "metalsharp_backend/kernel_handles.h"
#include "metalsharp_backend/kernel_integrity.h"
#include "metalsharp_backend/logs.h"
#include "metalsharp_backend/metalfx.h"
#include "metalsharp_backend/migration.h"
#include "metalsharp_backend/mono.h"
#include "metalsharp_backend/mtsp.h"
#include "metalsharp_backend/ob_callbacks.h"
#include "metalsharp_backend/pcsx2.h"
#include "metalsharp_backend/process.h"
#include "metalsharp_backend/rpcs3.h"
#include "metalsharp_backend/scan.h"
#include "metalsharp_backend/setup.h"
#include "metalsharp_backend/shadps4.h"
#include "metalsharp_backend/sharp.h"
#include "metalsharp_backend/sharpemu.h"
#include "metalsharp_backend/steam.h"
#include "metalsharp_backend/steam_actions.h"
#include "metalsharp_backend/thread.h"
#include "metalsharp_backend/updater.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0x00000100
#endif

static char* home_path(void) {
    const char* configured = getenv("METALSHARP_HOME");
    const char* home = getenv("HOME");
    size_t length;
    char* path;
    if (configured != NULL) {
        const char* start = configured;
        const char* end = configured + strlen(configured);
        while (start < end && isspace((unsigned char)*start))
            start++;
        while (end > start && isspace((unsigned char)end[-1]))
            end--;
        if (end > start)
            return strndup(start, (size_t)(end - start));
    }
    if (home == NULL || home[0] == '\0')
        home = ".";
    length = strlen(home) + strlen("/.metalsharp") + 1;
    path = (char*)malloc(length);
    if (path != NULL)
        (void)snprintf(path, length, "%s/.metalsharp", home);
    return path;
}

static unsigned short bridge_port(void) {
    const char* value = getenv("METALSHARP_STEAM_BRIDGE_PORT");
    char* end = NULL;
    unsigned long parsed;
    if (value == NULL || value[0] == '\0')
        return 18733;
    errno = 0;
    parsed = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || parsed == 0 || parsed > 65535)
        return 18733;
    return (unsigned short)parsed;
}

void ms_backend_context_init(ms_backend_context* context, const char* version) {
    if (context == NULL)
        return;
    memset(context, 0, sizeof(*context));
    context->version = version == NULL ? MS_BACKEND_DEFAULT_VERSION : version;
    context->metalsharp_home = home_path();
    context->steam_bridge_port = bridge_port();
    context->dev_mode = getenv("METALSHARP_DEV") != NULL && strcmp(getenv("METALSHARP_DEV"), "1") == 0;
}

static char* make_json(const char* version, const char* home, bool dev_mode) {
    char* quoted_version = ms_json_quote(version == NULL ? MS_BACKEND_DEFAULT_VERSION : version);
    char* quoted_home = ms_json_quote(home == NULL ? "" : home);
    char* body;
    int needed;
    if (quoted_version == NULL || quoted_home == NULL) {
        free(quoted_version);
        free(quoted_home);
        return NULL;
    }
    needed = snprintf(NULL, 0, "{\"ok\":true,\"version\":%s,\"pid\":%ld,\"dev_mode\":%s,\"metalsharp_home\":%s}",
                      quoted_version, (long)getpid(), dev_mode ? "true" : "false", quoted_home);
    if (needed < 0) {
        free(quoted_version);
        free(quoted_home);
        return NULL;
    }
    body = (char*)malloc((size_t)needed + 1);
    if (body != NULL) {
        (void)snprintf(body, (size_t)needed + 1,
                       "{\"ok\":true,\"version\":%s,\"pid\":%ld,\"dev_mode\":%s,\"metalsharp_home\":%s}",
                       quoted_version, (long)getpid(), dev_mode ? "true" : "false", quoted_home);
    }
    free(quoted_version);
    free(quoted_home);
    return body;
}

static char* host_abi_json(unsigned short port) {
    static const char body_template[] =
        "{\"ok\":true,\"magic\":\"MSAB\",\"version\":{\"major\":1,\"minor\":0},"
        "\"services\":[\"process\",\"paths\",\"logging\",\"steam\",\"graphics\",\"audio\",\"input\",\"managed_"
        "runtime\"],"
        "\"steam_bridge\":{\"default_port\":18733,\"active_port\":%u,\"env\":\"METALSHARP_STEAM_BRIDGE_PORT\"},"
        "\"managed_runtime_env\":[\"METALSHARP_MONO_LIB\",\"METALSHARP_MONO_ROOT\",\"METALSHARP_MONO_ASSEMBLY_DIR\","
        "\"METALSHARP_MONO_CONFIG_DIR\"]}";
    int needed = snprintf(NULL, 0, body_template, (unsigned)port);
    char* body;
    if (needed < 0)
        return NULL;
    body = (char*)malloc((size_t)needed + 1);
    if (body != NULL)
        (void)snprintf(body, (size_t)needed + 1, body_template, (unsigned)port);
    return body;
}

static _Thread_local const char* ms_current_request_path;

static void set_json_response(ms_http_response* response, int status, char* body) {
    if (ms_current_request_path && strncmp(ms_current_request_path, "/kernel-translation/", 20) == 0)
        status = 200;
    response->status = status;
    response->content_type = "application/json";
    response->body = (const unsigned char*)body;
    response->body_length = body == NULL ? 0 : strlen(body);
    response->owns_body = body != NULL;
}
static unsigned char* read_binary(const char* path, size_t* length) {
    struct stat st;
    unsigned char* data;
    size_t used = 0;
    if (!path || !length)
        return NULL;
    int fd = open(path, O_RDONLY | O_NOFOLLOW);
    if (fd < 0 || fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0 || st.st_size > 16 * 1024 * 1024) {
        if (fd >= 0)
            close(fd);
        return NULL;
    }
    data = malloc((size_t)st.st_size);
    if (!data) {
        close(fd);
        return NULL;
    }
    while (used < (size_t)st.st_size) {
        ssize_t got = read(fd, data + used, (size_t)st.st_size - used);
        if (got < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (got == 0)
            break;
        used += (size_t)got;
    }
    close(fd);
    if (used != (size_t)st.st_size) {
        free(data);
        return NULL;
    }
    *length = used;
    return data;
}
static const char* image_type(const char* path) {
    const char* dot = strrchr(path, '.');
    if (dot && (!strcasecmp(dot, ".png")))
        return "image/png";
    if (dot && (!strcasecmp(dot, ".svg")))
        return "image/svg+xml";
    if (dot && (!strcasecmp(dot, ".webp")))
        return "image/webp";
    return "image/jpeg";
}

bool ms_backend_handle(const ms_http_request* request, ms_http_response* response, void* context_ptr) {
    ms_backend_context* context = (ms_backend_context*)context_ptr;
    char* body;
    if (request == NULL || response == NULL || context == NULL)
        return false;
    ms_current_request_path = request->path;
    ms_bottle_poll();
    memset(response, 0, sizeof(*response));
    if (strcmp(request->method, "GET") != 0 && strcmp(request->method, "POST") != 0) {
        body = strdup("{\"ok\":false,\"error\":\"method not allowed\"}");
        if (body == NULL)
            return false;
        set_json_response(response, 405, body);
        return true;
    }
    if (strcmp(request->method, "GET") == 0 && strcmp(request->path, "/status") == 0) {
        ms_log_event(context->metalsharp_home, "Backend status checked");
        body = make_json(context->version, context->metalsharp_home, context->dev_mode);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "GET") == 0 && strcmp(request->path, "/runtime/host-abi") == 0) {
        body = host_abi_json(context->steam_bridge_port);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "GET") == 0 && strcmp(request->path, "/config") == 0) {
        body = ms_config_get_json(context->metalsharp_home);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(request->path, "/config") == 0) {
        int status = 500;
        body = ms_config_set_json(context->metalsharp_home, request->body, request->body_length, &status);
        if (body == NULL)
            return false;
        set_json_response(response, status, body);
        return true;
    }
    if (strcmp(request->method, "GET") == 0 && strcmp(request->path, "/metalfx/state") == 0) {
        body = ms_metalfx_get_json(context->metalsharp_home);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(request->path, "/metalfx/toggle") == 0) {
        body = ms_metalfx_set_json(context->metalsharp_home, request->body, request->body_length);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "GET") == 0 && strcmp(request->path, "/setup/state") == 0) {
        body = ms_setup_state_json(context->metalsharp_home);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(request->path, "/setup/save") == 0) {
        int status = 500;
        body = ms_setup_save_json(context->metalsharp_home, request->body, request->body_length, &status);
        if (body == NULL)
            return false;
        set_json_response(response, status, body);
        return true;
    }
    if (strcmp(request->method, "GET") == 0 && strcmp(request->path, "/setup/device-name") == 0) {
        body = ms_setup_device_name_json();
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "GET") == 0 && strcmp(request->path, "/setup/install-progress") == 0) {
        body = ms_setup_install_progress_json(context->metalsharp_home);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "GET") == 0 && strcmp(request->path, "/setup/installing") == 0) {
        body = ms_setup_installing_json();
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "GET") == 0 && strcmp(request->path, "/setup/dependencies") == 0) {
        body = ms_setup_dependencies_json(context->metalsharp_home);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "GET") == 0 && strcmp(request->path, "/setup/agility-versions") == 0) {
        body = ms_setup_agility_versions_json();
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(request->path, "/setup/install-deps") == 0) {
        int status = 500;
        body = ms_setup_install_dependencies_json(request->body, request->body_length, &status);
        if (body == NULL)
            return false;
        set_json_response(response, status, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(request->path, "/setup/install-all") == 0) {
        int status = 500;
        body = ms_setup_install_all_json(context->metalsharp_home, &status);
        if (body == NULL)
            return false;
        set_json_response(response, status, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(request->path, "/setup/install-vcpp-x64") == 0) {
        int status = 500;
        body = ms_setup_install_vcpp_json(context->metalsharp_home, false, &status);
        if (body == NULL)
            return false;
        set_json_response(response, status, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(request->path, "/setup/install-vcpp-x86") == 0) {
        int status = 500;
        body = ms_setup_install_vcpp_json(context->metalsharp_home, true, &status);
        if (body == NULL)
            return false;
        set_json_response(response, status, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(request->path, "/kernel-translation/handle/create") == 0) {
        body = ms_kernel_handle_create((const char*)request->body, request->body_length);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(request->path, "/kernel-translation/handle/close") == 0) {
        body = ms_kernel_handle_close((const char*)request->body, request->body_length);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(request->path, "/kernel-translation/handle/duplicate") == 0) {
        body = ms_kernel_handle_duplicate((const char*)request->body, request->body_length);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(request->path, "/kernel-translation/handle/query") == 0) {
        body = ms_kernel_handle_query((const char*)request->body, request->body_length);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(request->path, "/kernel-translation/handle/enumerate") == 0) {
        body = ms_kernel_handle_enumerate((const char*)request->body, request->body_length);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(request->path, "/kernel-translation/handle/system-info") == 0) {
        body = ms_kernel_handle_system_info((const char*)request->body, request->body_length);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(request->path, "/kernel-translation/handle/table-status") == 0) {
        body = ms_kernel_handle_table_status((const char*)request->body, request->body_length);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(request->path, "/kernel-translation/handle/seed-demo") == 0) {
        body = ms_kernel_handle_seed_demo((const char*)request->body, request->body_length);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(request->path, "/kernel-translation/apc/queue") == 0) {
        body = ms_kernel_apc_queue((const char*)request->body, request->body_length);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(request->path, "/kernel-translation/apc/test-alert") == 0) {
        body = ms_kernel_apc_test_alert((const char*)request->body, request->body_length);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(request->path, "/kernel-translation/apc/wait-alertable") == 0) {
        body = ms_kernel_apc_wait_alertable((const char*)request->body, request->body_length);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 &&
        strcmp(request->path, "/kernel-translation/apc/allocate-trampoline") == 0) {
        body = ms_kernel_apc_allocate_trampoline((const char*)request->body, request->body_length);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(request->path, "/kernel-translation/apc/suspend-thread") == 0) {
        body = ms_kernel_apc_suspend_thread((const char*)request->body, request->body_length);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 &&
        strcmp(request->path, "/kernel-translation/apc/get-thread-context") == 0) {
        body = ms_kernel_apc_get_context((const char*)request->body, request->body_length);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 &&
        strcmp(request->path, "/kernel-translation/apc/set-thread-context") == 0) {
        body = ms_kernel_apc_set_context((const char*)request->body, request->body_length);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(request->path, "/kernel-translation/apc/inject-sequence") == 0) {
        body = ms_kernel_apc_inject((const char*)request->body, request->body_length);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(request->path, "/kernel-translation/apc/queue-status") == 0) {
        body = ms_kernel_apc_queue_status((const char*)request->body, request->body_length);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 &&
        strcmp(request->path, "/kernel-translation/apc/trampoline-status") == 0) {
        body = ms_kernel_apc_trampoline_status((const char*)request->body, request->body_length);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 &&
        strcmp(request->path, "/kernel-translation/integrity/query-signing-level") == 0) {
        body = ms_integrity_query_signing_level((const char*)request->body, request->body_length);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 &&
        strcmp(request->path, "/kernel-translation/integrity/query-process-signing") == 0) {
        body = ms_integrity_query_process_signing((const char*)request->body, request->body_length);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 &&
        strcmp(request->path, "/kernel-translation/integrity/register-pe-module") == 0) {
        body = ms_integrity_register_pe((const char*)request->body, request->body_length);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 &&
        strcmp(request->path, "/kernel-translation/integrity/register-macho-module") == 0) {
        body = ms_integrity_register_macho((const char*)request->body, request->body_length);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 &&
        strcmp(request->path, "/kernel-translation/integrity/set-cached-signing-level") == 0) {
        body = ms_integrity_set_cached_level((const char*)request->body, request->body_length);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 &&
        strcmp(request->path, "/kernel-translation/integrity/list-modules") == 0) {
        body = ms_integrity_list_modules((const char*)request->body, request->body_length);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(request->path, "/kernel-translation/integrity/seed-demo") == 0) {
        body = ms_integrity_seed_demo((const char*)request->body, request->body_length);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(request->path, "/kernel-translation/driver/load") == 0) {
        body = ms_driver_load((const char*)request->body, request->body_length);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(request->path, "/kernel-translation/driver/unload") == 0) {
        body = ms_driver_unload((const char*)request->body, request->body_length);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(request->path, "/kernel-translation/driver/list") == 0) {
        body = ms_driver_list((const char*)request->body, request->body_length);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 &&
        strcmp(request->path, "/kernel-translation/driver/create-device") == 0) {
        body = ms_driver_create_device((const char*)request->body, request->body_length);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(request->path, "/kernel-translation/driver/list-devices") == 0) {
        body = ms_driver_list_devices((const char*)request->body, request->body_length);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(request->path, "/kernel-translation/driver/dispatch-irp") == 0) {
        body = ms_driver_dispatch_irp((const char*)request->body, request->body_length);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(request->path, "/kernel-translation/driver/list-irps") == 0) {
        body = ms_driver_list_irps((const char*)request->body, request->body_length);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 &&
        strcmp(request->path, "/kernel-translation/driver/register-ioctl") == 0) {
        body = ms_driver_register_ioctl((const char*)request->body, request->body_length);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(request->path, "/kernel-translation/driver/decode-ioctl") == 0) {
        body = ms_driver_decode_ioctl((const char*)request->body, request->body_length);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(request->path, "/kernel-translation/driver/list-ioctls") == 0) {
        body = ms_driver_list_ioctls((const char*)request->body, request->body_length);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 &&
        strcmp(request->path, "/kernel-translation/driver/type-mapping-survey") == 0) {
        body = ms_driver_type_survey((const char*)request->body, request->body_length);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 &&
        strcmp(request->path, "/kernel-translation/driver/extension-template") == 0) {
        body = ms_driver_extension_template((const char*)request->body, request->body_length);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(request->path, "/kernel-translation/driver/seed-demo") == 0) {
        body = ms_driver_seed_demo((const char*)request->body, request->body_length);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "GET") == 0 && strcmp(request->path, "/setup/agility-versions") == 0) {
        body = ms_setup_agility_versions_json();
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "GET") == 0 && strcmp(request->path, "/setup/dependencies") == 0) {
        body = ms_setup_dependencies_json(context->metalsharp_home);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "GET") == 0 && strcmp(request->path, "/mtsp/default-rules") == 0) {
        body = ms_mtsp_default_rules_json();
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "GET") == 0 && strcmp(request->path, "/mtsp/launch-shape") == 0) {
        body = ms_mtsp_launch_shape_json(request->query);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "GET") == 0 && strcmp(request->path, "/mtsp/pipelines") == 0) {
        body = ms_mtsp_pipelines_json(request->query);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(request->path, "/cache/clear") == 0) {
        body = ms_cache_clear_json(context->metalsharp_home, request->body, request->body_length);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "GET") == 0 && strcmp(request->path, "/cache/size") == 0) {
        body = ms_cache_size_json(context->metalsharp_home);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "GET") == 0 && strcmp(request->path, "/logs") == 0) {
        body = ms_logs_json(context->metalsharp_home);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "GET") == 0 && strcmp(request->path, "/logs/stream") == 0) {
        body = ms_logs_stream_json(context->metalsharp_home, request->query);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "GET") == 0 && strcmp(request->path, "/logs/crash-reports") == 0) {
        body = ms_crash_reports_json(context->metalsharp_home);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "GET") == 0 && strcmp(request->path, "/update/check") == 0) {
        body = ms_update_check_json();
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(request->path, "/update/start") == 0) {
        body = ms_update_start_json(context->metalsharp_home);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "GET") == 0 && strcmp(request->path, "/update/progress") == 0) {
        body = ms_update_progress_json(context->metalsharp_home);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "GET") == 0 && strcmp(request->path, "/update/migrate/check") == 0) {
        body = ms_migration_check_json(context->metalsharp_home);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "GET") == 0 && strcmp(request->path, "/update/migrate/progress") == 0) {
        body = ms_migration_progress_json(context->metalsharp_home);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "GET") == 0 && strcmp(request->path, "/update/migrate/report") == 0) {
        body = ms_migration_report_json(context->metalsharp_home);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(request->path, "/update/migrate/start") == 0) {
        body = ms_migration_start_json(context->metalsharp_home);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "GET") == 0 && strcmp(request->path, "/update/dmg-path") == 0) {
        body = ms_update_dmg_path_json(context->metalsharp_home);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(request->path, "/update/cleanup") == 0) {
        body = ms_update_cleanup_json(context->metalsharp_home);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "GET") == 0 && strcmp(request->path, "/scan") == 0) {
        ms_log_event(context->metalsharp_home, "Scanning for installed games...");
        body = ms_scan_all_json(context->metalsharp_home);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "GET") == 0 && strcmp(request->path, "/steam/status") == 0) {
        body = ms_steam_status_json(context->metalsharp_home);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "GET") == 0 && strcmp(request->path, "/steam/library") == 0) {
        ms_log_event(context->metalsharp_home, "Loading Steam library...");
        body = ms_steam_library_json(context->metalsharp_home);
        if (body == NULL)
            return false;
        ms_log_event(context->metalsharp_home, "Loaded 0 games");
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "GET") == 0 && strcmp(request->path, "/steam/api-key") == 0) {
        body = ms_steam_api_key_json(context->metalsharp_home);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "GET") == 0 && strcmp(request->path, "/steam/is-running") == 0) {
        body = ms_steam_is_running_json(context->metalsharp_home);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "GET") == 0 && strcmp(request->path, "/steam/bridge-status") == 0) {
        body = ms_steam_bridge_status_json(context->metalsharp_home);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "GET") == 0 && strcmp(request->path, "/steam/watch-steamapps") == 0) {
        body = ms_steam_watch_json(context->metalsharp_home);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "GET") == 0 && strcmp(request->path, "/mtsp/pipelines") == 0) {
        body = ms_mtsp_pipelines_json(request->query);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "GET") == 0 && strcmp(request->path, "/mtsp/default-rules") == 0) {
        body = ms_mtsp_default_rules_json();
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "GET") == 0 && strcmp(request->path, "/mtsp/launch-shape") == 0) {
        body = ms_mtsp_launch_shape_json(request->query);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(request->path, "/mtsp/prepare") == 0) {
        int status = 500;
        body = ms_mtsp_prepare_json(request->body, request->body_length, &status);
        if (body == NULL)
            return false;
        set_json_response(response, status, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(request->path, "/mtsp/recipe") == 0) {
        int status = 500;
        body = ms_mtsp_recipe_json(request->body, request->body_length, &status);
        if (body == NULL)
            return false;
        set_json_response(response, status, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(request->path, "/mtsp/doctor") == 0) {
        int status = 500;
        body = ms_mtsp_doctor_json(request->body, request->body_length, &status);
        if (body == NULL)
            return false;
        set_json_response(response, status, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(request->path, "/game/resolve-routing") == 0) {
        int status = 500;
        body = ms_game_resolve_json(context->metalsharp_home, request->body, request->body_length, &status);
        if (body == NULL)
            return false;
        set_json_response(response, status, body);
        return true;
    }
    if (strcmp(request->method, "GET") == 0 && strcmp(request->path, "/game/dual-info") == 0) {
        int status = 500;
        body = ms_game_dual_json(context->metalsharp_home, request->query, &status);
        if (body == NULL)
            return false;
        set_json_response(response, status, body);
        return true;
    }
    if (strcmp(request->method, "GET") == 0 && strcmp(request->path, "/goldberg/status") == 0) {
        int status = 500;
        body = ms_goldberg_json(context->metalsharp_home, request->query, &status);
        if (body == NULL)
            return false;
        set_json_response(response, status, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(request->path, "/goldberg/toggle") == 0) {
        int status = 500;
        body = ms_goldberg_toggle_json(context->metalsharp_home, request->body, request->body_length, &status);
        if (body == NULL)
            return false;
        set_json_response(response, status, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 &&
        strcmp(request->path, "/kernel-translation/anti-debug/simulate-check") == 0) {
        int status = 500;
        body = ms_antidebug_json("simulate-check", request->body, request->body_length, &status);
        if (body == NULL)
            return false;
        set_json_response(response, status, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 &&
        strcmp(request->path, "/kernel-translation/anti-debug/run-all-checks") == 0) {
        int status = 500;
        body = ms_antidebug_json("run-all-checks", request->body, request->body_length, &status);
        if (body == NULL)
            return false;
        set_json_response(response, status, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 &&
        strcmp(request->path, "/kernel-translation/anti-debug/check-results") == 0) {
        int status = 500;
        body = ms_antidebug_json("check-results", request->body, request->body_length, &status);
        if (body == NULL)
            return false;
        set_json_response(response, status, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 &&
        strcmp(request->path, "/kernel-translation/anti-debug/hw-breakpoint-map") == 0) {
        int status = 500;
        body = ms_antidebug_json("hw-breakpoint-map", request->body, request->body_length, &status);
        if (body == NULL)
            return false;
        set_json_response(response, status, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 &&
        strcmp(request->path, "/kernel-translation/anti-debug/full-breakpoint-map") == 0) {
        int status = 500;
        body = ms_antidebug_json("full-breakpoint-map", request->body, request->body_length, &status);
        if (body == NULL)
            return false;
        set_json_response(response, status, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 &&
        strcmp(request->path, "/kernel-translation/anti-debug/module-sanitize") == 0) {
        int status = 500;
        body = ms_antidebug_json("module-sanitize", request->body, request->body_length, &status);
        if (body == NULL)
            return false;
        set_json_response(response, status, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 &&
        strcmp(request->path, "/kernel-translation/anti-debug/add-sanitize-rule") == 0) {
        int status = 500;
        body = ms_antidebug_json("add-sanitize-rule", request->body, request->body_length, &status);
        if (body == NULL)
            return false;
        set_json_response(response, status, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 &&
        strcmp(request->path, "/kernel-translation/anti-debug/timing-analysis") == 0) {
        int status = 500;
        body = ms_antidebug_json("timing-analysis", request->body, request->body_length, &status);
        if (body == NULL)
            return false;
        set_json_response(response, status, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 &&
        strcmp(request->path, "/kernel-translation/anti-debug/filesystem-check") == 0) {
        int status = 500;
        body = ms_antidebug_json("filesystem-check", request->body, request->body_length, &status);
        if (body == NULL)
            return false;
        set_json_response(response, status, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 &&
        strcmp(request->path, "/kernel-translation/anti-debug/status-survey") == 0) {
        int status = 500;
        body = ms_antidebug_json("status-survey", request->body, request->body_length, &status);
        if (body == NULL)
            return false;
        set_json_response(response, status, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 &&
        strcmp(request->path, "/kernel-translation/anti-debug/seed-demo") == 0) {
        int status = 500;
        body = ms_antidebug_json("seed-demo", request->body, request->body_length, &status);
        if (body == NULL)
            return false;
        set_json_response(response, status, body);
        return true;
    }
    if (strcmp(request->method, "GET") == 0 && strcmp(request->path, "/diagnostics/cache-doctor") == 0) {
        int status = 500;
        body = ms_diagnostics_json("cache-doctor", request->query, request->body, request->body_length, &status);
        if (body == NULL)
            return false;
        set_json_response(response, status, body);
        return true;
    }
    if (strcmp(request->method, "GET") == 0 && strcmp(request->path, "/diagnostics/fna/classify") == 0) {
        int status = 500;
        body = ms_diagnostics_json("fna-classify", request->query, request->body, request->body_length, &status);
        if (body == NULL)
            return false;
        set_json_response(response, status, body);
        return true;
    }
    if (strcmp(request->method, "GET") == 0 && strcmp(request->path, "/diagnostics/fna/explain") == 0) {
        int status = 500;
        body = ms_diagnostics_json("fna-explain", request->query, request->body, request->body_length, &status);
        if (body == NULL)
            return false;
        set_json_response(response, status, body);
        return true;
    }
    if (strcmp(request->method, "GET") == 0 && strcmp(request->path, "/diagnostics/fna/signals") == 0) {
        int status = 500;
        body = ms_diagnostics_json("fna-signals", request->query, request->body, request->body_length, &status);
        if (body == NULL)
            return false;
        set_json_response(response, status, body);
        return true;
    }
    if (strcmp(request->method, "GET") == 0 && strcmp(request->path, "/diagnostics/launch") == 0) {
        int status = 500;
        body = ms_diagnostics_json("launch", request->query, request->body, request->body_length, &status);
        if (body == NULL)
            return false;
        set_json_response(response, status, body);
        return true;
    }
    if (strcmp(request->method, "GET") == 0 && strcmp(request->path, "/diagnostics/launch/timing") == 0) {
        int status = 500;
        body = ms_diagnostics_json("launch-timing", request->query, request->body, request->body_length, &status);
        if (body == NULL)
            return false;
        set_json_response(response, status, body);
        return true;
    }
    if (strcmp(request->method, "GET") == 0 && strcmp(request->path, "/diagnostics/m12/dry-run") == 0) {
        int status = 500;
        body = ms_diagnostics_json("m12-dry-run", request->query, request->body, request->body_length, &status);
        if (body == NULL)
            return false;
        set_json_response(response, status, body);
        return true;
    }
    if (strcmp(request->method, "GET") == 0 && strcmp(request->path, "/diagnostics/pipeline/dry-run") == 0) {
        int status = 500;
        body = ms_diagnostics_json("pipeline-dry-run", request->query, request->body, request->body_length, &status);
        if (body == NULL)
            return false;
        set_json_response(response, status, body);
        return true;
    }
    if (strcmp(request->method, "GET") == 0 && strcmp(request->path, "/diagnostics/pso-manifests") == 0) {
        int status = 500;
        body = ms_diagnostics_json("pso-manifests", request->query, request->body, request->body_length, &status);
        if (body == NULL)
            return false;
        set_json_response(response, status, body);
        return true;
    }
    if (strcmp(request->method, "GET") == 0 && strcmp(request->path, "/diagnostics/runtime-artifacts") == 0) {
        int status = 500;
        body = ms_diagnostics_json("runtime-artifacts", request->query, request->body, request->body_length, &status);
        if (body == NULL)
            return false;
        set_json_response(response, status, body);
        return true;
    }
    if (strcmp(request->method, "GET") == 0 && strcmp(request->path, "/diagnostics/wineboot-state") == 0) {
        int status = 500;
        body = ms_diagnostics_json("wineboot-state", request->query, request->body, request->body_length, &status);
        if (body == NULL)
            return false;
        set_json_response(response, status, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(request->path, "/diagnostics/binding-contract/validate") == 0) {
        int status = 500;
        body = ms_diagnostics_json("binding-contract", request->query, request->body, request->body_length, &status);
        if (body == NULL)
            return false;
        set_json_response(response, status, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(request->path, "/diagnostics/command-replay/validate") == 0) {
        int status = 500;
        body = ms_diagnostics_json("command-replay", request->query, request->body, request->body_length, &status);
        if (body == NULL)
            return false;
        set_json_response(response, status, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(request->path, "/launcher/evidence") == 0) {
        int status = 500;
        body = ms_diagnostics_json("launcher-evidence", request->query, request->body, request->body_length, &status);
        if (body == NULL)
            return false;
        set_json_response(response, status, body);
        return true;
    }
    if (strcmp(request->method, "GET") == 0 && strcmp(request->path, "/emulators") == 0) {
        body = ms_emulators_json(context->metalsharp_home);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "GET") == 0 && strcmp(request->path, "/sharp-library/rpcs3/status") == 0) {
        body = ms_rpcs3_status_json(context->metalsharp_home);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "GET") == 0 && strcmp(request->path, "/sharp-library/rpcs3/games") == 0) {
        body = ms_rpcs3_games_json(context->metalsharp_home);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "GET") == 0 && strcmp(request->path, "/sharp-library/rpcs3/update/check") == 0) {
        body = ms_rpcs3_update_json(context->metalsharp_home, "check");
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(request->path, "/sharp-library/rpcs3/update/refresh") == 0) {
        body = ms_rpcs3_update_json(context->metalsharp_home, "refresh");
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "GET") == 0 && strcmp(request->path, "/sharp-library/rpcs3/update/progress") == 0) {
        body = ms_rpcs3_update_json(context->metalsharp_home, "progress");
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(request->path, "/sharp-library/rpcs3/update/install") == 0) {
        body = ms_rpcs3_update_json(context->metalsharp_home, "install");
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(request->path, "/sharp-library/rpcs3/update/rollback") == 0) {
        body = ms_rpcs3_update_json(context->metalsharp_home, "rollback");
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strncmp(request->path, "/sharp-library/rpcs3/", 21) == 0) {
        const char* action = request->path + 21;
        if (!strcmp(action, "scan") || !strcmp(action, "add-root") || !strcmp(action, "remove-root") ||
            !strcmp(action, "launch") || !strcmp(action, "stop") || !strcmp(action, "open-ui") ||
            !strcmp(action, "install-firmware") || !strcmp(action, "install-package") ||
            !strcmp(action, "remove-runtime") || !strcmp(action, "pin-current") || !strcmp(action, "unpin") ||
            !strcmp(action, "skip-update") || !strcmp(action, "clear-skip")) {
            body = ms_rpcs3_action_json(context->metalsharp_home, action, request->body, request->body_length);
            if (body == NULL)
                return false;
            set_json_response(response, 200, body);
            return true;
        }
    }
    if (strcmp(request->method, "GET") == 0 && strcmp(request->path, "/sharp-library/shadps4/status") == 0) {
        body = ms_shadps4_status_json(context->metalsharp_home);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "GET") == 0 && strcmp(request->path, "/sharp-library/shadps4/games") == 0) {
        body = ms_shadps4_games_json(context->metalsharp_home);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "GET") == 0 && strcmp(request->path, "/sharp-library/shadps4/update/check") == 0) {
        body = ms_shadps4_update_json(context->metalsharp_home, "check");
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(request->path, "/sharp-library/shadps4/update/refresh") == 0) {
        body = ms_shadps4_update_json(context->metalsharp_home, "refresh");
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "GET") == 0 && strcmp(request->path, "/sharp-library/shadps4/update/progress") == 0) {
        body = ms_shadps4_update_json(context->metalsharp_home, "progress");
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(request->path, "/sharp-library/shadps4/update/install") == 0) {
        body = ms_shadps4_update_json(context->metalsharp_home, "install");
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(request->path, "/sharp-library/shadps4/update/rollback") == 0) {
        body = ms_shadps4_update_json(context->metalsharp_home, "rollback");
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strncmp(request->path, "/sharp-library/shadps4/", 23) == 0) {
        const char* action = request->path + 23;
        if (!strcmp(action, "scan") || !strcmp(action, "add-root") || !strcmp(action, "remove-root") ||
            !strcmp(action, "import-modules") || !strcmp(action, "import-fonts") || !strcmp(action, "launch") ||
            !strcmp(action, "stop") || !strcmp(action, "remove-runtime") || !strcmp(action, "pin-current") ||
            !strcmp(action, "unpin") || !strcmp(action, "skip-update") || !strcmp(action, "clear-skip")) {
            body = ms_shadps4_action_json(context->metalsharp_home, action, request->body, request->body_length);
            if (body == NULL)
                return false;
            set_json_response(response, 200, body);
            return true;
        }
    }
    if (strcmp(request->method, "GET") == 0 && strcmp(request->path, "/sharp-library/sharpemu/status") == 0) {
        body = ms_sharpemu_status_json(context->metalsharp_home);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "GET") == 0 && strcmp(request->path, "/sharp-library/sharpemu/games") == 0) {
        body = ms_sharpemu_games_json(context->metalsharp_home);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "GET") == 0 && strcmp(request->path, "/sharp-library/sharpemu/sessions") == 0) {
        body = ms_sharpemu_sessions_json(context->metalsharp_home);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "GET") == 0 && strcmp(request->path, "/sharp-library/sharpemu/update/check") == 0) {
        body = ms_sharpemu_update_json(context->metalsharp_home, "check");
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(request->path, "/sharp-library/sharpemu/update/refresh") == 0) {
        body = ms_sharpemu_update_json(context->metalsharp_home, "refresh");
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "GET") == 0 && strcmp(request->path, "/sharp-library/sharpemu/update/progress") == 0) {
        body = ms_sharpemu_update_json(context->metalsharp_home, "progress");
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(request->path, "/sharp-library/sharpemu/update/install") == 0) {
        body = ms_sharpemu_update_json(context->metalsharp_home, "install");
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(request->path, "/sharp-library/sharpemu/update/rollback") == 0) {
        body = ms_sharpemu_update_json(context->metalsharp_home, "rollback");
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strncmp(request->path, "/sharp-library/sharpemu/", 24) == 0) {
        const char* action = request->path + 24;
        if (!strcmp(action, "scan") || !strcmp(action, "add-root") || !strcmp(action, "remove-root") ||
            !strcmp(action, "import-modules") || !strcmp(action, "import-fonts") || !strcmp(action, "launch") ||
            !strcmp(action, "stop") || !strcmp(action, "remove-runtime") || !strcmp(action, "pin-current") ||
            !strcmp(action, "unpin") || !strcmp(action, "skip-update") || !strcmp(action, "clear-skip")) {
            body = ms_sharpemu_action_json(context->metalsharp_home, action, request->body, request->body_length);
            if (body == NULL)
                return false;
            set_json_response(response, 200, body);
            return true;
        }
    }
    if (strcmp(request->method, "GET") == 0 && strcmp(request->path, "/sharp-library/pcsx2/status") == 0) {
        body = ms_pcsx2_status_json(context->metalsharp_home);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "GET") == 0 && strcmp(request->path, "/sharp-library/pcsx2/games") == 0) {
        body = ms_pcsx2_games_json(context->metalsharp_home);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "GET") == 0 && strcmp(request->path, "/sharp-library/pcsx2/settings") == 0) {
        body = ms_pcsx2_settings_json(context->metalsharp_home);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "GET") == 0 && strcmp(request->path, "/sharp-library/pcsx2/update/check") == 0) {
        body = ms_pcsx2_update_json(context->metalsharp_home, "check");
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(request->path, "/sharp-library/pcsx2/update/refresh") == 0) {
        body = ms_pcsx2_update_json(context->metalsharp_home, "refresh");
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "GET") == 0 && strcmp(request->path, "/sharp-library/pcsx2/update/progress") == 0) {
        body = ms_pcsx2_update_json(context->metalsharp_home, "progress");
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(request->path, "/sharp-library/pcsx2/update/install") == 0) {
        body = ms_pcsx2_update_json(context->metalsharp_home, "install");
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(request->path, "/sharp-library/pcsx2/update/rollback") == 0) {
        body = ms_pcsx2_update_json(context->metalsharp_home, "rollback");
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strncmp(request->path, "/sharp-library/pcsx2/", 21) == 0) {
        const char* action = request->path + 21;
        if (!strcmp(action, "initialize") || !strcmp(action, "configure") || !strcmp(action, "scan") ||
            !strcmp(action, "add-root") || !strcmp(action, "remove-root") || !strcmp(action, "import-bios") ||
            !strcmp(action, "launch") || !strcmp(action, "stop") || !strcmp(action, "open-ui") ||
            !strcmp(action, "open-setup") || !strcmp(action, "remove-runtime") || !strcmp(action, "pin-current") ||
            !strcmp(action, "unpin") || !strcmp(action, "skip-update") || !strcmp(action, "clear-skip")) {
            body = ms_pcsx2_action_json(context->metalsharp_home, action, request->body, request->body_length);
            if (body == NULL)
                return false;
            set_json_response(response, 200, body);
            return true;
        }
    }
    if (strcmp(request->method, "GET") == 0 && strcmp(request->path, "/sharp-library/gog/status") == 0) {
        body = ms_gog_status_json(context->metalsharp_home);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "GET") == 0 && strcmp(request->path, "/sharp-library/gog/games") == 0) {
        body = ms_gog_games_json(context->metalsharp_home);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(request->path, "/sharp-library/gog/initialize-prefix") == 0) {
        body = ms_gog_action_json(context->metalsharp_home, "initialize-prefix", request->body, request->body_length);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(request->path, "/sharp-library/gog/auth-code") == 0) {
        body = ms_gog_action_json(context->metalsharp_home, "auth-code", request->body, request->body_length);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(request->path, "/sharp-library/gog/logout") == 0) {
        body = ms_gog_action_json(context->metalsharp_home, "logout", request->body, request->body_length);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(request->path, "/sharp-library/gog/remove-prefix") == 0) {
        body = ms_gog_action_json(context->metalsharp_home, "remove-prefix", request->body, request->body_length);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(request->path, "/sharp-library/gog/sync") == 0) {
        body = ms_gog_action_json(context->metalsharp_home, "sync", request->body, request->body_length);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(request->path, "/sharp-library/gog/install") == 0) {
        body = ms_gog_action_json(context->metalsharp_home, "install", request->body, request->body_length);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(request->path, "/sharp-library/gog/import") == 0) {
        body = ms_gog_action_json(context->metalsharp_home, "import", request->body, request->body_length);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(request->path, "/sharp-library/gog/progress") == 0) {
        body = ms_gog_action_json(context->metalsharp_home, "progress", request->body, request->body_length);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(request->path, "/sharp-library/gog/play") == 0) {
        body = ms_gog_action_json(context->metalsharp_home, "play", request->body, request->body_length);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(request->path, "/sharp-library/gog/stop") == 0) {
        body = ms_gog_action_json(context->metalsharp_home, "stop", request->body, request->body_length);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(request->path, "/sharp-library/gog/uninstall") == 0) {
        body = ms_gog_action_json(context->metalsharp_home, "uninstall", request->body, request->body_length);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "GET") == 0 && strcmp(request->path, "/gamejolt/storage") == 0) {
        body = ms_gamejolt_storage_json(context->metalsharp_home);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(request->path, "/gamejolt/storage") == 0) {
        body = ms_gamejolt_set_storage_json(context->metalsharp_home, request->body, request->body_length);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(request->path, "/gamejolt/name") == 0) {
        body = ms_gamejolt_set_name_json(context->metalsharp_home, request->body, request->body_length);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(request->path, "/gamejolt/engine") == 0) {
        body = ms_gamejolt_set_engine_json(context->metalsharp_home, request->body, request->body_length);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(request->path, "/gamejolt/uninstall") == 0) {
        body = ms_gamejolt_uninstall_json(context->metalsharp_home, request->body, request->body_length);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "GET") == 0 && strcmp(request->path, "/gamejolt") == 0) {
        body = ms_gamejolt_json(context->metalsharp_home);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(request->path, "/gamejolt/sync") == 0) {
        body = ms_gamejolt_json(context->metalsharp_home);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(request->path, "/gamejolt/launch") == 0) {
        body = ms_gamejolt_launch_json(context->metalsharp_home, request->body, request->body_length);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(request->path, "/gamejolt/status") == 0) {
        body = ms_gamejolt_pid_status_json(request->body, request->body_length);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "GET") == 0 && strcmp(request->path, "/gamejolt/cover") == 0) {
        const char* query = request->query == NULL ? "" : request->query;
        const char* q = strstr(query, "id=");
        char id[129];
        size_t id_len = 0, bytes = 0;
        char* cover_path;
        unsigned char* image;
        if (!q) {
            body = strdup("{\"ok\":false,\"error\":\"cover not found\"}");
            if (body == NULL)
                return false;
            set_json_response(response, 404, body);
            return true;
        }
        q += 3;
        while (q[id_len] && q[id_len] != '&' && id_len < sizeof(id) - 1)
            id_len++;
        memcpy(id, q, id_len);
        id[id_len] = '\0';
        cover_path = ms_gamejolt_cover_path(context->metalsharp_home, id);
        image = cover_path ? read_binary(cover_path, &bytes) : NULL;
        if (!image) {
            free(cover_path);
            body = strdup("{\"ok\":false,\"error\":\"cover not found\"}");
            if (body == NULL)
                return false;
            set_json_response(response, 404, body);
            return true;
        }
        response->status = 200;
        response->content_type = image_type(cover_path);
        response->body = image;
        response->body_length = bytes;
        response->owns_body = true;
        free(cover_path);
        return true;
    }
    if (strcmp(request->method, "GET") == 0 && (strcmp(request->path, "/sharp-library/rpcs3/cover") == 0 ||
                                                strcmp(request->path, "/sharp-library/shadps4/cover") == 0 ||
                                                strcmp(request->path, "/sharp-library/sharpemu/cover") == 0 ||
                                                strcmp(request->path, "/sharp-library/pcsx2/cover") == 0)) {
        const char* query = request->query == NULL ? "" : request->query;
        const char* q = strstr(query, "id=");
        char id[129];
        size_t id_len = 0, bytes = 0;
        char* cover_path;
        unsigned char* image;
        if (!q) {
            body = strdup("{\"ok\":false,\"error\":\"cover not found\"}");
            if (body == NULL)
                return false;
            set_json_response(response, 404, body);
            return true;
        }
        q += 3;
        while (q[id_len] && q[id_len] != '&' && id_len < sizeof(id) - 1)
            id_len++;
        memcpy(id, q, id_len);
        id[id_len] = '\0';
        if (strcmp(request->path, "/sharp-library/shadps4/cover") == 0)
            cover_path = ms_shadps4_cover_path(context->metalsharp_home, id);
        else if (strcmp(request->path, "/sharp-library/sharpemu/cover") == 0)
            cover_path = ms_sharpemu_cover_path(context->metalsharp_home, id);
        else if (strcmp(request->path, "/sharp-library/pcsx2/cover") == 0)
            cover_path = ms_pcsx2_cover_path(context->metalsharp_home, id);
        else
            cover_path = ms_rpcs3_cover_path(context->metalsharp_home, id);
        image = cover_path ? read_binary(cover_path, &bytes) : NULL;
        if (!image) {
            free(cover_path);
            body = strdup("{\"ok\":false,\"error\":\"cover not found\"}");
            if (body == NULL)
                return false;
            set_json_response(response, 404, body);
            return true;
        }
        response->status = 200;
        response->content_type = image_type(cover_path);
        response->body = image;
        response->body_length = bytes;
        response->owns_body = true;
        free(cover_path);
        return true;
    }
    if (strcmp(request->method, "GET") == 0 && strcmp(request->path, "/sharp-library/cover") == 0) {
        const char* query = request->query == NULL ? "" : request->query;
        const char* q = strstr(query, "id=");
        char id[129];
        size_t id_len = 0, bytes = 0;
        char* cover_path;
        unsigned char* image;
        if (!q) {
            body = strdup("{\"ok\":false,\"error\":\"cover not found\"}");
            if (body == NULL)
                return false;
            set_json_response(response, 404, body);
            return true;
        }
        q += 3;
        while (q[id_len] && q[id_len] != '&' && id_len < sizeof(id) - 1)
            id_len++;
        memcpy(id, q, id_len);
        id[id_len] = '\0';
        cover_path = ms_sharp_cover_path(context->metalsharp_home, id);
        image = cover_path ? read_binary(cover_path, &bytes) : NULL;
        if (!image) {
            free(cover_path);
            body = strdup("{\"ok\":false,\"error\":\"cover not found\"}");
            if (body == NULL)
                return false;
            set_json_response(response, 404, body);
            return true;
        }
        response->status = 200;
        response->content_type = image_type(cover_path);
        response->body = image;
        response->body_length = bytes;
        response->owns_body = true;
        free(cover_path);
        return true;
    }
    if (strcmp(request->method, "GET") == 0 && strcmp(request->path, "/sharp-library") == 0) {
        body = ms_sharp_library_json(context->metalsharp_home);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(request->path, "/sharp-library/install") == 0) {
        body = ms_sharp_action_json(context->metalsharp_home, request->body, request->body_length, "install");
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "GET") == 0 && strcmp(request->path, "/sharp-library/epic/status") == 0) {
        body = ms_epic_status_json(context->metalsharp_home);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "GET") == 0 && strcmp(request->path, "/sharp-library/epic/games") == 0) {
        body = ms_epic_games_json(context->metalsharp_home, 0);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(request->path, "/sharp-library/epic/install-tool") == 0) {
        body = ms_epic_install_tool_json(context->metalsharp_home);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(request->path, "/sharp-library/epic/auth") == 0) {
        body = ms_epic_auth_json(context->metalsharp_home, request->body, request->body_length);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(request->path, "/sharp-library/epic/logout") == 0) {
        body = ms_epic_logout_json(context->metalsharp_home);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(request->path, "/sharp-library/epic/sync") == 0) {
        body = ms_epic_games_json(context->metalsharp_home, 1);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(request->path, "/sharp-library/epic/install") == 0) {
        body = ms_epic_install_json(context->metalsharp_home, request->body, request->body_length);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(request->path, "/sharp-library/epic/progress") == 0) {
        body = ms_epic_progress_json(context->metalsharp_home, request->body, request->body_length);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(request->path, "/sharp-library/epic/cancel") == 0) {
        body = ms_epic_cancel_json(context->metalsharp_home, request->body, request->body_length);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(request->path, "/sharp-library/epic/initialize") == 0) {
        body = ms_epic_initialize_json(context->metalsharp_home, request->body, request->body_length);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(request->path, "/sharp-library/epic/play") == 0) {
        body = ms_epic_launch_json(context->metalsharp_home, request->body, request->body_length);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(request->path, "/sharp-library/epic/stop") == 0) {
        body = ms_epic_stop_json(context->metalsharp_home, request->body, request->body_length);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(request->path, "/sharp-library/epic/stop-all") == 0) {
        body = ms_epic_stop_all_json(context->metalsharp_home);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(request->path, "/sharp-library/epic/uninstall") == 0) {
        body = ms_epic_uninstall_json(context->metalsharp_home, request->body, request->body_length);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(request->path, "/sharp-library/launchers/install") == 0) {
        body = ms_sharp_launcher_install_json(context->metalsharp_home, request->body, request->body_length);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "GET") == 0 && strcmp(request->path, "/sharp-library/launchers/status") == 0) {
        body = ms_sharp_launcher_status_json(context->metalsharp_home);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(request->path, "/sharp-library/launchers/launch") == 0) {
        body = ms_sharp_launcher_launch_json(context->metalsharp_home, request->body, request->body_length);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(request->path, "/sharp-library/import-bottle-app") == 0) {
        body = ms_sharp_action_json(context->metalsharp_home, request->body, request->body_length, "import");
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(request->path, "/bottles/relaunch-installer") == 0) {
        body = ms_bottle_relaunch_installer_json(context->metalsharp_home, request->body, request->body_length, NULL);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(request->path, "/sharp-library/uninstall") == 0) {
        body = ms_sharp_action_json(context->metalsharp_home, request->body, request->body_length, "uninstall");
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(request->path, "/sharp-library/launch") == 0) {
        body = ms_sharp_action_json(context->metalsharp_home, request->body, request->body_length, "launch");
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(request->path, "/sharp-library/doctor") == 0) {
        body = ms_sharp_action_json(context->metalsharp_home, request->body, request->body_length, "doctor");
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(request->path, "/sharp-library/set-cover") == 0) {
        body = ms_sharp_action_json(context->metalsharp_home, request->body, request->body_length, "set-cover");
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(request->path, "/sharp-library/set-cover-position") == 0) {
        body =
            ms_sharp_action_json(context->metalsharp_home, request->body, request->body_length, "set-cover-position");
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(request->path, "/sharp-library/set-launch-args") == 0) {
        body = ms_sharp_action_json(context->metalsharp_home, request->body, request->body_length, "set-launch-args");
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(request->path, "/sharp-library/set-engine") == 0) {
        body = ms_sharp_action_json(context->metalsharp_home, request->body, request->body_length, "set-engine");
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "GET") == 0 && strcmp(request->path, "/wine-mono/status") == 0) {
        const char* kind = strstr(request->query, "prefix=steam") != NULL ? "steam" : "gog";
        body = ms_mono_status_json(context->metalsharp_home, kind);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(request->path, "/wine-mono/install") == 0) {
        body = ms_mono_install_json(context->metalsharp_home, (const char*)request->body, request->body_length);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(request->path, "/wine-mono/reset") == 0) {
        body = ms_mono_reset_json(context->metalsharp_home, (const char*)request->body, request->body_length);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "GET") == 0 && strcmp(request->path, "/bottles") == 0) {
        ms_bottle_poll();
        body = ms_bottles_list_json(context->metalsharp_home);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "GET") == 0 && strcmp(request->path, "/bottles/profiles") == 0) {
        body = ms_bottles_profiles_json();
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "GET") == 0 && strcmp(request->path, "/bottles/compatibility-matrix") == 0) {
        body = ms_bottles_matrix_json(context->metalsharp_home);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "GET") == 0 && strcmp(request->path, "/bottles/redist-sources") == 0) {
        body = ms_bottles_redist_json(context->metalsharp_home);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "GET") == 0 && strcmp(request->path, "/bottles/route-contracts") == 0) {
        body = ms_bottles_contracts_json();
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(request->path, "/steam/install") == 0) {
        int status = 500;
        body = ms_steam_install_json(context->metalsharp_home, &status);
        if (body == NULL)
            return false;
        set_json_response(response, status, body);
        return true;
    }
    if (strcmp(request->method, "GET") == 0 && strcmp(request->path, "/steam/stop-targets") == 0) {
        int status = 500;
        body = ms_steam_stop_targets_json(context->metalsharp_home, &status);
        if (body == NULL)
            return false;
        set_json_response(response, status, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(request->path, "/steam/bridge-start") == 0) {
        int status = 500;
        body = ms_steam_misc_json("bridge-start", request->body, request->body_length, &status);
        if (body == NULL)
            return false;
        set_json_response(response, status, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(request->path, "/steam/compatdata") == 0) {
        int status = 500;
        body = ms_steam_misc_json("compatdata", request->body, request->body_length, &status);
        if (body == NULL)
            return false;
        set_json_response(response, status, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(request->path, "/steam/install-game") == 0) {
        int status = 500;
        body = ms_steam_install_game_json(context->metalsharp_home, (const char*)request->body, request->body_length,
                                          &status);
        if (body == NULL)
            return false;
        set_json_response(response, status, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(request->path, "/steam/uninstall-game") == 0) {
        int status = 500;
        body = ms_steam_uninstall_game_json(context->metalsharp_home, (const char*)request->body, request->body_length,
                                            &status);
        if (body == NULL)
            return false;
        set_json_response(response, status, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(request->path, "/steam/launch-offline") == 0) {
        int status = 500;
        body = ms_steam_launch_offline_json(context->metalsharp_home, (const char*)request->body, request->body_length,
                                            &status);
        if (body == NULL)
            return false;
        set_json_response(response, status, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(request->path, "/steam/runtime-doctor") == 0) {
        int status = 500;
        body = ms_steam_misc_json("runtime-doctor", request->body, request->body_length, &status);
        if (body == NULL)
            return false;
        set_json_response(response, status, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(request->path, "/steam/d3d12-runtime-doctor") == 0) {
        int status = 500;
        body = ms_steam_misc_json("d3d12-runtime-doctor", request->body, request->body_length, &status);
        if (body == NULL)
            return false;
        set_json_response(response, status, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(request->path, "/steam/launch") == 0) {
        int status = 500;
        ms_log_event(context->metalsharp_home, "Launching Wine Steam...");
        body = ms_steam_launch_json(context->metalsharp_home, &status);
        if (body == NULL)
            return false;
        if (status >= 400)
            ms_issue_log(context->metalsharp_home, "steam-launch", "wine-steam", "MetalSharp Wine not found");
        set_json_response(response, status, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(request->path, "/steam/stop") == 0) {
        int status = 500;
        ms_log_event(context->metalsharp_home, "Stopping Wine Steam...");
        body = ms_steam_stop_json(context->metalsharp_home, &status);
        if (body == NULL)
            return false;
        set_json_response(response, status, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(request->path, "/steam/mac-launch") == 0) {
        int status = 500;
        ms_log_event(context->metalsharp_home, "Launching macOS Steam...");
        body = ms_steam_mac_launch_json(context->metalsharp_home, &status);
        if (body == NULL)
            return false;
        if (status >= 400)
            ms_issue_log(context->metalsharp_home, "steam-launch", "macos-steam", "macOS Steam is not installed");
        set_json_response(response, status, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(request->path, "/steam/mac-install") == 0) {
        int status = 500;
        ms_log_event(context->metalsharp_home, "Opening macOS Steam installer...");
        body = ms_steam_mac_install_json(&status);
        if (body == NULL)
            return false;
        set_json_response(response, status, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(request->path, "/steam/mac-stop") == 0) {
        int status = 500;
        ms_log_event(context->metalsharp_home, "Stopping macOS Steam...");
        body = ms_steam_mac_stop_json(&status);
        if (body == NULL)
            return false;
        set_json_response(response, status, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(request->path, "/steam/launch-game") == 0) {
        int status = 500;
        body = ms_steam_launch_game_json(context->metalsharp_home, (const char*)request->body, request->body_length,
                                         &status);
        if (body == NULL)
            return false;
        set_json_response(response, status, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(request->path, "/steam/mac-launch-game") == 0) {
        int status = 500;
        body = ms_steam_mac_launch_game_json(context->metalsharp_home, (const char*)request->body, request->body_length,
                                             &status);
        if (body == NULL)
            return false;
        set_json_response(response, status, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(request->path, "/steam/view-game") == 0) {
        int status = 500;
        body = ms_steam_view_game_json(context->metalsharp_home, (const char*)request->body, request->body_length,
                                       &status);
        if (body == NULL)
            return false;
        set_json_response(response, status, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(request->path, "/steam/save-api-key") == 0) {
        int status = 500;
        ms_log_event(context->metalsharp_home, "Steam API key saved");
        body = ms_steam_save_api_key_json(context->metalsharp_home, request->body, request->body_length, &status);
        if (body == NULL)
            return false;
        if (status < 400)
            ms_log_event(context->metalsharp_home, "Steam API key sync loaded 0 games");
        set_json_response(response, status, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(request->path, "/game/prepare") == 0) {
        int status = 500;
        body = ms_process_prepare_json(context->metalsharp_home, (const char*)request->body, request->body_length,
                                       &status);
        if (body == NULL)
            return false;
        set_json_response(response, status, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(request->path, "/launch") == 0) {
        int status = 500;
        body =
            ms_process_launch_json(context->metalsharp_home, (const char*)request->body, request->body_length, &status);
        if (body == NULL)
            return false;
        set_json_response(response, status, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(request->path, "/game/launch-auto") == 0) {
        int status = 500;
        body = ms_process_launch_auto_json(context->metalsharp_home, (const char*)request->body, request->body_length,
                                           &status);
        if (body == NULL)
            return false;
        set_json_response(response, status, body);
        return true;
    }
    if (strcmp(request->method, "GET") == 0 && strcmp(request->path, "/game/running") == 0) {
        body = ms_process_running_json();
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(request->path, "/kill") == 0) {
        int status = 500;
        body = ms_process_kill_json((const char*)request->body, request->body_length, &status);
        if (body == NULL)
            return false;
        set_json_response(response, status, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(request->path, "/games/force-quit") == 0) {
        int status = 500;
        body = ms_process_force_quit_json(&status);
        if (body == NULL)
            return false;
        set_json_response(response, status, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(request->path, "/processes/force-kill") == 0) {
        int status = 500;
        body = ms_process_force_kill_json(context->metalsharp_home, &status);
        if (body == NULL)
            return false;
        set_json_response(response, status, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(request->path, "/steam/install-recipe-deps") == 0) {
        int status = 500;
        body = ms_steam_misc_json("install-recipe-deps", request->body, request->body_length, &status);
        if (body == NULL)
            return false;
        set_json_response(response, status, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strncmp(request->path, "/bottles/", 9) == 0) {
        const char* action = strrchr(request->path, '/');
        int status = 200;
        body = ms_bottle_action_json(context->metalsharp_home, action == NULL ? "bottle-action" : action + 1,
                                     request->body, request->body_length, &status);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strncmp(request->path, "/d3dmetal/bottles/", 18) == 0) {
        const char* action = strrchr(request->path, '/');
        int status = 200;
        body = ms_d3dmetal_json(context->metalsharp_home, action == NULL ? "status" : action + 1, request->body,
                                request->body_length, &status);
        if (body == NULL)
            return false;
        set_json_response(response, 200, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strncmp(request->path, "/kernel-translation/integration/", 32) == 0) {
        const char* action = strrchr(request->path, '/');
        int status = 500;
        body = ms_integration_json(action == NULL ? "integration" : action + 1, request->body, request->body_length,
                                   &status);
        if (body == NULL)
            return false;
        set_json_response(response, status, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strncmp(request->path, "/kernel-translation/thread/", 27) == 0) {
        const char* action = strrchr(request->path, '/');
        int status = 500;
        body = ms_thread_json(action == NULL ? "thread" : action + 1, request->body, request->body_length, &status);
        if (body == NULL)
            return false;
        set_json_response(response, status, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(request->path, "/kernel-translation/ipc/start") == 0) {
        int status = 500;
        body = ms_kernel_extra_json("ipc-start", request->body, request->body_length, &status);
        if (body == NULL)
            return false;
        set_json_response(response, status, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strcmp(request->path, "/kernel-translation/ipc/stop") == 0) {
        int status = 500;
        body = ms_kernel_extra_json("ipc-stop", request->body, request->body_length, &status);
        if (body == NULL)
            return false;
        set_json_response(response, status, body);
        return true;
    }
    if (strcmp(request->method, "GET") == 0 && strcmp(request->path, "/kernel-translation/ipc/status") == 0) {
        int status = 500;
        body = ms_kernel_extra_json("ipc-status", request->body, request->body_length, &status);
        if (body == NULL)
            return false;
        set_json_response(response, status, body);
        return true;
    }
    if (strcmp(request->method, "GET") == 0 && strcmp(request->path, "/kernel-translation/ipc/handles") == 0) {
        int status = 500;
        body = ms_kernel_extra_json("ipc-handles", request->body, request->body_length, &status);
        if (body == NULL)
            return false;
        set_json_response(response, status, body);
        return true;
    }
    if ((strcmp(request->method, "GET") == 0 || strcmp(request->method, "POST") == 0) &&
        strncmp(request->path, "/kernel-translation/es-live/", 28) == 0) {
        const char* action = strrchr(request->path, '/');
        int status = 500;
        char live_action[64];
        if (action == NULL)
            snprintf(live_action, sizeof(live_action), "live-status");
        else
            snprintf(live_action, sizeof(live_action), "live-%s", action + 1);
        body = ms_es_bridge_json(live_action, request->body, request->body_length, &status);
        if (body == NULL)
            return false;
        set_json_response(response, status, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strncmp(request->path, "/kernel-translation/es/", 23) == 0) {
        const char* action = strrchr(request->path, '/');
        int status = 500;
        body =
            ms_es_bridge_json(action == NULL ? "es-status" : action + 1, request->body, request->body_length, &status);
        if (body == NULL)
            return false;
        set_json_response(response, status, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strncmp(request->path, "/kernel-translation/ob/", 23) == 0) {
        const char* action = strrchr(request->path, '/');
        int status = 500;
        body = ms_ob_callback_json(action == NULL ? "object-callback" : action + 1, request->body, request->body_length,
                                   &status);
        if (body == NULL)
            return false;
        set_json_response(response, status, body);
        return true;
    }
    if (strcmp(request->method, "POST") == 0 && strncmp(request->path, "/kernel-translation/", 20) == 0 &&
        strncmp(request->path, "/kernel-translation/ipc/", 24) != 0) {
        const char* action = strrchr(request->path, '/');
        int status = 500;
        body = ms_kernel_extra_json(action == NULL ? "kernel-translation" : action + 1, request->body,
                                    request->body_length, &status);
        if (body == NULL)
            return false;
        set_json_response(response, status, body);
        return true;
    }
    body = strdup("{\"ok\":false,\"error\":\"not found\"}");
    if (body == NULL)
        return false;
    set_json_response(response, 404, body);
    return true;
}
