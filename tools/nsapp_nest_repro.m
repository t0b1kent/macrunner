/* MacRunner 2026-07-28 (HK input lane) — standalone reproduction of the exact
 * shape winemac.drv gives AppKit, with no guest, no prefix and no wine.
 *
 * Measured state to explain: [NSApp run] is on the main-thread stack inside
 * _DPSNextEvent/_BlockUntilNextEventMatchingListInMode, com.apple.NSEventThread
 * exists, Cocoa windows exist and draw — and app_sendEvent_enter is 0,
 * applicationDidBecomeActive is 0, activationPolicy reads `prohibited`.
 *
 * The wine shape has two unusual properties at once, and a 45-minute title run
 * cannot separate them:
 *   (1) NESTING. ntdll's apple_main_thread() runs CFRunLoopRun() on the real
 *       main thread (loader.c). macdrv_start_cocoa_app then schedules
 *       run_cocoa_app as a CFRunLoopSource perform callback on that loop
 *       (cocoa_main.m:420), and [NSApp run] is entered from INSIDE that
 *       callback and never returns.
 *   (2) BUNDLE-LESSNESS. The running image is a bare Mach-O re-exec'd out of
 *       $TMPDIR/winetemp-*, with no Info.plist and CFBundleIdentifier=[NULL].
 *
 * This program reproduces each independently:
 *   nested   — CFRunLoopRun() on main, [NSApp run] from a source callback (wine)
 *   direct   — [NSApp run] straight from main() (the control)
 * and reports, every second: activation policy, setActivationPolicy: return
 * value, isActive, keyWindow, and the -sendEvent: count.
 *
 * Build: clang -fobjc-arc -framework Cocoa -o /tmp/mr-agents/nsapp_repro \
 *              tools/nsapp_nest_repro.m
 * Run:   /tmp/mr-agents/nsapp_repro nested|direct [seconds]
 */

#import <Cocoa/Cocoa.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

static volatile long g_send_event_count;
static volatile long g_did_become_active;
static volatile long g_run_returned;
static int g_policy_set_ret = -1;

@interface ReproApplication : NSApplication
@end

@implementation ReproApplication
- (void)sendEvent:(NSEvent *)e
{
    g_send_event_count++;
    [super sendEvent:e];
}
@end

@interface ReproDelegate : NSObject <NSApplicationDelegate>
@end

@implementation ReproDelegate
- (void)applicationDidBecomeActive:(NSNotification *)n
{
    g_did_become_active++;
    fprintf(stderr, "repro: applicationDidBecomeActive #%ld\n", g_did_become_active);
    fflush(stderr);
}
@end

static const char *policy_name(NSApplicationActivationPolicy p)
{
    switch (p)
    {
        case NSApplicationActivationPolicyRegular:    return "regular";
        case NSApplicationActivationPolicyAccessory:  return "accessory";
        case NSApplicationActivationPolicyProhibited: return "prohibited";
    }
    return "unknown";
}

