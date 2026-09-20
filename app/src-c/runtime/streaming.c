#include "metalsharp_backend/streaming.h"

#include "metalsharp_backend/json.h"
#include "metalsharp_backend/json_writer.h"

#include <CommonCrypto/CommonDigest.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* Game streaming host integration (LizardByte Sunshine). MetalSharp installs,
 * launches, and manages credentials for the Sunshine host on this Mac; the
 * mobile device runs Moonlight (App Store / Play Store) against it. The web
 * API lives on https://localhost:47990 with HTTP basic auth; non-browser
 * clients are exempt from CSRF per Sunshine's API docs. */

#define SUNSHINE_APP_PATH     "/Applications/Sunshine.app"
#define SUNSHINE_BIN_PATH     SUNSHINE_APP_PATH "/Contents/MacOS/Sunshine"
#define SUNSHINE_WEB_BASE     "https://localhost:47990"
#define SUNSHINE_DOWNLOAD_URL "https://github.com/LizardByte/Sunshine/releases/latest/download/Sunshine-macOS-arm64.dmg"
#define STREAMING_CREDS_FILE  "streaming-creds.json"
#define STREAMING_PROGRESS_FILE "streaming_progress.json"
#define STREAMING_PIN_NAME      "MetalSharp Mobile"

/* ------------------------------------------------------------------ */
/* small helpers                                                       */
/* ------------------------------------------------------------------ */

static char* stream_path_join(const char* a, const char* b) {
    size_t x = strlen(a), y = strlen(b);
    bool slash = x > 0 && a[x - 1] != '/';
    char* p = malloc(x + y + (slash ? 2 : 1));
    if (!p)
        return NULL;
    snprintf(p, x + y + (slash ? 2 : 1), "%s%s%s", a, slash ? "/" : "", b);
    return p;
}

static bool stream_file_nonempty(const char* path) {
    struct stat st;
    return path && stat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0;
}

static char* stream_read_file(const char* path, size_t max_bytes) {
    FILE* file = fopen(path, "rb");
    char* buf;
    size_t got;
    if (!file)
        return NULL;
    buf = malloc(max_bytes + 1);
    if (!buf) {
        fclose(file);
        return NULL;
    }
    got = fread(buf, 1, max_bytes, file);
    fclose(file);
    buf[got] = '\0';
    return buf;
}

static char* streaming_error(const char* message) {
    ms_json_writer writer;
    char* out;
    ms_json_writer_init(&writer);
    ms_json_writer_object_begin(&writer);
    ms_json_writer_key(&writer, "ok");
    ms_json_writer_bool(&writer, false);
    ms_json_writer_key(&writer, "error");
    ms_json_writer_string(&writer, message);
    ms_json_writer_object_end(&writer);
    out = ms_json_writer_take(&writer);
    return out;
}

/* fork/exec with stdout captured into out (NUL-terminated). args is a
 * NULL-terminated argv where args[0] is the executable path. Returns true
 * when the child exited 0. */
static bool streaming_exec_capture(char* const args[], char* out, size_t out_size) {
    int pipefd[2];
    pid_t pid;
    bool exited_ok = false;
    sigset_t block, previous;
    if (pipe(pipefd) != 0)
        return false;
    /* Bottle actions installs a process-wide SIGCHLD reaper (waitpid(-1)) and
     * never restores it; block delivery on this thread across fork/waitpid so
     * it cannot reap (and misreport) our short-lived children. */
    sigemptyset(&block);
    sigaddset(&block, SIGCHLD);
    pthread_sigmask(SIG_BLOCK, &block, &previous);
    pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        pthread_sigmask(SIG_SETMASK, &previous, NULL);
        return false;
    }
    if (pid == 0) {
        pthread_sigmask(SIG_SETMASK, &previous, NULL);
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);
        execv(args[0], args);
        _exit(127);
    }
    close(pipefd[1]);
    if (out && out_size > 0) {
        size_t used = 0;
        ssize_t got;
        while ((got = read(pipefd[0], out + used, out_size - 1 - used)) > 0) {
            used += (size_t)got;
            if (used >= out_size - 1)
                break;
        }
        out[used] = '\0';
    } else if (out) {
        out[0] = '\0';
    }
    close(pipefd[0]);
    {
        int status;
        pid_t waited;
        do {
            waited = waitpid(pid, &status, 0);
        } while (waited < 0 && errno == EINTR);
        if (waited < 0 && errno == ECHILD) {
            /* Reaper stole the child despite the mask (delivered on another
             * thread). Exit status unknowable — fail open rather than report
             * a spurious failure for a command that ran. */
            exited_ok = true;
        } else {
            exited_ok = waited == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0;
        }
    }
    pthread_sigmask(SIG_SETMASK, &previous, NULL);
    return exited_ok;
}

