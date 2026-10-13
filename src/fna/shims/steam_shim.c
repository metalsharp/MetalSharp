#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

/* The Wine-side bridge listens on <METALSHARP_HOME>/run/steam-bridge.sock, an
 * owner-only Unix socket, rather than a fixed TCP port that other software
 * may already be using. */
#define BRIDGE_SOCKET_NAME "run/steam-bridge.sock"

#define MSG_INIT             1
#define MSG_SHUTDOWN         2
#define MSG_RESTART_APP      3
#define MSG_IS_RUNNING       4
#define MSG_GET_APP_ID       5
#define MSG_RUN_CALLBACKS    6
#define MSG_GET_STEAM_ID     7
#define MSG_GET_HSTEAM_PIPE  8
#define MSG_GET_HSTEAM_USER  9
#define MSG_GET_PERSONA_NAME 10
#define MSG_INTERNAL_INIT    11
#define MSG_PING             0xFF

#define MSG_RESPONSE 0x80000000
#define MSG_ERROR    0x40000000

typedef int32_t HSteamPipe;
typedef int32_t HSteamUser;
typedef uint32_t AppId_t;
typedef uint64_t CSteamID;

static int g_sock = -1;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_initialized = 0;
static HSteamPipe g_pipe = 0;
static HSteamUser g_user = 0;
static int bridge_socket_path(struct sockaddr_un* address) {
    const char* home = getenv("METALSHARP_HOME");
    const char* user_home = getenv("HOME");
    int written;
    memset(address, 0, sizeof(*address));
    address->sun_family = AF_UNIX;
    if (home && home[0])
        written = snprintf(address->sun_path, sizeof(address->sun_path), "%s/%s", home, BRIDGE_SOCKET_NAME);
    else if (user_home && user_home[0])
        written =
            snprintf(address->sun_path, sizeof(address->sun_path), "%s/.metalsharp/%s", user_home, BRIDGE_SOCKET_NAME);
    else
        return 0;
    if (written < 0 || (size_t)written >= sizeof(address->sun_path)) {
        fprintf(stderr, "[steam_shim] Steam bridge socket path is too long\n");
        return 0;
    }
    return 1;
}

static int send_all(int s, const void* buf, int len) {
    const char* p = (const char*)buf;
    while (len > 0) {
        int n = send(s, p, len, 0);
        if (n <= 0)
            return 0;
        p += n;
        len -= n;
    }
    return 1;
}

static int recv_all(int s, void* buf, int len) {
    char* p = (char*)buf;
    while (len > 0) {
        int n = recv(s, p, len, 0);
        if (n <= 0)
            return 0;
        p += n;
        len -= n;
    }
    return 1;
}

static int bridge_connect(void) {
    struct sockaddr_un addr;
    uid_t peer_uid;
    gid_t peer_gid;
    if (g_sock >= 0)
        return 1;
    if (!bridge_socket_path(&addr))
        return 0;
    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s < 0)
        return 0;
    if (connect(s, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(s);
        return 0;
    }
    /* The bridge always runs as this user. */
    if (getpeereid(s, &peer_uid, &peer_gid) != 0 || peer_uid != geteuid()) {
        fprintf(stderr, "[steam_shim] Ignoring Steam bridge owned by another user\n");
        close(s);
        return 0;
    }
    g_sock = s;
    fprintf(stderr, "[steam_shim] Connected to bridge at %s\n", addr.sun_path);
    return 1;
}

static int bridge_rpc(uint32_t msg_type, const void* payload, uint32_t plen, uint32_t* resp_type, uint8_t** resp_data,
                      uint32_t* resp_len) {
    pthread_mutex_lock(&g_lock);
    if (!bridge_connect()) {
        pthread_mutex_unlock(&g_lock);
        return 0;
    }

    if (!send_all(g_sock, &msg_type, 4) || !send_all(g_sock, &plen, 4) ||
        (plen > 0 && !send_all(g_sock, payload, plen))) {
        close(g_sock);
        g_sock = -1;
        pthread_mutex_unlock(&g_lock);
        return 0;
    }

    uint32_t rt, rl;
    if (!recv_all(g_sock, &rt, 4) || !recv_all(g_sock, &rl, 4)) {
        close(g_sock);
        g_sock = -1;
        pthread_mutex_unlock(&g_lock);
        return 0;
    }

    uint8_t* rd = NULL;
    if (rl > 0 && rl < 65536) {
        rd = (uint8_t*)malloc(rl);
        if (!recv_all(g_sock, rd, rl)) {
            free(rd);
            close(g_sock);
            g_sock = -1;
            pthread_mutex_unlock(&g_lock);
            return 0;
        }
    }

    if (resp_type)
        *resp_type = rt;
    if (resp_data)
        *resp_data = rd;
    else
        free(rd);
    if (resp_len)
        *resp_len = rl;
    pthread_mutex_unlock(&g_lock);
    return 1;
}

