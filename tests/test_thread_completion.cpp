#include <atomic>
#include <metalsharp/ExtraShims.h>
#include <metalsharp/Kernel32Shim.h>
#include <metalsharp/SyncContext.h>
#include <metalsharp/Win32Types.h>
#include <unistd.h>

using namespace metalsharp::win32;
using metalsharp::ShimLibrary;

namespace {

struct ThreadArguments {
    std::atomic<bool> started{false};
    std::atomic<bool> release{false};
    DWORD exitCode = 37;
};

DWORD MSABI threadEntry(void* rawArguments) {
    auto* arguments = static_cast<ThreadArguments*>(rawArguments);
    arguments->started.store(true);
    while (!arguments->release.load())
        usleep(1000);
    return arguments->exitCode;
}

} // namespace

int main() {
    using CreateThreadFn = HANDLE(MSABI*)(void*, SIZE_T, void*, void*, DWORD, DWORD*);
    using GetExitCodeThreadFn = BOOL(MSABI*)(HANDLE, DWORD*);
    using WaitForSingleObjectFn = DWORD(MSABI*)(HANDLE, DWORD);
    using WaitForMultipleObjectsFn = DWORD(MSABI*)(DWORD, HANDLE*, BOOL, DWORD);

    ShimLibrary kernel32 = Kernel32Shim::create();
    addMissingKernel32(kernel32);

    auto createThread = reinterpret_cast<CreateThreadFn>(kernel32.functions.at("CreateThread")());
    auto getExitCodeThread = reinterpret_cast<GetExitCodeThreadFn>(kernel32.functions.at("GetExitCodeThread")());
    auto waitForSingleObject = reinterpret_cast<WaitForSingleObjectFn>(kernel32.functions.at("WaitForSingleObject")());
    auto waitForMultipleObjects =
        reinterpret_cast<WaitForMultipleObjectsFn>(kernel32.functions.at("WaitForMultipleObjects")());

    ThreadArguments singleWaitArguments;
    ThreadArguments multipleWaitArguments;
    DWORD singleThreadId = 0;
    DWORD multipleThreadId = 0;
    HANDLE singleWaitThread = nullptr;
    HANDLE multipleWaitThread = nullptr;
    auto cleanup = [&] {
        singleWaitArguments.release.store(true);
        multipleWaitArguments.release.store(true);
        if (singleWaitThread)
            waitForSingleObject(singleWaitThread, 2000);
        if (multipleWaitThread)
            waitForSingleObject(multipleWaitThread, 2000);
        if (singleWaitThread)
            SyncContext::instance().destroyHandle(singleWaitThread);
        if (multipleWaitThread)
            SyncContext::instance().destroyHandle(multipleWaitThread);
    };

    singleWaitThread =
        createThread(nullptr, 0, reinterpret_cast<void*>(threadEntry), &singleWaitArguments, 0, &singleThreadId);
    multipleWaitThread =
        createThread(nullptr, 0, reinterpret_cast<void*>(threadEntry), &multipleWaitArguments, 0, &multipleThreadId);
    if (!singleWaitThread || !multipleWaitThread || singleThreadId == 0 || multipleThreadId == 0) {
        cleanup();
        return 1;
    }

    for (int i = 0; i < 2000 && (!singleWaitArguments.started.load() || !multipleWaitArguments.started.load()); i++)
        usleep(1000);
    if (!singleWaitArguments.started.load() || !multipleWaitArguments.started.load()) {
        cleanup();
        return 1;
    }

    DWORD singleExitCode = 0;
    DWORD multipleExitCode = 0;
    if (!getExitCodeThread(singleWaitThread, &singleExitCode) || singleExitCode != 259) {
        cleanup();
        return 1;
    }
    if (!getExitCodeThread(multipleWaitThread, &multipleExitCode) || multipleExitCode != 259) {
        cleanup();
        return 1;
    }
    if (waitForSingleObject(singleWaitThread, 0) != WAIT_TIMEOUT) {
        cleanup();
        return 1;
    }

    singleWaitArguments.release.store(true);
    multipleWaitArguments.release.store(true);
    if (waitForSingleObject(singleWaitThread, 2000) != WAIT_OBJECT_0) {
        cleanup();
        return 1;
    }

    HANDLE multipleWaitHandles[] = {multipleWaitThread};
    if (waitForMultipleObjects(1, multipleWaitHandles, 0, 2000) != WAIT_OBJECT_0) {
        cleanup();
        return 1;
    }
    if (!getExitCodeThread(singleWaitThread, &singleExitCode) || singleExitCode != singleWaitArguments.exitCode) {
        cleanup();
        return 1;
    }
    if (!getExitCodeThread(multipleWaitThread, &multipleExitCode) ||
        multipleExitCode != multipleWaitArguments.exitCode) {
        cleanup();
        return 1;
    }
    if (waitForSingleObject(multipleWaitThread, 0) != WAIT_OBJECT_0) {
        cleanup();
        return 1;
    }

    cleanup();
    return 0;
}