/* ------------------------------------------------------------------ */
/* stored web-UI credentials                                           */
/* ------------------------------------------------------------------ */

static bool streaming_read_creds(const char* home, char* user, size_t user_size, char* pass, size_t pass_size) {
    char* path = stream_path_join(home, STREAMING_CREDS_FILE);
    char* text = path ? stream_read_file(path, 4096) : NULL;
    char parse_error[128];
    ms_json* json = NULL;
    bool ok = false;
    free(path);
    if (!text)
        return false;
    json = ms_json_parse(text, strlen(text), parse_error, sizeof(parse_error));
    if (json && ms_json_type_of(json) == MS_JSON_OBJECT) {
        char *u = NULL, *p = NULL;
        const ms_json* value;
        if ((value = ms_json_object_get(json, "username")) != NULL)
            ms_json_as_string(value, &u);
        if ((value = ms_json_object_get(json, "password")) != NULL)
            ms_json_as_string(value, &p);
        if (u && p && u[0] && p[0]) {
            snprintf(user, user_size, "%s", u);
            snprintf(pass, pass_size, "%s", p);
            ok = true;
        }
        free(u);
        free(p);
    }
    ms_json_free(json);
    free(text);
    return ok;
}

static bool streaming_write_creds(const char* home, const char* user, const char* pass) {
    char* path = stream_path_join(home, STREAMING_CREDS_FILE);
    char* quoted_user = ms_json_quote(user);
    char* quoted_pass = ms_json_quote(pass);
    FILE* file;
    bool ok = false;
    if (!path || !quoted_user || !quoted_pass) {
        free(path);
        free(quoted_user);
        free(quoted_pass);
        return false;
    }
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0) {
        file = fdopen(fd, "w");
        if (file) {
            fprintf(file, "{\"username\":%s,\"password\":%s}\n", quoted_user, quoted_pass);
            ok = fclose(file) == 0;
        } else {
            close(fd);
        }
    }
    free(path);
    free(quoted_user);
    free(quoted_pass);
    return ok;
}

static bool streaming_generate_passphrase(char* out, size_t out_size) {
    static const char alphabet[] = "abcdefghjkmnpqrstuvwxyzABCDEFGHJKMNPQRSTUVWXYZ23456789";
    unsigned char raw[32];
    size_t want = out_size - 1 < sizeof(raw) ? out_size - 1 : sizeof(raw);
    int fd = open("/dev/urandom", O_RDONLY);
    size_t i;
    if (fd < 0 || out_size < 2)
        return false;
    if (read(fd, raw, sizeof(raw)) != (ssize_t)sizeof(raw)) {
        close(fd);
        return false;
    }
    close(fd);
    for (i = 0; i < want; i++)
        out[i] = alphabet[raw[i] % (sizeof(alphabet) - 1)];
    out[want] = '\0';
    return true;
}

/* ------------------------------------------------------------------ */
/* Sunshine process / web API helpers                                  */
/* ------------------------------------------------------------------ */

static bool streaming_installed(void) {
    return stream_file_nonempty(SUNSHINE_BIN_PATH) || access(SUNSHINE_APP_PATH "/Contents/Info.plist", F_OK) == 0;
}

/* The web server answers any request (even 401) once it is up. */
static bool streaming_web_up_timeout(const char* max_time) {
    char out[64] = {0};
    char* args[] = {(char*)"curl",
                    (char*)"-sk",
                    (char*)"--max-time",
                    (char*)max_time,
                    (char*)"-o",
                    (char*)"/dev/null",
                    (char*)"-w",
                    (char*)"%{http_code}",
                    (char*)SUNSHINE_WEB_BASE "/api/pin",
                    NULL};
    return streaming_exec_capture(args, out, sizeof(out)) && out[0] != '\0' && strcmp(out, "000") != 0;
}

static bool streaming_web_up(void) {
    return streaming_web_up_timeout("3");
}

/* Authenticated request against the Sunshine web API. json_body may be NULL.
 * The response body is captured in body (bounded) and the HTTP status code in
 * code (via curl's -w, split from the captured stdout). */
