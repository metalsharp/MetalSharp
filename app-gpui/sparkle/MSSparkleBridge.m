// MetalSharp's Sparkle bridge: a user driver that never shows Sparkle's own windows. MetalSharp's GPUI update sheet
// drives it through the C functions at the bottom and draws its state (ms_sparkle_state_json), so an update looks as it
// always has, while Sparkle finds it in the build's appcast, downloads it (a .delta when there is one for this version),
// checks its EdDSA signature and code signature, installs it and relaunches MetalSharp.
//
// Everything here runs on the main thread, as Sparkle requires: GPUI calls in from its main-thread callbacks, and
// Sparkle calls the user driver on the main thread.

#import <Foundation/Foundation.h>
#import <Sparkle/Sparkle.h>

typedef NS_ENUM(NSInteger, MSSparklePhase) {
    MSSparklePhaseIdle,
    MSSparklePhaseChecking,
    MSSparklePhaseDownloading,
    MSSparklePhaseExtracting,
    MSSparklePhaseReady,
    MSSparklePhaseInstalling,
    MSSparklePhaseNoUpdate,
    MSSparklePhaseError,
};

static NSString *MSSparklePhaseName(MSSparklePhase phase) {
    switch (phase) {
    case MSSparklePhaseIdle: return @"idle";
    case MSSparklePhaseChecking: return @"checking";
    case MSSparklePhaseDownloading: return @"downloading";
    case MSSparklePhaseExtracting: return @"extracting";
    case MSSparklePhaseReady: return @"ready";
    case MSSparklePhaseInstalling: return @"installing";
    case MSSparklePhaseNoUpdate: return @"none";
    case MSSparklePhaseError: return @"error";
    }
    return @"idle";
}

@interface MSSparkleDriver : NSObject <SPUUserDriver, SPUUpdaterDelegate>
@property (nonatomic) MSSparklePhase phase;
@property (nonatomic, copy) NSString *version;
@property (nonatomic, copy) NSString *error;
@property (nonatomic) uint64_t expected;
@property (nonatomic) uint64_t received;
@property (nonatomic) double extracted;
@property (nonatomic) BOOL applicationTerminated;
@property (nonatomic, copy) void (^cancel)(void);
@property (nonatomic, copy) void (^readyReply)(SPUUserUpdateChoice);
@property (nonatomic, copy) void (^retryTerminate)(void);
@end

@implementation MSSparkleDriver

- (void)resetProgress {
    self.expected = 0;
    self.received = 0;
    self.extracted = 0;
}

// MetalSharp schedules its own checks; Sparkle never checks by itself and never sends a system profile.
- (void)showUpdatePermissionRequest:(SPUUpdatePermissionRequest *)request
                              reply:(void (^)(SUUpdatePermissionResponse *))reply {
    reply([[SUUpdatePermissionResponse alloc] initWithAutomaticUpdateChecks:NO sendSystemProfile:NO]);
}

- (void)showUserInitiatedUpdateCheckWithCancellation:(void (^)(void))cancellation {
    self.phase = MSSparklePhaseChecking;
    self.error = nil;
    self.cancel = cancellation;
    [self resetProgress];
}

// The user already chose to update in MetalSharp's sheet, so whatever Sparkle found is downloaded at once. An update
// downloaded in an earlier session is installed the same way.
- (void)showUpdateFoundWithAppcastItem:(SUAppcastItem *)appcastItem
                                 state:(SPUUserUpdateState *)state
                                 reply:(void (^)(SPUUserUpdateChoice))reply {
    self.version = appcastItem.displayVersionString ?: appcastItem.versionString;
    self.cancel = nil;
    self.phase = state.stage == SPUUserUpdateStageNotDownloaded ? MSSparklePhaseDownloading : MSSparklePhaseExtracting;
    reply(SPUUserUpdateChoiceInstall);
}

- (void)showUpdateReleaseNotesWithDownloadData:(SPUDownloadData *)downloadData {
}

- (void)showUpdateReleaseNotesFailedToDownloadWithError:(NSError *)error {
}

- (void)showUpdateNotFoundWithError:(NSError *)error acknowledgement:(void (^)(void))acknowledgement {
    self.phase = MSSparklePhaseNoUpdate;
    self.cancel = nil;
    acknowledgement();
}

- (void)showUpdaterError:(NSError *)error acknowledgement:(void (^)(void))acknowledgement {
    self.phase = MSSparklePhaseError;
    self.error = error.localizedDescription ?: @"The update failed";
    self.cancel = nil;
    self.readyReply = nil;
    acknowledgement();
}

- (void)showDownloadInitiatedWithCancellation:(void (^)(void))cancellation {
    self.phase = MSSparklePhaseDownloading;
    self.cancel = cancellation;
    [self resetProgress];
}

- (void)showDownloadDidReceiveExpectedContentLength:(uint64_t)expectedContentLength {
    self.expected = expectedContentLength;
    self.received = 0;
}

- (void)showDownloadDidReceiveDataOfLength:(uint64_t)length {
    self.received += length;
}

- (void)showDownloadDidStartExtractingUpdate {
    self.phase = MSSparklePhaseExtracting;
    self.cancel = nil;
    self.extracted = 0;
}

- (void)showExtractionReceivedProgress:(double)progress {
    self.extracted = progress;
}

// Ready: MetalSharp stops Steam, Wine and its backend and writes its post-update migration marker, then answers
// with ms_sparkle_install().
- (void)showReadyToInstallAndRelaunch:(void (^)(SPUUserUpdateChoice))reply {
    self.phase = MSSparklePhaseReady;
    self.readyReply = reply;
}

