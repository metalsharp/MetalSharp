/* MetalSharp dock-hide helper dylib.
 *
 * Injected into Wine processes via DYLD_INSERT_LIBRARIES by the MetalSharp
 * backend. macOS 27 attributes every Wine child to the MetalSharp bundle, so
 * each launched game/Steam process registers a regular app with the Dock and
 * shows an extra MetalSharp icon. This dylib forces the Wine process's
 * activation policy to NSApplicationActivationPolicyAccessory — accessory
 * apps get no dock icon at all (windows still render and receive input).
 *
 * Wine's mac driver sets the policy to Regular during its own init, so a
 * one-shot set is not enough. The dylib therefore:
 *   1. sets Accessory as soon as NSApplication exists,
 *   2. swizzles -[NSApplication setActivationPolicy:] to force Accessory on
 *      any later call,
 *   3. re-asserts Accessory periodically during startup.
 *
 * Built as a universal binary (arm64 + x86_64) since Wine runs under
 * Rosetta. Set METALSHARP_DOCKHIDE_DEBUG=1 to trace loading to /tmp.
 */

#include <objc/runtime.h>
#include <objc/message.h>
#include <pthread.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static void dockhide_trace(const char* message) {
    if (!getenv("METALSHARP_DOCKHIDE_DEBUG"))
        return;
    FILE* f = fopen("/tmp/dockhide-debug.log", "a");
    if (f) {
        fprintf(f, "dockhide[pid %d]: %s\n", (int)getpid(), message);
        fclose(f);
    }
}

static IMP dockhide_orig_set_policy = NULL;

/* Replacement IMP: whatever policy Wine requests, answer Accessory. */
static long dockhide_set_activation_policy_stub(id self, SEL _cmd, long policy) {
    (void)policy;
    if (dockhide_orig_set_policy)
        return (long)((BOOL(*)(id, SEL, long))dockhide_orig_set_policy)(self, _cmd, 1);
    return 0;
}

static void apply_accessory_policy(void) {
    Class cls = objc_getClass("NSApplication");
    if (!cls)
        return;
    id app = ((id(*)(id, SEL))objc_msgSend)((id)cls, sel_registerName("sharedApplication"));
    if (!app)
        return;
    ((void(*)(id, SEL, long))objc_msgSend)(app, sel_registerName("setActivationPolicy:"),
                                           1 /* NSApplicationActivationPolicyAccessory */);
    dockhide_trace("activation policy set to Accessory");
}

static void* dockhide_poll_thread(void* unused) {
    (void)unused;
    Class cls = NULL;
    /* Wait up to 15s for AppKit/NSApplication to appear. */
    for (int attempt = 0; attempt < 50; attempt++) {
        usleep(300 * 1000);
        cls = objc_getClass("NSApplication");
        if (cls)
            break;
    }
    if (!cls) {
        dockhide_trace("NSApplication never appeared (non-GUI process)");
        return NULL;
    }
    apply_accessory_policy();

    /* Wine's mac driver sets Regular during its init; force every later
     * setActivationPolicy: back to Accessory. */
    Method m = class_getInstanceMethod(cls, sel_registerName("setActivationPolicy:"));
    if (m) {
        dockhide_orig_set_policy = method_getImplementation(m);
        method_setImplementation(m, (IMP)dockhide_set_activation_policy_stub);
        dockhide_trace("setActivationPolicy: swizzled to force Accessory");
    }

    /* Belt and braces: re-assert during the whole startup window. */
    for (int i = 0; i < 30; i++) {
        usleep(500 * 1000);
        apply_accessory_policy();
    }
    dockhide_trace("startup window complete");
    return NULL;
}

__attribute__((constructor))
static void dockhide_constructor(void) {
    dockhide_trace("constructor loaded");
    if (objc_getClass("NSApplication")) {
        apply_accessory_policy();
        return;
    }
    pthread_t thread;
    if (pthread_create(&thread, NULL, dockhide_poll_thread, NULL) == 0)
        pthread_detach(thread);
}