static bool streaming_api(const char* user, const char* pass, bool post, const char* endpoint, const char* json_body,
                          char* body, size_t body_size, char* code, size_t code_size) {
    char auth_header[256];
    char code_format[32];
    char* args[20];
    size_t n = 0;
    char combined[8192];
    bool net_ok;
    snprintf(auth_header, sizeof(auth_header), "%s:%s", user, pass);
    snprintf(code_format, sizeof(code_format), "\n%%{http_code}");
    args[n++] = (char*)"curl";
    args[n++] = (char*)"-sk";
    args[n++] = (char*)"-X";
    args[n++] = (char*)(post ? "POST" : "GET");
    args[n++] = (char*)"-u";
    args[n++] = auth_header;
    args[n++] = (char*)"-H";
    args[n++] = (char*)"Content-Type: application/json";
    args[n++] = (char*)"--max-time";
    args[n++] = (char*)(post ? "60" : "6");
    if (json_body) {
        args[n++] = (char*)"--data";
        args[n++] = (char*)json_body;
    }
    args[n++] = (char*)"-w";
    args[n++] = code_format;
    args[n++] = (char*)endpoint;
    args[n] = NULL;
    combined[0] = '\0';
    net_ok = streaming_exec_capture(args, combined, sizeof(combined));
    body[0] = '\0';
    code[0] = '\0';
    if (combined[0]) {
        char* nl = strrchr(combined, '\n');
        if (nl) {
            *nl = '\0';
            snprintf(code, code_size, "%s", nl + 1);
        }
        snprintf(body, body_size, "%s", combined);
    }
    return net_ok;
}

/* ------------------------------------------------------------------ */
/* install worker                                                      */
/* ------------------------------------------------------------------ */

static bool streaming_sha256_file(const char* path, char* out_hex, size_t out_size) {
    FILE* file = fopen(path, "rb");
    CC_SHA256_CTX context;
    unsigned char buffer[8192], digest[CC_SHA256_DIGEST_LENGTH];
    size_t got;
    if (!file || out_size < CC_SHA256_DIGEST_LENGTH * 2 + 1 || CC_SHA256_Init(&context) != 1) {
        if (file)
            fclose(file);
        return false;
    }
    while ((got = fread(buffer, 1, sizeof(buffer), file)) > 0)
        CC_SHA256_Update(&context, buffer, (CC_LONG)got);
    if (ferror(file) || CC_SHA256_Final(digest, &context) != 1) {
        fclose(file);
        return false;
    }
    fclose(file);
    for (size_t i = 0; i < CC_SHA256_DIGEST_LENGTH; i++)
        snprintf(out_hex + i * 2, 3, "%02x", digest[i]);
    out_hex[CC_SHA256_DIGEST_LENGTH * 2] = '\0';
    return true;
}

/* Fetch the expected sha256 for the Sunshine DMG from GitHub's release API
 * (assets[].digest), so a hijacked or mistagged upstream release is caught
 * before anything is installed. */
static bool streaming_expected_dmg_sha256(char* out_hex, size_t out_size) {
    char* response = malloc(512 * 1024);
    char* args[] = {(char*)"curl",
                    (char*)"-sk",
                    (char*)"--max-time",
                    (char*)"20",
                    (char*)"https://api.github.com/repos/LizardByte/Sunshine/releases/latest",
                    NULL};
    char* asset;
    char* digest_field;
    bool ok = false;
    if (!response)
        return false;
    if (!streaming_exec_capture(args, response, 512 * 1024)) {
        free(response);
        return false;
    }
    asset = strstr(response, "\"name\": \"Sunshine-macOS-arm64.dmg\"");
    if (!asset) {
        free(response);
        return false;
    }
    digest_field = strstr(asset, "\"digest\": \"sha256:");
    if (digest_field && digest_field < asset + 4096) {
        char* hex = digest_field + strlen("\"digest\": \"sha256:");
        if (strlen(hex) >= 64 && strspn(hex, "0123456789abcdef") >= 64) {
            snprintf(out_hex, out_size, "%.*s", 64, hex);
            ok = true;
        }
    }
    free(response);
    return ok;
}

static pid_t g_install_pid = 0;
static bool g_install_active = false;

static void streaming_write_progress(const char* home, const char* state, const char* detail);
static bool streaming_progress_terminal(const char* home, bool* stale) {
    char* path = stream_path_join(home, STREAMING_PROGRESS_FILE);
    struct stat st;
    bool terminal = false;
    *stale = false;
    if (path && stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
        char* text = stream_read_file(path, 4096);
        if (text) {
            terminal = strstr(text, "\"status\":\"complete\"") != NULL || strstr(text, "\"status\":\"error\"") != NULL;
            /* A non-terminal progress older than the download cap (20 min) can
             * only come from a worker that died without writing a state. */
            *stale = !terminal && time(NULL) - st.st_mtime > 1800;
            free(text);
        }
    }
    free(path);
    return terminal;
}