static int bridge_simple(uint32_t msg_type, const void* payload, uint32_t plen) {
    uint32_t rt;
    uint8_t* rd;
    uint32_t rl;
    int ok = bridge_rpc(msg_type, payload, plen, &rt, &rd, &rl);
    free(rd);
    return ok && (rt & MSG_RESPONSE);
}

__attribute__((used)) int SteamAPI_Init(void) {
    uint32_t rt;
    uint8_t* rd = NULL;
    uint32_t rl;
    int ok = bridge_rpc(MSG_INIT, NULL, 0, &rt, &rd, &rl);
    int result = 0;
    if (ok && rl >= 1)
        result = rd[0];
    free(rd);
    if (result)
        g_initialized = 1;
    fprintf(stderr, "[steam_shim] SteamAPI_Init -> %d\n", result);
    return result;
}

__attribute__((used)) int SteamInternal_SteamAPI_Init(const char* cmd, void* err) {
    uint32_t rt;
    uint8_t* rd = NULL;
    uint32_t rl;
    uint32_t clen = cmd ? (uint32_t)strlen(cmd) : 0;
    fprintf(stderr, "[steam_shim] SteamInternal_SteamAPI_Init cmd=\"%s\" (len=%u)\n", cmd ? cmd : "(null)", clen);
    int ok = bridge_rpc(MSG_INIT, cmd, clen, &rt, &rd, &rl);
    int result = 0;
    if (ok && rl >= 1)
        result = rd[0];
    free(rd);
    if (!result) {
        fprintf(stderr, "[steam_shim] SteamInternal_SteamAPI_Init failed, trying plain SteamAPI_Init\n");
        ok = bridge_rpc(MSG_INIT, NULL, 0, &rt, &rd, &rl);
        result = 0;
        if (ok && rl >= 1)
            result = rd[0];
        free(rd);
    }
    if (result)
        g_initialized = 1;
    fprintf(stderr, "[steam_shim] SteamInternal_SteamAPI_Init -> %d\n", result);
    return result;
}

__attribute__((used)) void SteamAPI_Shutdown(void) {
    bridge_simple(MSG_SHUTDOWN, NULL, 0);
    g_initialized = 0;
    if (g_sock >= 0) {
        close(g_sock);
        g_sock = -1;
    }
}

__attribute__((used)) int SteamAPI_RestartAppIfNecessary(AppId_t appid) {
    uint32_t rt;
    uint8_t* rd = NULL;
    uint32_t rl;
    int ok = bridge_rpc(MSG_RESTART_APP, &appid, 4, &rt, &rd, &rl);
    int result = 0;
    if (ok && rl >= 1)
        result = rd[0];
    free(rd);
    fprintf(stderr, "[steam_shim] SteamAPI_RestartAppIfNecessary(%u) -> %d\n", appid, result);
    return result;
}

__attribute__((used)) int SteamAPI_IsSteamRunning(void) {
    if (g_initialized)
        return 1;
    uint32_t rt;
    uint8_t* rd = NULL;
    uint32_t rl;
    int ok = bridge_rpc(MSG_IS_RUNNING, NULL, 0, &rt, &rd, &rl);
    int result = 0;
    if (ok && rl >= 1)
        result = rd[0];
    free(rd);
    return result;
}

__attribute__((used)) void SteamAPI_RunCallbacks(void) {
    bridge_simple(MSG_RUN_CALLBACKS, NULL, 0);
}

__attribute__((used)) HSteamPipe SteamAPI_GetHSteamPipe(void) {
    uint32_t rt;
    uint8_t* rd = NULL;
    uint32_t rl;
    int ok = bridge_rpc(MSG_GET_HSTEAM_PIPE, NULL, 0, &rt, &rd, &rl);
    int32_t result = 0;
    if (ok && rl >= 4)
        memcpy(&result, rd, 4);
    free(rd);
    g_pipe = result;
    return result;
}

__attribute__((used)) HSteamUser SteamAPI_GetHSteamUser(void) {
    uint32_t rt;
    uint8_t* rd = NULL;
    uint32_t rl;
    int ok = bridge_rpc(MSG_GET_HSTEAM_USER, NULL, 0, &rt, &rd, &rl);
    int32_t result = 0;
    if (ok && rl >= 4)
        memcpy(&result, rd, 4);
    free(rd);
    g_user = result;
    return result;
}

__attribute__((used)) void* SteamInternal_CreateInterface(const char* ver) {
    return (void*)1;
}

__attribute__((used)) int SteamInternal_GameServer_Init(uint32_t ip, uint16_t gp, uint16_t qp, uint16_t sp, int secure,
                                                        AppId_t appid) {
    return 1;
}

__attribute__((used)) uint32_t SteamUtils_GetAppID(void) {
    uint32_t rt;
    uint8_t* rd = NULL;
    uint32_t rl;
    int ok = bridge_rpc(MSG_GET_APP_ID, NULL, 0, &rt, &rd, &rl);
    uint32_t result = 0;
    if (ok && rl >= 4)
        memcpy(&result, rd, 4);
    free(rd);
    return result;
}

