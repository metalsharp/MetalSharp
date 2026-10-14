#ifndef METALSHARP_BACKEND_SETUP_H
#define METALSHARP_BACKEND_SETUP_H

#include <stdbool.h>
#include <stddef.h>

/* Recursively clear macOS quarantine xattrs from an installed tree so
 * Gatekeeper never blocks locally unpacked payloads. */
void ms_clear_quarantine_tree(const char* path);
/* Upgrade installed Wine wrappers so routed launch environments are preserved. */
bool ms_setup_wine_wrapper_ensure(const char* metalsharp_home);
/* Replace or remove pre-C-backend hook DLL copies that still carry the NT IPC client. */
void ms_setup_refresh_legacy_ntdll_hooks(const char* metalsharp_home);
/* Link the bundled SDL2 into the Wine runtime so winebus can use its SDL controller bus. */
bool ms_setup_wine_sdl_ensure(const char* metalsharp_home);
bool ms_setup_ensure_faudio(const char* metalsharp_home);

char* ms_setup_state_json(const char* metalsharp_home);
char* ms_setup_save_json(const char* metalsharp_home, const unsigned char* body, size_t body_length, int* status);
char* ms_setup_device_name_json(void);
char* ms_setup_dependencies_json(const char* metalsharp_home);
char* ms_setup_agility_versions_json(void);
char* ms_setup_install_progress_json(const char* metalsharp_home);
char* ms_setup_installing_json(void);
char* ms_setup_install_dependencies_json(const unsigned char*, size_t, int*);
char* ms_setup_install_all_json(const char*, int*);
char* ms_setup_install_vcpp_json(const char*, bool, int*);
char* ms_setup_vcpp_status_json(const char*);

#endif