static void streaming_refresh_install(const char* home) {
    int wait_status;
    pid_t reaped = g_install_pid > 0 ? waitpid(g_install_pid, &wait_status, WNOHANG) : 0;
    if (g_install_pid > 0 && reaped == g_install_pid) {
        g_install_pid = 0;
        g_install_active = false;
    } else if (g_install_pid > 0 && reaped < 0 && errno == ECHILD) {
        /* The SIGCHLD reaper stole a possibly-still-running worker. The
         * worker writes its terminal state before exiting, so trust the
         * progress file (plus a staleness cap for silent deaths). */
        bool stale = false;
        bool terminal = streaming_progress_terminal(home, &stale);
        g_install_pid = 0;
        g_install_active = !terminal && !stale;
        if (stale)
            streaming_write_progress(home, "error",
                                     "Sunshine install stalled — MetalSharp restarted or the worker was stopped");
    }
}

static void streaming_write_progress(const char* home, const char* state, const char* detail) {
    char* path = stream_path_join(home, STREAMING_PROGRESS_FILE);
    FILE* file;
    if (!path)
        return;
    file = fopen(path, "wb");
    if (file) {
        if (detail)
            fprintf(file, "{\"status\":\"%s\",\"detail\":\"%s\"}", state, detail);
        else
            fprintf(file, "{\"status\":\"%s\",\"detail\":null}", state);
        fclose(file);
    }
    free(path);
}

/* Runs in the detached install worker. */
static void streaming_install_worker(const char* home) {
    char dmg[PATH_MAX];
    char mount[PATH_MAX];
    char app_source[PATH_MAX];
    char out[512];
    char* detach_args[] = {(char*)"/usr/bin/hdiutil", (char*)"detach", mount, (char*)"-force", NULL};

    snprintf(dmg, sizeof(dmg), "%s/sunshine-download.dmg", home);
    snprintf(mount, sizeof(mount), "%s/sunshine-mount", home);

    streaming_write_progress(home, "downloading", NULL);
    {
        char* curl_args[] = {(char*)"curl",
                             (char*)"-fL",
                             (char*)"--retry",
                             (char*)"2",
                             (char*)"--max-time",
                             (char*)"1200",
                             (char*)"-o",
                             dmg,
                             (char*)SUNSHINE_DOWNLOAD_URL,
                             NULL};
        if (!streaming_exec_capture(curl_args, out, sizeof(out)) || !stream_file_nonempty(dmg)) {
            (void)unlink(dmg);
            streaming_write_progress(home, "error", "Could not download Sunshine — check your internet connection");
            _exit(1);
        }
    }
    /* Supply-chain check: the DMG must match the digest GitHub publishes for
     * the release asset we downloaded. */
    {
        char actual[CC_SHA256_DIGEST_LENGTH * 2 + 1];
        char expected[CC_SHA256_DIGEST_LENGTH * 2 + 1];
        if (!streaming_expected_dmg_sha256(expected, sizeof(expected))) {
            (void)unlink(dmg);
            streaming_write_progress(home, "error",
                                     "Could not verify the Sunshine download (GitHub API unreachable) — try again");
            _exit(1);
        }
        if (!streaming_sha256_file(dmg, actual, sizeof(actual)) || strcmp(actual, expected) != 0) {
            (void)unlink(dmg);
            streaming_write_progress(
                home, "error", "The downloaded Sunshine image failed its integrity check — nothing was installed");
            _exit(1);
        }
    }

    streaming_write_progress(home, "mounting", NULL);
    (void)mkdir(mount, 0755);
    {
        char* attach_args[] = {(char*)"/usr/bin/hdiutil", (char*)"attach",      dmg,   (char*)"-nobrowse",
                               (char*)"-readonly",        (char*)"-mountpoint", mount, NULL};
        if (!streaming_exec_capture(attach_args, out, sizeof(out))) {
            (void)unlink(dmg);
            streaming_write_progress(home, "error", "Could not open the Sunshine disk image");
            _exit(1);
        }
    }

    streaming_write_progress(home, "copying", NULL);
    char plist[PATH_MAX];
    snprintf(app_source, sizeof(app_source), "%s/Sunshine.app", mount);
    snprintf(plist, sizeof(plist), "%s/Contents/Info.plist", app_source);
    if (!stream_file_nonempty(plist)) {
        (void)streaming_exec_capture(detach_args, out, sizeof(out));
        (void)unlink(dmg);
        streaming_write_progress(home, "error", "Sunshine.app was missing from the disk image");
        _exit(1);
    }
    {
        char* rm_args[] = {(char*)"/bin/rm", (char*)"-rf", (char*)SUNSHINE_APP_PATH, NULL};
        char* cp_args[] = {(char*)"/bin/cp", (char*)"-R", app_source, (char*)"/Applications", NULL};
        char* xattr_args[] = {(char*)"/usr/bin/xattr", (char*)"-cr", (char*)SUNSHINE_APP_PATH, NULL};
        (void)streaming_exec_capture(rm_args, out, sizeof(out));
        if (!streaming_exec_capture(cp_args, out, sizeof(out))) {
            (void)streaming_exec_capture(detach_args, out, sizeof(out));
            (void)unlink(dmg);
            streaming_write_progress(home, "error", "Could not copy Sunshine.app into /Applications");
            _exit(1);
        }
        (void)streaming_exec_capture(xattr_args, out, sizeof(out));
    }
    (void)streaming_exec_capture(detach_args, out, sizeof(out));
    (void)unlink(dmg);
    if (!streaming_installed()) {
        streaming_write_progress(home, "error", "Sunshine installed, but the app bundle was not found");
        _exit(1);
    }
    streaming_write_progress(home, "complete", NULL);
    _exit(0);
}

