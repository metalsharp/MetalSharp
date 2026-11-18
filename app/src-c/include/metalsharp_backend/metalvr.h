#ifndef METALSHARP_BACKEND_METALVR_H
#define METALSHARP_BACKEND_METALVR_H

/* The MetalVR route's preparation before a game starts (metalvr.c): the VR server must be running; asks for Screen
 * Recording once and points the Steam prefix's OpenXR runtime and SteamVR at MetalVR. NULL when the game may start;
 * otherwise a message for the user, to be freed. */
char* ms_metalvr_prepare(const char* metalsharp_home);

/* GET /metalvr/status, POST /metalvr/start, POST /metalvr/stop: {"ok", "running", "error"?} */
char* ms_metalvr_status_json(const char* metalsharp_home);
char* ms_metalvr_start_json(const char* metalsharp_home, int* status);
char* ms_metalvr_stop_json(const char* metalsharp_home, int* status);

/* GET /metalvr/headset: the chosen headset, the trusted ALVR client, whether MetalVR is installed and running, and the
 * headsets it supports. POST /metalvr/headset saves the choice; POST /metalvr/setup runs a setup step. */
#include <stddef.h>
char* ms_metalvr_headset_json(const char* metalsharp_home);
char* ms_metalvr_set_headset_json(const char* metalsharp_home, const char* body, size_t length, int* status);
char* ms_metalvr_setup_json(const char* metalsharp_home, const char* body, size_t length, int* status);

#endif