- (void)showInstallingUpdateWithApplicationTerminated:(BOOL)applicationTerminated
                          retryTerminatingApplication:(void (^)(void))retryTerminatingApplication {
    self.phase = MSSparklePhaseInstalling;
    self.applicationTerminated = applicationTerminated;
    self.retryTerminate = retryTerminatingApplication;
}

- (void)showUpdateInstalledAndRelaunched:(BOOL)relaunched acknowledgement:(void (^)(void))acknowledgement {
    acknowledgement();
}

- (void)showUpdateInFocus {
}

- (void)dismissUpdateInstallation {
    if (self.phase != MSSparklePhaseError && self.phase != MSSparklePhaseNoUpdate &&
        self.phase != MSSparklePhaseInstalling)
        self.phase = MSSparklePhaseIdle;
    self.cancel = nil;
    self.readyReply = nil;
}

// METALSHARP_SPARKLE_FEED_URL replaces the build's feed (Info.plist SUFeedURL), for testing against a local feed.
- (nullable NSString *)feedURLStringForUpdater:(SPUUpdater *)updater {
    NSString *feed = NSProcessInfo.processInfo.environment[@"METALSHARP_SPARKLE_FEED_URL"];
    return feed.length ? feed : nil;
}

@end

static MSSparkleDriver *driver;
static SPUUpdater *updater;
static NSString *startError;

// Sparkle starts once, the first time it is asked for. It is unavailable when this MetalSharp was built without a feed
// (no SUFeedURL or SUPublicEDKey: development builds), and the caller then updates with a DMG as before.
static BOOL ms_sparkle_started(void) {
    if (updater)
        return YES;
    if (startError)
        return NO;
    NSDictionary *info = NSBundle.mainBundle.infoDictionary;
    NSString *feed = NSProcessInfo.processInfo.environment[@"METALSHARP_SPARKLE_FEED_URL"];
    if (![info[@"SUPublicEDKey"] length] || (![info[@"SUFeedURL"] length] && !feed.length)) {
        startError = @"this MetalSharp has no update feed";
        return NO;
    }
    driver = [MSSparkleDriver new];
    SPUUpdater *candidate = [[SPUUpdater alloc] initWithHostBundle:NSBundle.mainBundle
                                                 applicationBundle:NSBundle.mainBundle
                                                        userDriver:driver
                                                          delegate:driver];
    NSError *error = nil;
    if (![candidate startUpdater:&error]) {
        startError = error.localizedDescription ?: @"Sparkle did not start";
        driver = nil;
        return NO;
    }
    updater = candidate;
    return YES;
}

static char *ms_sparkle_copy(NSString *text) {
    return strdup(text.UTF8String ?: "");
}

// 1 when this MetalSharp can update itself through Sparkle.
int ms_sparkle_available(void) {
    return ms_sparkle_started() ? 1 : 0;
}

// Starts a check that installs what it finds: 0 when it started, -1 when Sparkle is unavailable or already busy.
int ms_sparkle_update(void) {
    if (!ms_sparkle_started() || !updater.canCheckForUpdates)
        return -1;
    driver.phase = MSSparklePhaseChecking;
    driver.error = nil;
    driver.version = nil;
    [driver resetProgress];
    [updater checkForUpdates];
    return 0;
}

// Answers Sparkle's "ready to install": it installs and relaunches MetalSharp once this app quits.
int ms_sparkle_install(void) {
    void (^reply)(SPUUserUpdateChoice) = driver.readyReply;
    if (!reply)
        return -1;
    driver.readyReply = nil;
    // Sparkle reports "installing" (showInstallingUpdateWithApplicationTerminated:) once its installer has been told
    // to go ahead; MetalSharp quits then, and the installer relaunches it.
    reply(SPUUserUpdateChoiceInstall);
    return 0;
}

// Cancels a check or download in flight, or dismisses an update that is ready, which stays downloaded for later.
void ms_sparkle_cancel(void) {
    void (^cancel)(void) = driver.cancel;
    void (^reply)(SPUUserUpdateChoice) = driver.readyReply;
    driver.cancel = nil;
    driver.readyReply = nil;
    if (cancel)
        cancel();
    if (reply)
        reply(SPUUserUpdateChoiceDismiss);
    if (driver)
        driver.phase = MSSparklePhaseIdle;
}

// The update's state as JSON, freed with ms_sparkle_free:
// {"available", "phase", "version", "expected", "received", "extracted", "terminated", "error"}.
char *ms_sparkle_state_json(void) {
    NSMutableDictionary *state = [NSMutableDictionary dictionary];
    BOOL available = ms_sparkle_started();
    state[@"available"] = @(available);
    state[@"phase"] = MSSparklePhaseName(driver ? driver.phase : MSSparklePhaseIdle);
    state[@"version"] = driver.version ?: @"";
    state[@"expected"] = @(driver.expected);
    state[@"received"] = @(driver.received);
    state[@"extracted"] = @(driver.extracted);
    state[@"terminated"] = @(driver.applicationTerminated);
    state[@"error"] = driver.error ?: (available ? @"" : (startError ?: @""));
    NSData *data = [NSJSONSerialization dataWithJSONObject:state options:0 error:nil];
    NSString *text = data ? [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding] : @"{}";
    return ms_sparkle_copy(text);
}

void ms_sparkle_free(char *text) {
    free(text);
}