char* ms_streaming_install_json(const char* home, int* status) {
    pid_t pid;
    if (status)
        *status = 200;
    if (!stream_file_nonempty("/usr/bin/hdiutil")) {
        if (status)
            *status = 500;
        return streaming_error("hdiutil was not found — Sunshine cannot be installed automatically");
    }
    streaming_refresh_install(home);
    if (g_install_active) {
        return strdup("{\"ok\":true,\"started\":true,\"installing\":true}");
    }
    streaming_write_progress(home, "preparing", NULL);
    pid = fork();
    if (pid < 0) {
        streaming_write_progress(home, "error", "could not start the Sunshine installer");
        if (status)
            *status = 500;
        return streaming_error("could not start the Sunshine installer");
    }
    if (pid == 0) {
        int fd;
        /* The worker forks without exec, so FD_CLOEXEC on the listening and
         * client sockets never applies — close every inherited descriptor or
         * the worker keeps the backend's ports bound after a backend restart. */
        for (fd = 3; fd < 256; fd++)
            close(fd);
        fd = open("/dev/null", O_WRONLY);
        if (fd >= 0) {
            dup2(fd, STDIN_FILENO);
            close(fd);
        }
        streaming_install_worker(home);
    }
    g_install_pid = pid;
    g_install_active = true;
    return strdup("{\"ok\":true,\"started\":true}");
}

/* ------------------------------------------------------------------ */
/* status                                                              */
/* ------------------------------------------------------------------ */

