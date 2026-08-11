#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>

static const CGFloat kBgR = 0.106f;
static const CGFloat kBgG = 0.157f;
static const CGFloat kBgB = 0.220f;
static const CGFloat kAccentR = 0.400f;
static const CGFloat kAccentG = 0.753f;
static const CGFloat kAccentB = 0.957f;

static NSWindow *gWindow;
static NSTextField *gStatusLabel;
static NSProgressIndicator *gProgressBar;
static NSTextField *gTitleLabel;
static NSTimer *gPollTimer;
static BOOL gMigrationComplete = NO;
static NSTimeInterval gStartTime = 0;

@interface MigrationView : NSView
@end

@implementation MigrationView

- (void)drawRect:(NSRect)dirtyRect {
    [super drawRect:dirtyRect];
    NSRect bounds = self.bounds;
    [[NSColor colorWithCalibratedRed:kBgR green:kBgG blue:kBgB alpha:1.0f] setFill];
    NSRectFill(bounds);

    CGFloat midX = NSMidX(bounds);
    CGFloat pipeY = bounds.size.height - 80;

    NSArray *stageNames = @[@"D3D11", @"Metal", @"DXGI", @"DXMT", @"FNA"];
    NSInteger numStages = stageNames.count;
    CGFloat stageWidth = 60.0f;
    CGFloat gap = 16.0f;
    CGFloat totalWidth = numStages * stageWidth + (numStages - 1) * gap;
    CGFloat startX = midX - totalWidth / 2.0f;

    NSTimeInterval now = [[NSDate date] timeIntervalSinceReferenceDate];

    for (NSInteger i = 0; i < numStages; i++) {
        CGFloat x = startX + i * (stageWidth + gap);
        NSRect boxRect = NSMakeRect(x, pipeY - 14, stageWidth, 28);

        CGFloat wave = (sinf((float)(now - gStartTime) * 2.0f + (float)i * 1.2f) + 1.0f) / 2.0f;
        CGFloat alpha = 0.12f + 0.5f * wave;
        NSColor *boxColor = [NSColor colorWithCalibratedRed:kAccentR green:kAccentG blue:kAccentB alpha:alpha];
        NSBezierPath *roundedPath = [NSBezierPath bezierPathWithRoundedRect:boxRect xRadius:6 yRadius:6];
        [boxColor setFill];
        [roundedPath fill];

        [[NSColor colorWithCalibratedRed:kAccentR green:kAccentG blue:kAccentB alpha:0.4f] setStroke];
        [roundedPath setLineWidth:1.0f];
        [roundedPath stroke];

        NSDictionary *attrs = @{
            NSFontAttributeName : [NSFont monospacedSystemFontOfSize:10.0f weight:NSFontWeightMedium],
            NSForegroundColorAttributeName : [NSColor colorWithCalibratedRed:kAccentR green:kAccentG blue:kAccentB alpha:0.9f],
        };
        NSString *name = stageNames[i];
        NSSize textSize = [name sizeWithAttributes:attrs];
        NSPoint textPoint = NSMakePoint(NSMidX(boxRect) - textSize.width / 2.0f, NSMidY(boxRect) - textSize.height / 2.0f);
        [name drawAtPoint:textPoint withAttributes:attrs];

        if (i < numStages - 1) {
            CGFloat arrowX = NSMaxX(boxRect) + 2;
            NSPoint arrowStart = NSMakePoint(arrowX, NSMidY(boxRect));
            NSPoint arrowEnd = NSMakePoint(arrowX + gap - 4, NSMidY(boxRect));
            CGFloat arrowAlpha = 0.2f + 0.3f * wave;
            NSColor *arrowColor = [NSColor colorWithCalibratedRed:kAccentR green:kAccentG blue:kAccentB alpha:arrowAlpha];
            [arrowColor setStroke];
            [NSBezierPath strokeLineFromPoint:arrowStart toPoint:arrowEnd];
            NSPoint triTip = arrowEnd;
            NSPoint tri1 = NSMakePoint(arrowEnd.x - 4, arrowEnd.y - 3);
            NSPoint tri2 = NSMakePoint(arrowEnd.x - 4, arrowEnd.y + 3);
            NSBezierPath *tri = [NSBezierPath bezierPath];
            [tri moveToPoint:triTip];
            [tri lineToPoint:tri1];
            [tri lineToPoint:tri2];
            [tri closePath];
            [arrowColor setFill];
            [tri fill];
        }
    }

    [self setNeedsDisplay:YES];
}
@end

