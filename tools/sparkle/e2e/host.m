// A host for the Sparkle bridge's end-to-end test (run-e2e.sh): it updates itself through MSSparkleBridge.m's C API
// exactly as MetalSharp's update sheet does (update, poll the state, install when ready, quit when installing) and logs
// each phase and its own version to the file Info.plist's MSE2ELog names. After Sparkle relaunches it, the log shows
// the new version.
#import <AppKit/AppKit.h>

int ms_sparkle_available(void);
int ms_sparkle_update(void);
int ms_sparkle_install(void);
char *ms_sparkle_state_json(void);
void ms_sparkle_free(char *text);

static NSString *logPath;

static void logLine(NSString *line) {
    NSString *text = [NSString stringWithFormat:@"%@ %@\n", [NSDate date], line];
    NSFileHandle *file = [NSFileHandle fileHandleForWritingAtPath:logPath];
    if (!file) {
        [text writeToFile:logPath atomically:NO encoding:NSUTF8StringEncoding error:nil];
        return;
    }
    [file seekToEndOfFile];
    [file writeData:[text dataUsingEncoding:NSUTF8StringEncoding]];
    [file closeFile];
}

@interface HostDelegate : NSObject <NSApplicationDelegate>
@property (nonatomic, copy) NSString *lastPhase;
@end

@implementation HostDelegate
- (void)applicationDidFinishLaunching:(NSNotification *)notification {
    NSDictionary *info = NSBundle.mainBundle.infoDictionary;
    logPath = info[@"MSE2ELog"];
    NSString *version = info[@"CFBundleVersion"];
    logLine([NSString stringWithFormat:@"launched version %@ from %@", version, NSBundle.mainBundle.bundlePath]);
    if ([version isEqualToString:info[@"MSE2ETarget"]]) {
        logLine(@"done: running the target version");
        [NSApp terminate:nil];
        return;
    }
    logLine([NSString stringWithFormat:@"available %d", ms_sparkle_available()]);
    logLine([NSString stringWithFormat:@"update %d", ms_sparkle_update()]);
    [NSTimer scheduledTimerWithTimeInterval:0.25 repeats:YES block:^(NSTimer *timer) {
        char *raw = ms_sparkle_state_json();
        NSString *json = [NSString stringWithUTF8String:raw];
        ms_sparkle_free(raw);
        NSDictionary *state = [NSJSONSerialization JSONObjectWithData:[json dataUsingEncoding:NSUTF8StringEncoding]
                                                              options:0 error:nil];
        NSString *phase = state[@"phase"];
        if (![phase isEqualToString:self.lastPhase]) {
            logLine([NSString stringWithFormat:@"state %@", json]);
            self.lastPhase = phase;
        }
        if ([phase isEqualToString:@"ready"]) {
            logLine([NSString stringWithFormat:@"install %d", ms_sparkle_install()]);
        } else if ([phase isEqualToString:@"installing"]) {
            logLine(@"quitting for the installer");
            [timer invalidate];
            [NSApp terminate:nil];
        } else if ([phase isEqualToString:@"error"] || [phase isEqualToString:@"none"]) {
            logLine(@"failed");
            [timer invalidate];
            [NSApp terminate:nil];
        }
    }];
}
@end

int main(void) {
    @autoreleasepool {
        NSApplication *app = NSApplication.sharedApplication;
        HostDelegate *delegate = [HostDelegate new];
        app.delegate = delegate;
        [app setActivationPolicy:NSApplicationActivationPolicyAccessory];
        [app run];
    }
    return 0;
}
