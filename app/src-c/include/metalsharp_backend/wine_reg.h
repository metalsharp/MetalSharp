#ifndef METALSHARP_BACKEND_WINE_REG_H
#define METALSHARP_BACKEND_WINE_REG_H

#include <stdbool.h>

/* `wine reg <reg_args...>` (NULL-terminated, at most 13) in prefix, with its output discarded and no DLL
 * overrides, within 60 seconds. Returns whether reg exited 0. */
bool ms_wine_reg(const char* wine, const char* prefix, char* const reg_args[]);

#endif