/* Everything winemac.drv's run_cocoa_app does, in the same order. */
static void become_cocoa_app(void)
{
    NSWindow *win;

    fprintf(stderr, "repro: become_cocoa_app on_main_thread=%d\n", (int)pthread_main_np());
    fflush(stderr);

    [ReproApplication sharedApplication];
    fprintf(stderr, "repro: NSApp=%p class=%s policy_initial=%s\n",
            (void *)NSApp, object_getClassName(NSApp),
            policy_name([NSApp activationPolicy]));

    /* winemac.drv's transformProcessToForeground: discards this BOOL. */
    g_policy_set_ret = (int)[NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    fprintf(stderr, "repro: setActivationPolicy:regular ret=%d policy_now=%s\n",
            g_policy_set_ret, policy_name([NSApp activationPolicy]));

    [NSApp setDelegate:[[ReproDelegate alloc] init]];

    win = [[NSWindow alloc] initWithContentRect:NSMakeRect(200, 200, 420, 300)
                                      styleMask:(NSWindowStyleMaskTitled |
                                                 NSWindowStyleMaskClosable |
                                                 NSWindowStyleMaskResizable)
                                        backing:NSBackingStoreBuffered
                                          defer:NO];
    [win setTitle:@"MacRunner NSApp repro"];
    [win setReleasedWhenClosed:NO];
    [win makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];

    fprintf(stderr, "repro: window=%p visible=%d onscreen_before_run key=%p isActive=%d\n",
            (void *)win, (int)[win isVisible], (void *)[NSApp keyWindow], (int)[NSApp isActive]);
    fflush(stderr);

    /* The reporter has to live on a run loop that keeps ticking while [NSApp
     * run] owns the thread, so it is a repeating NSTimer in common modes. */
    [[NSRunLoop currentRunLoop] addTimer:
        [NSTimer timerWithTimeInterval:1.0 repeats:YES block:^(NSTimer *t) {
            fprintf(stderr, "repro: t+ policy=%s isActive=%d key=%p main_win=%p "
                    "sendEvent=%ld didBecomeActive=%ld run_returned=%ld\n",
                    policy_name([NSApp activationPolicy]), (int)[NSApp isActive],
                    (void *)[NSApp keyWindow], (void *)[NSApp mainWindow],
                    g_send_event_count, g_did_become_active, g_run_returned);
            fflush(stderr);
        }] forMode:NSRunLoopCommonModes];

    fprintf(stderr, "repro: entering [NSApp run]\n");
    fflush(stderr);
    [NSApp run];
    g_run_returned++;
    fprintf(stderr, "repro: [NSApp run] RETURNED (#%ld)\n", g_run_returned);
    fflush(stderr);
}

static void source_perform(void *info)
{
    (void)info;
    fprintf(stderr, "repro: CFRunLoopSource perform callback entered\n");
    fflush(stderr);
    become_cocoa_app();
}

/* Mirrors macdrv_start_cocoa_app(): a NON-main thread signals a source that is
 * scheduled on the main run loop, then waits. */
static void *starter_thread(void *arg)
{
    CFRunLoopSourceContext ctx;
    CFRunLoopSourceRef source;

    (void)arg;
    memset(&ctx, 0, sizeof(ctx));
    ctx.perform = source_perform;
    source = CFRunLoopSourceCreate(NULL, 0, &ctx);
    CFRunLoopAddSource(CFRunLoopGetMain(), source, kCFRunLoopCommonModes);
    CFRunLoopSourceSignal(source);
    CFRunLoopWakeUp(CFRunLoopGetMain());
    fprintf(stderr, "repro: starter thread signalled the main run loop source\n");
    fflush(stderr);
    return NULL;
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "nested";
    double secs = argc > 2 ? atof(argv[2]) : 12.0;
    pthread_t th;

    fprintf(stderr, "repro: mode=%s secs=%.0f pid=%d exe=%s bundleid=%s\n",
            mode, secs, getpid(), argv[0],
            [[NSBundle mainBundle] bundleIdentifier].UTF8String ?: "(nil)");
    fflush(stderr);

    /* Watchdog: [NSApp run] never returns, so the process must kill itself. */
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(secs * NSEC_PER_SEC)),
                   dispatch_get_global_queue(0, 0), ^{
        fprintf(stderr, "repro: FINAL policy=%s isActive=%d sendEvent=%ld "
                "didBecomeActive=%ld setPolicyRet=%d run_returned=%ld\n",
                policy_name([NSApp activationPolicy]), (int)[NSApp isActive],
                g_send_event_count, g_did_become_active, g_policy_set_ret,
                g_run_returned);
        fflush(stderr);
        _exit(0);
    });

    if (!strcmp(mode, "direct"))
    {
        become_cocoa_app();
        return 0;
    }

    /* nested: exactly ntdll apple_main_thread() + macdrv_start_cocoa_app(). */
    pthread_create(&th, NULL, starter_thread, NULL);
    CFRunLoopRun();
    fprintf(stderr, "repro: outer CFRunLoopRun RETURNED\n");
    fflush(stderr);
    return 0;
}