static NSDictionary *fetchJSON(NSString *urlString, NSString *method) {
    NSURL *url = [NSURL URLWithString:urlString];
    NSMutableURLRequest *req = [NSMutableURLRequest requestWithURL:url];
    req.HTTPMethod = method ?: @"GET";
    req.timeoutInterval = 5.0;
    NSString *apiToken = [[[NSProcessInfo processInfo] environment] objectForKey:@"METALSHARP_API_TOKEN"];
    if (apiToken.length > 0) {
        [req setValue:[NSString stringWithFormat:@"Bearer %@", apiToken] forHTTPHeaderField:@"Authorization"];
    }

    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    __block NSDictionary *result = nil;

    NSURLSessionDataTask *task = [[NSURLSession sharedSession] dataTaskWithRequest:req completionHandler:^(NSData *data, NSURLResponse *response, NSError *error) {
        if (data && !error) {
            result = [NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
        }
        dispatch_semaphore_signal(sem);
    }];
    [task resume];
    dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC));
    return result;
}

static void relaunchApp() {
    NSURL *appURL = [NSURL fileURLWithPath:@"/Applications/MetalSharp.app"];
    if ([[NSFileManager defaultManager] fileExistsAtPath:appURL.path]) {
        NSWorkspaceOpenConfiguration *config = [[NSWorkspaceOpenConfiguration alloc] init];
        config.activates = YES;
        [[NSWorkspace sharedWorkspace] openApplicationAtURL:appURL configuration:config completionHandler:nil];
    }
    [NSApp terminate:nil];
}

static void pollProgress() {
    NSDictionary *progress = fetchJSON(@"http://127.0.0.1:9274/update/migrate/progress", @"GET");
    if (!progress) return;

    NSString *status = progress[@"status"] ?: @"";
    NSString *message = progress[@"message"] ?: @"";
    NSNumber *stepNum = progress[@"step"] ?: @(0);
    NSNumber *totalSteps = progress[@"total"] ?: @(1);

    [gStatusLabel setStringValue:message];

    double fraction = [stepNum doubleValue] / [totalSteps doubleValue];
    [gProgressBar setDoubleValue:fraction * 100.0];

    if ([status isEqualToString:@"complete"]) {
        gMigrationComplete = YES;
        [gPollTimer invalidate];
        [gStatusLabel setStringValue:@"Migration complete! Relaunching MetalSharp..."];
        [gProgressBar setDoubleValue:100.0];
        [NSTimer scheduledTimerWithTimeInterval:2.0
                                        target:[NSApp delegate]
                                      selector:@selector(relaunch)
                                      userInfo:nil
                                       repeats:NO];
    } else if ([status isEqualToString:@"error"]) {
        [gPollTimer invalidate];
        NSString *errMsg = progress[@"error"] ?: @"Unknown error";
        [gStatusLabel setStringValue:[NSString stringWithFormat:@"Error: %@", errMsg]];
    }
}

@interface AppDelegate : NSObject <NSApplicationDelegate>
@end

@implementation AppDelegate

- (void)relaunch {
    relaunchApp();
}