char* ms_streaming_status_json(const char* home) {
    ms_json_writer writer;
    char* out;
    bool installed = streaming_installed();
    bool running = streaming_web_up();
    char creds_user[128] = {0}, creds_pass[256] = {0};
    bool have_creds = streaming_read_creds(home, creds_user, sizeof(creds_user), creds_pass, sizeof(creds_pass));
    bool creds_valid = false;
    char pairings_summary[1024] = {0};
    unsigned pairing_count = 0;
    char version[128] = "unknown";
    char* progress_path = stream_path_join(home, STREAMING_PROGRESS_FILE);
    char* progress_body = progress_path ? stream_read_file(progress_path, 4096) : NULL;
    char parse_error[128];
    ms_json* progress = NULL;
    char *progress_state = NULL, *progress_error = NULL;
    bool installing;
    char* defaults_args[] = {(char*)"/usr/bin/defaults", (char*)"read", (char*)SUNSHINE_APP_PATH "/Contents/Info",
                             (char*)"CFBundleShortVersionString", NULL};

    streaming_refresh_install(home);
    if (installed) {
        char raw_version[128] = {0};
        if (streaming_exec_capture(defaults_args, raw_version, sizeof(raw_version))) {
            char* nl = strpbrk(raw_version, "\r\n");
            if (nl)
                *nl = '\0';
            if (raw_version[0])
                snprintf(version, sizeof(version), "%s", raw_version);
        }
    }
    if (progress_body)
        progress = ms_json_parse(progress_body, strlen(progress_body), parse_error, sizeof(parse_error));
    if (progress && ms_json_type_of(progress) == MS_JSON_OBJECT) {
        const ms_json* value;
        if ((value = ms_json_object_get(progress, "status")) != NULL)
            ms_json_as_string(value, &progress_state);
        if ((value = ms_json_object_get(progress, "detail")) != NULL)
            ms_json_as_string(value, &progress_error);
    }
    if (!g_install_active && progress_state &&
        (strcmp(progress_state, "preparing") == 0 || strcmp(progress_state, "downloading") == 0 ||
         strcmp(progress_state, "mounting") == 0 || strcmp(progress_state, "copying") == 0)) {
        /* Backend restarted (or the worker died) mid-install; persist a
         * terminal state so the wizard does not poll forever. */
        free(progress_state);
        progress_state = strdup("error");
        if (!progress_error) {
            free(progress_error);
            progress_error = strdup("MetalSharp restarted before the Sunshine install finished — start it again");
        }
        streaming_write_progress(home, progress_state, progress_error);
    }
    installing = g_install_active;

    if (running && have_creds) {
        char body[2048] = {0}, code[16] = {0};
        if (streaming_api(creds_user, creds_pass, false, SUNSHINE_WEB_BASE "/api/pin", NULL, body, sizeof(body), code,
                          sizeof(code)) &&
            strcmp(code, "200") == 0) {
            ms_json* parsed = ms_json_parse(body, strlen(body), parse_error, sizeof(parse_error));
            creds_valid = parsed != NULL;
            if (parsed && ms_json_type_of(parsed) == MS_JSON_OBJECT) {
                const ms_json* arr = ms_json_object_get(parsed, "pairings");
                if (arr && ms_json_type_of(arr) == MS_JSON_ARRAY) {
                    size_t count = ms_json_array_length(arr);
                    pairing_count = (unsigned)count;
                    for (size_t i = 0; i < count && i < 5; i++) {
                        const ms_json* item = ms_json_array_get(arr, i);
                        char *id = NULL, *name = NULL;
                        const ms_json* field;
                        if (item && ms_json_type_of(item) == MS_JSON_OBJECT) {
                            if ((field = ms_json_object_get(item, "name")) != NULL)
                                ms_json_as_string(field, &name);
                            if ((field = ms_json_object_get(item, "id")) != NULL)
                                ms_json_as_string(field, &id);
                            if (i)
                                strncat(pairings_summary, ", ",
                                        sizeof(pairings_summary) - strlen(pairings_summary) - 1);
                            strncat(pairings_summary, name && name[0] ? name : (id ? id : "device"),
                                    sizeof(pairings_summary) - strlen(pairings_summary) - 1);
                        }
                        free(id);
                        free(name);
                    }
                }
            }
            ms_json_free(parsed);
        }
    }

    ms_json_writer_init(&writer);
    ms_json_writer_object_begin(&writer);
    ms_json_writer_key(&writer, "ok");
    ms_json_writer_bool(&writer, true);
    ms_json_writer_key(&writer, "installed");
    ms_json_writer_bool(&writer, installed);
    ms_json_writer_key(&writer, "running");
    ms_json_writer_bool(&writer, running);
    ms_json_writer_key(&writer, "version");
    ms_json_writer_string(&writer, version);
    ms_json_writer_key(&writer, "creds_configured");
    ms_json_writer_bool(&writer, have_creds);
    ms_json_writer_key(&writer, "creds_valid");
    ms_json_writer_bool(&writer, creds_valid);
    ms_json_writer_key(&writer, "creds_username");
    ms_json_writer_string(&writer, creds_user);
    ms_json_writer_key(&writer, "web_url");
    ms_json_writer_string(&writer, SUNSHINE_WEB_BASE);
    ms_json_writer_key(&writer, "installing");
    ms_json_writer_bool(&writer, installing);
    ms_json_writer_key(&writer, "pairing_count");
    ms_json_writer_u64(&writer, pairing_count);
    ms_json_writer_key(&writer, "pairings_summary");
    ms_json_writer_string(&writer, pairings_summary);
    ms_json_writer_key(&writer, "progress_status");
    if (progress_state)
        ms_json_writer_string(&writer, progress_state);
    else
        ms_json_writer_null(&writer);
    ms_json_writer_key(&writer, "progress_detail");
    if (progress_error)
        ms_json_writer_string(&writer, progress_error);
    else
        ms_json_writer_null(&writer);
    ms_json_writer_object_end(&writer);
    out = ms_json_writer_take(&writer);
    free(progress_state);
    free(progress_error);
    ms_json_free(progress);
    free(progress_body);
    free(progress_path);
    return out;
}

/* ------------------------------------------------------------------ */
/* launch (with first-run credential bootstrap)                        */
/* ------------------------------------------------------------------ */