__attribute__((used)) void SteamAPI_SetMiniDumpComment(const char* msg) {}
__attribute__((used)) void SteamAPI_WriteMiniDump(uint32_t a, void* b, uint32_t c) {}
__attribute__((used)) void SteamAPI_SetWarningMessageHook(void* fn) {}
__attribute__((used)) void SteamAPI_RegisterCallback(void* cb, int i) {}
__attribute__((used)) void SteamAPI_UnregisterCallback(void* cb) {}
__attribute__((used)) void SteamAPI_RegisterCallResult(void* cb, uint64_t call) {}
__attribute__((used)) void SteamAPI_UnregisterCallResult(void* cb, uint64_t call) {}

__attribute__((used)) void* SteamAPI_GetISteamUser(HSteamPipe p, HSteamUser u, const char* v) {
    return (void*)1;
}
__attribute__((used)) void* SteamAPI_GetISteamFriends(HSteamPipe p, HSteamUser u, const char* v) {
    return (void*)2;
}
__attribute__((used)) void* SteamAPI_GetISteamUtils(HSteamPipe p, const char* v) {
    return (void*)3;
}
__attribute__((used)) void* SteamAPI_GetISteamMatchmaking(HSteamPipe p, HSteamUser u, const char* v) {
    return (void*)4;
}
__attribute__((used)) void* SteamAPI_GetISteamMatchmakingServers(HSteamPipe p, HSteamUser u, const char* v) {
    return (void*)5;
}
__attribute__((used)) void* SteamAPI_GetISteamUserStats(HSteamPipe p, HSteamUser u, const char* v) {
    return (void*)6;
}
__attribute__((used)) void* SteamAPI_GetISteamGameServerStats(HSteamPipe p, HSteamUser u, const char* v) {
    return (void*)7;
}
__attribute__((used)) void* SteamAPI_GetISteamApps(HSteamPipe p, HSteamUser u, const char* v) {
    return (void*)8;
}
__attribute__((used)) void* SteamAPI_GetISteamNetworking(HSteamPipe p, HSteamUser u, const char* v) {
    return (void*)9;
}
__attribute__((used)) void* SteamAPI_GetISteamRemoteStorage(HSteamPipe p, HSteamUser u, const char* v) {
    return (void*)10;
}
__attribute__((used)) void* SteamAPI_GetISteamScreenshots(HSteamPipe p, HSteamUser u, const char* v) {
    return (void*)11;
}
__attribute__((used)) void* SteamAPI_GetISteamController(HSteamPipe p, HSteamUser u, const char* v) {
    return (void*)12;
}
__attribute__((used)) void* SteamAPI_GetISteamUGC(HSteamPipe p, HSteamUser u, const char* v) {
    return (void*)13;
}
__attribute__((used)) void* SteamAPI_GetISteamHTTP(HSteamPipe p, HSteamUser u, const char* v) {
    return (void*)14;
}
__attribute__((used)) void* SteamAPI_GetISteamMusic(HSteamPipe p, HSteamUser u, const char* v) {
    return (void*)15;
}
__attribute__((used)) void* SteamAPI_GetISteamMusicRemote(HSteamPipe p, HSteamUser u, const char* v) {
    return (void*)16;
}
__attribute__((used)) void* SteamAPI_GetISteamHTMLSurface(HSteamPipe p, HSteamUser u, const char* v) {
    return (void*)17;
}
__attribute__((used)) void* SteamAPI_GetISteamInventory(HSteamPipe p, HSteamUser u, const char* v) {
    return (void*)18;
}
__attribute__((used)) void* SteamAPI_GetISteamVideo(HSteamPipe p, HSteamUser u, const char* v) {
    return (void*)19;
}
__attribute__((used)) void* SteamAPI_GetISteamParentalSettings(HSteamPipe p, HSteamUser u, const char* v) {
    return (void*)20;
}
__attribute__((used)) void* SteamAPI_GetISteamNetworkingSockets(HSteamPipe p, HSteamUser u, const char* v) {
    return (void*)21;
}
__attribute__((used)) void* SteamAPI_GetISteamNetworkingUtils(HSteamPipe p, const char* v) {
    return (void*)22;
}
__attribute__((used)) void* SteamAPI_GetISteamNetworkingMessages(HSteamPipe p, HSteamUser u, const char* v) {
    return (void*)23;
}
__attribute__((used)) void* SteamAPI_GetISteamGenericInterface(HSteamPipe p, HSteamUser u, const char* v) {
    return (void*)1;
}

__attribute__((constructor)) static void shim_init(void) {
    fprintf(stderr, "[steam_shim] MetalSharp Steam API Shim loaded\n");
}

__attribute__((destructor)) static void shim_fini(void) {
    if (g_initialized)
        SteamAPI_Shutdown();
    if (g_sock >= 0) {
        close(g_sock);
        g_sock = -1;
    }
    fprintf(stderr, "[steam_shim] Unloaded\n");
}