- (void)applicationDidFinishLaunching:(NSNotification *)notification {
    gStartTime = [[NSDate date] timeIntervalSinceReferenceDate];

    NSRect frame = NSMakeRect(0, 0, 520, 340);
    NSUInteger style = NSWindowStyleMaskTitled | NSWindowStyleMaskFullSizeContentView;
    gWindow = [[NSWindow alloc] initWithContentRect:frame
                                          styleMask:style
                                            backing:NSBackingStoreBuffered
                                              defer:NO];
    [gWindow setTitle:@"MetalSharp Migration"];
    [gWindow setTitleVisibility:NSWindowTitleHidden];
    [gWindow setTitlebarAppearsTransparent:YES];
    [gWindow center];
    [gWindow setMovableByWindowBackground:YES];
    [gWindow setBackgroundColor:[NSColor colorWithCalibratedRed:kBgR green:kBgG blue:kBgB alpha:1.0f]];
    [gWindow setHasShadow:YES];
    [gWindow setLevel:NSStatusWindowLevel];
    [gWindow setOpaque:YES];

    MigrationView *contentView = [[MigrationView alloc] initWithFrame:frame];
    [gWindow setContentView:contentView];

    gTitleLabel = [[NSTextField alloc] initWithFrame:NSMakeRect(0, frame.size.height - 50, frame.size.width, 30)];
    [gTitleLabel setEditable:NO];
    [gTitleLabel setBezeled:NO];
    [gTitleLabel setDrawsBackground:NO];
    [gTitleLabel setAlignment:NSTextAlignmentCenter];
    [gTitleLabel setStringValue:@"MetalSharp Migration"];
    [gTitleLabel setFont:[NSFont systemFontOfSize:20.0f weight:NSFontWeightBold]];
    [gTitleLabel setTextColor:[NSColor whiteColor]];
    [contentView addSubview:gTitleLabel];

    CGFloat subY = frame.size.height - 72;
    NSTextField *subLabel = [[NSTextField alloc] initWithFrame:NSMakeRect(0, subY, frame.size.width, 20)];
    [subLabel setEditable:NO];
    [subLabel setBezeled:NO];
    [subLabel setDrawsBackground:NO];
    [subLabel setAlignment:NSTextAlignmentCenter];
    [subLabel setStringValue:@"Upgrading runtime to latest version"];
    [subLabel setFont:[NSFont systemFontOfSize:12.0f weight:NSFontWeightRegular]];
    [subLabel setTextColor:[NSColor colorWithCalibratedRed:kAccentR green:kAccentG blue:kAccentB alpha:0.8f]];
    [contentView addSubview:subLabel];

    gProgressBar = [[NSProgressIndicator alloc] initWithFrame:NSMakeRect(60, 50, frame.size.width - 120, 20)];
    [gProgressBar setStyle:NSProgressIndicatorStyleBar];
    [gProgressBar setMinValue:0];
    [gProgressBar setMaxValue:100];
    [gProgressBar setDoubleValue:0];
    [contentView addSubview:gProgressBar];

    gStatusLabel = [[NSTextField alloc] initWithFrame:NSMakeRect(20, 25, frame.size.width - 40, 22)];
    [gStatusLabel setEditable:NO];
    [gStatusLabel setBezeled:NO];
    [gStatusLabel setDrawsBackground:NO];
    [gStatusLabel setAlignment:NSTextAlignmentCenter];
    [gStatusLabel setStringValue:@"Checking migration status..."];
    [gStatusLabel setFont:[NSFont systemFontOfSize:12.0f weight:NSFontWeightMedium]];
    [gStatusLabel setTextColor:[NSColor colorWithCalibratedWhite:0.7f alpha:1.0f]];
    [contentView addSubview:gStatusLabel];

    [gWindow makeKeyAndOrderFront:nil];
    [gWindow orderFrontRegardless];

    [contentView setNeedsDisplay:YES];
    [[NSRunLoop currentRunLoop] addTimer:[NSTimer scheduledTimerWithTimeInterval:0.05 target:contentView selector:@selector(setNeedsDisplay:) userInfo:nil repeats:YES] forMode:NSDefaultRunLoopMode];

    fetchJSON(@"http://127.0.0.1:9274/update/migrate/start", @"POST");

    gPollTimer = [NSTimer scheduledTimerWithTimeInterval:0.5 target:self selector:@selector(pollTick) userInfo:nil repeats:YES];
    [[NSRunLoop currentRunLoop] addTimer:gPollTimer forMode:NSDefaultRunLoopMode];
}

- (void)pollTick {
    pollProgress();
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)sender {
    return !gMigrationComplete;
}
@end

int main(int argc, const char *argv[]) {
    @autoreleasepool {
        NSApplication *app = [NSApplication sharedApplication];
        AppDelegate *delegate = [[AppDelegate alloc] init];
        [app setDelegate:delegate];
        [app run];
    }
    return 0;
}