char* ms_streaming_launch_json(const char* home, int* status) {
    ms_json_writer writer;
    char* out;
    char creds_user[128] = {0}, creds_pass[256] = {0};
    bool have_creds = streaming_read_creds(home, creds_user, sizeof(creds_user), creds_pass, sizeof(creds_pass));
    char* open_args[] = {(char*)"/usr/bin/open", (char*)"-a", (char*)"Sunshine", NULL};
    char open_out[256];
    bool creds_recognized = false;
    char body[512] = {0}, code[16] = {0};
    char json_body[512];

    if (status)
        *status = 200;
    if (!streaming_installed()) {
        if (status)
            *status = 400;
        return streaming_error("Sunshine is not installed — install it first");
    }
    if (!have_creds) {
        snprintf(creds_user, sizeof(creds_user), "metalsharp");
        if (!streaming_generate_passphrase(creds_pass, sizeof(creds_pass))) {
            if (status)
                *status = 500;
            return streaming_error("could not generate streaming credentials");
        }
        if (!streaming_write_creds(home, creds_user, creds_pass)) {
            if (status)
                *status = 500;
            return streaming_error("could not store streaming credentials");
        }
    }
    if (!streaming_exec_capture(open_args, open_out, sizeof(open_out))) {
        if (status)
            *status = 500;
        return streaming_error("could not launch Sunshine");
    }
    for (unsigned i = 0; i < 15; i++) {
        sleep(1);
        if (streaming_web_up_timeout("1"))
            break;
    }
    if (!streaming_web_up()) {
        if (status)
            *status = 504;
        return streaming_error(
            "Sunshine launched, but its web service did not come up — approve the screen-recording permission if "
            "macOS asked, then try again");
    }
    /* If credentials already work, done. Otherwise bootstrap them via the
     * auth-free first-run endpoint (only succeeds while Sunshine has no web
     * credentials yet). */
    {
        char probe_body[256] = {0}, probe_code[16] = {0};
        if (streaming_api(creds_user, creds_pass, false, SUNSHINE_WEB_BASE "/api/pin", NULL, probe_body,
                          sizeof(probe_body), probe_code, sizeof(probe_code)) &&
            strcmp(probe_code, "200") == 0) {
            creds_recognized = true;
        } else {
            snprintf(json_body, sizeof(json_body),
                     "{\"newUsername\":\"%s\",\"newPassword\":\"%s\",\"confirmNewPassword\":\"%s\"}", creds_user,
                     creds_pass, creds_pass);
            if (streaming_api("", "", true, SUNSHINE_WEB_BASE "/api/password", json_body, body, sizeof(body), code,
                              sizeof(code)) &&
                strstr(body, "\"status\":true") != NULL)
                creds_recognized = true;
        }
    }
    ms_json_writer_init(&writer);
    ms_json_writer_object_begin(&writer);
    ms_json_writer_key(&writer, "ok");
    ms_json_writer_bool(&writer, true);
    ms_json_writer_key(&writer, "running");
    ms_json_writer_bool(&writer, true);
    ms_json_writer_key(&writer, "creds_username");
    ms_json_writer_string(&writer, creds_user);
    ms_json_writer_key(&writer, "creds_recognized");
    ms_json_writer_bool(&writer, creds_recognized);
    ms_json_writer_object_end(&writer);
    out = ms_json_writer_take(&writer);
    return out;
}

char* ms_streaming_stop_json(const char* home, int* status) {
    char out[256];
    (void)home;
    if (status)
        *status = 200;
    /* osascript auto-launches a non-running app to deliver the quit event, so
     * only quit when the web service says Sunshine is actually up. */
    if (!streaming_web_up())
        return strdup("{\"ok\":true,\"running\":false}");
    {
        char* quit_args[] = {(char*)"/usr/bin/osascript", (char*)"-e", (char*)"tell application \"Sunshine\" to quit",
                             NULL};
        (void)streaming_exec_capture(quit_args, out, sizeof(out));
    }
    sleep(2);
    if (streaming_web_up()) {
        char* pkill_args[] = {(char*)"/usr/bin/pkill", (char*)"-f", (char*)SUNSHINE_BIN_PATH, NULL};
        (void)streaming_exec_capture(pkill_args, out, sizeof(out));
        sleep(1);
    }
    /* Report the truth: the host may take a moment to shut down. */
    if (streaming_web_up_timeout("1"))
        return streaming_error("Sunshine is still shutting down — try again in a moment");
    return strdup("{\"ok\":true,\"running\":false}");
}

