#ifndef METALSHARP_BACKEND_GAME_EXECUTABLE_H
#define METALSHARP_BACKEND_GAME_EXECUTABLE_H

#include <stdbool.h>
#include <stddef.h>

/*
 * Per-game executable overrides are persisted relative to the provider's
 * canonical install directory, so moving the installation preserves the choice.
 * load returns 0 when no override exists, 1 when valid, and -1 when a saved
 * override is invalid or no longer exists.
 */
int ms_game_executable_override_load(const char* home, const char* provider, const char* game_id,
                                     const char* install_dir, char** executable_out);
bool ms_game_executable_override_save(const char* home, const char* provider, const char* game_id,
                                      const char* install_dir, const char* executable, char* error, size_t error_size);

#endif