char* ms_streaming_pin_json(const char* home, const unsigned char* body_bytes, size_t body_length, int* status) {
    ms_json_writer writer;
    char* out;
    char creds_user[128] = {0}, creds_pass[256] = {0};
    char body[2048] = {0}, code[16] = {0}, parse_error[128];
    char pin[8] = {0}, pairing_id[64] = {0};
    char request_body[256];
    ms_json* parsed;
    const ms_json* field;
    char* request_body_ptr = NULL;
    bool paired = false;

    if (status)
        *status = 200;
    if (!streaming_read_creds(home, creds_user, sizeof(creds_user), creds_pass, sizeof(creds_pass))) {
        if (status)
            *status = 400;
        return streaming_error(
            "Sunshine credentials are not set up yet — launch streaming once so MetalSharp can configure them");
    }
    if (!streaming_web_up()) {
        if (status)
            *status = 409;
        return streaming_error("Sunshine is not running — start streaming first");
    }
    {
        char* text = body_bytes ? malloc(body_length + 1) : NULL;
        if (!text) {
            if (status)
                *status = 400;
            return streaming_error("invalid PIN request");
        }
        memcpy(text, body_bytes, body_length);
        text[body_length] = '\0';
        parsed = ms_json_parse(text, body_length, parse_error, sizeof(parse_error));
        free(text);
    }
    if (parsed && ms_json_type_of(parsed) == MS_JSON_OBJECT) {
        if ((field = ms_json_object_get(parsed, "pin")) != NULL) {
            char* value = NULL;
            if (ms_json_as_string(field, &value)) {
                snprintf(pin, sizeof(pin), "%s", value);
                free(value);
            }
        }
    }
    ms_json_free(parsed);
    if (strlen(pin) != 4) {
        if (status)
            *status = 400;
        return streaming_error("Enter the 4-digit PIN shown in Moonlight");
    }
    for (const char* c = pin; *c; c++) {
        if (*c < '0' || *c > '9') {
            if (status)
                *status = 400;
            return streaming_error("The PIN must contain exactly 4 numeric digits");
        }
    }
    if (!streaming_api(creds_user, creds_pass, false, SUNSHINE_WEB_BASE "/api/pin", NULL, body, sizeof(body), code,
                       sizeof(code))) {
        if (status)
            *status = 502;
        return streaming_error("Could not reach the Sunshine web service");
    }
    if (strcmp(code, "401") == 0) {
        if (status)
            *status = 401;
        return streaming_error(
            "Sunshine web credentials changed — update them in the Sunshine web UI, then delete the stored "
            "streaming credentials and launch again");
    }
    parsed = ms_json_parse(body, strlen(body), parse_error, sizeof(parse_error));
    if (parsed && ms_json_type_of(parsed) == MS_JSON_OBJECT) {
        const ms_json* arr = ms_json_object_get(parsed, "pairings");
        if (arr && ms_json_type_of(arr) == MS_JSON_ARRAY && ms_json_array_length(arr) > 0) {
            const ms_json* item = ms_json_array_get(arr, 0);
            const ms_json* id_field = item ? ms_json_object_get(item, "id") : NULL;
            char* id = NULL;
            if (id_field && ms_json_as_string(id_field, &id)) {
                snprintf(pairing_id, sizeof(pairing_id), "%s", id);
                free(id);
            }
        }
    }
    ms_json_free(parsed);
    if (!pairing_id[0]) {
        if (status)
            *status = 409;
        return streaming_error(
            "No device is waiting to pair — open Moonlight on your phone, tap this Mac, and enter the PIN it shows");
    }
    snprintf(request_body, sizeof(request_body), "{\"pairing_id\":\"%s\",\"pin\":\"%s\",\"name\":\"%s\"}", pairing_id,
             pin, STREAMING_PIN_NAME);
    request_body_ptr = request_body;
    if (!streaming_api(creds_user, creds_pass, true, SUNSHINE_WEB_BASE "/api/pin", request_body_ptr, body, sizeof(body),
                       code, sizeof(code))) {
        if (status)
            *status = 502;
        return streaming_error("Pairing request to Sunshine failed");
    }
    if (strcmp(code, "401") == 0) {
        if (status)
            *status = 401;
        return streaming_error("Sunshine web credentials changed — update them in the Sunshine web UI");
    }
    parsed = ms_json_parse(body, strlen(body), parse_error, sizeof(parse_error));
    if (parsed && ms_json_type_of(parsed) == MS_JSON_OBJECT) {
        const ms_json* value = ms_json_object_get(parsed, "status");
        bool flag = false;
        if (value && ms_json_as_bool(value, &flag))
            paired = flag;
    }
    ms_json_free(parsed);
    ms_json_writer_init(&writer);
    ms_json_writer_object_begin(&writer);
    ms_json_writer_key(&writer, "ok");
    ms_json_writer_bool(&writer, paired);
    if (!paired) {
        ms_json_writer_key(&writer, "error");
        ms_json_writer_string(&writer,
                              "Pairing failed — the PIN did not match or the device disconnected. Try again with a "
                              "fresh PIN from Moonlight.");
    }
    ms_json_writer_object_end(&writer);
    out = ms_json_writer_take(&writer);
    return out;
}

char* ms_streaming_unpair_all_json(const char* home, int* status) {
    char creds_user[128] = {0}, creds_pass[256] = {0};
    char body[512] = {0}, code[16] = {0};
    if (status)
        *status = 200;
    if (!streaming_read_creds(home, creds_user, sizeof(creds_user), creds_pass, sizeof(creds_pass))) {
        if (status)
            *status = 400;
        return streaming_error("Sunshine credentials are not set up yet — launch streaming first");
    }
    if (!streaming_api(creds_user, creds_pass, true, SUNSHINE_WEB_BASE "/api/clients/unpair-all", "{}", body,
                       sizeof(body), code, sizeof(code)) ||
        strcmp(code, "200") != 0) {
        if (status)
            *status = 502;
        return streaming_error("Could not unpair devices — is Sunshine running?");
    }
    return strdup("{\"ok\":true}");
}
