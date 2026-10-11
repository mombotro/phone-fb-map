// FBMode12.m - SpringBoard tweak for the iPhone 5s (iOS 12). Loaded by our TweakInject.dylib, only into SpringBoard.
//
// While /tmp/fbmode exists and its modification time is under 5 seconds old (a framebuffer program touches it about
// once a second), SpringBoard
//   1. drops touch events, so taps cannot reach the home screen under our picture,
//   2. keeps every window hidden, so it has nothing to draw and cannot fight the program for the display, and
//   3. gets its idle timer reset, so the screen does not dim or lock.
// With no flag the tweak does nothing. If a program dies, SpringBoard takes touches again within 5 seconds.
// This is the iPhone 4 tweak (tweak/FBMode.m) without the kiosk parts. It uses plain Objective-C runtime swizzling.
// Everything it does goes to /tmp/fbmode12.log. A dump of the SpringBoard classes that may matter goes to /tmp/fbmode12.dump.
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define FLAG "/tmp/fbmode"
#define LOGFILE "/tmp/fbmode12.log"

static long g_dropped;
static int g_was_active;
static int (*HIDType)(void *);

static void logf_(const char *fmt, ...) {
    FILE *f = fopen(LOGFILE, "a");
    if (!f) return;
    va_list ap; va_start(ap, fmt);
    fprintf(f, "[%ld] ", (long)time(NULL) % 100000);
    vfprintf(f, fmt, ap);
    fputc('\n', f);
    va_end(ap); fclose(f);
}

static int active(void) {
    struct stat st;
    if (stat(FLAG, &st) != 0) return 0;
    return (time(NULL) - st.st_mtime) < 5;
}

static int is_touch_hid(void *ev) { return HIDType && ev && HIDType(ev) == 11; }          // kIOHIDEventTypeDigitizer
static int is_touch_uievent(id ev) {                                                        // UIEventTypeTouches == 0
    return ev && ((long (*)(id, SEL))objc_msgSend)(ev, sel_registerName("type")) == 0;
}

static void (*orig_app_hid)(id, SEL, void *);
static void new_app_hid(id self, SEL cmd, void *ev) { if (active() && is_touch_hid(ev)) { g_dropped++; return; } orig_app_hid(self, cmd, ev); }
static void (*orig_app_send)(id, SEL, id);
static void new_app_send(id self, SEL cmd, id ev) { if (active() && is_touch_uievent(ev)) { g_dropped++; return; } orig_app_send(self, cmd, ev); }
static void (*orig_win_send)(id, SEL, id);
static void new_win_send(id self, SEL cmd, id ev) { if (active() && is_touch_uievent(ev)) { g_dropped++; return; } orig_win_send(self, cmd, ev); }

// While the flag is fresh every window is hidden, and a window that something tries to show stays hidden.
static CFMutableArrayRef g_hidden;
static void (*orig_setHidden)(id, SEL, BOOL);
static long g_forced_hidden;

static void remember(id w) {
    if (!g_hidden) g_hidden = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
    CFRange r = CFRangeMake(0, CFArrayGetCount(g_hidden));
    if (!CFArrayContainsValue(g_hidden, r, w)) CFArrayAppendValue(g_hidden, w);
}
static void new_setHidden(id self, SEL cmd, BOOL h) {
    if (active() && !h) { remember(self); g_forced_hidden++; orig_setHidden(self, cmd, 1); return; }
    orig_setHidden(self, cmd, h);
}
static void hide_all_windows(void) {
    Class ac = objc_getClass("UIApplication");
    if (!ac || !orig_setHidden) return;
    id app = ((id (*)(id, SEL))objc_msgSend)((id)ac, sel_registerName("sharedApplication"));
    id wins = app ? ((id (*)(id, SEL))objc_msgSend)(app, sel_registerName("windows")) : NULL;
    if (!wins) return;
    SEL isHidden = sel_registerName("isHidden");
    CFArrayRef arr = (CFArrayRef)wins;
    long n = CFArrayGetCount(arr), hid = 0;
    for (long i = 0; i < n; i++) {
        id w = (id)CFArrayGetValueAtIndex(arr, i);
        if (((BOOL (*)(id, SEL))objc_msgSend)(w, isHidden)) continue;
        remember(w);
        orig_setHidden(w, sel_registerName("setHidden:"), 1);
        hid++;
    }
    logf_("silent mode: hid %ld of %ld windows", hid, n);
}
static void show_hidden_windows(void) {
    if (!g_hidden || !orig_setHidden) return;
    long n = CFArrayGetCount(g_hidden);
    for (long i = 0; i < n; i++) orig_setHidden((id)CFArrayGetValueAtIndex(g_hidden, i), sel_registerName("setHidden:"), 0);
    logf_("silent mode off: showed %ld windows (forced hidden %ld times, dropped %ld)", n, g_forced_hidden, g_dropped);
    CFArrayRemoveAllValues(g_hidden);
    g_forced_hidden = 0;
}

static void hook_one(const char *cls, const char *sel, IMP hook, IMP *orig) {
    Class c = objc_getClass(cls);
    if (!c) { logf_("hook %s %s: class not found", cls, sel); return; }
    SEL s = sel_registerName(sel);
    Method m = class_getInstanceMethod(c, s);
    if (!m) { logf_("hook %s %s: method not found", cls, sel); return; }
    *orig = method_getImplementation(m);
    class_replaceMethod(c, s, hook, method_getTypeEncoding(m));
    logf_("hooked %s %s", cls, sel);
}

// Idle timer. On iOS 12, SBBacklightController has preventIdleSleepForNumberOfSeconds: (a float). Older names are tried after.
static void reset_idle(void) {
    static int logged12;
    Class bl = objc_getClass("SBBacklightController");
    SEL pre = sel_registerName("preventIdleSleepForNumberOfSeconds:");
    if (bl && class_respondsToSelector(bl, pre) && class_respondsToSelector(object_getClass((id)bl), sel_registerName("sharedInstance"))) {
        id inst = ((id (*)(id, SEL))objc_msgSend)((id)bl, sel_registerName("sharedInstance"));
        if (inst) {
            ((void (*)(id, SEL, float))objc_msgSend)(inst, pre, 8.0f);
            if (!logged12) { logged12 = 1; logf_("idle sleep prevented with -[SBBacklightController preventIdleSleepForNumberOfSeconds:]"); }
            return;
        }
    }
    static const char *classes[] = { "SBBacklightController", "SBIdleTimerGlobalCoordinator", "SBIdleTimerGlobalStateMonitor", NULL };
    static const char *plain[] = { "resetIdleTimer", "resetLockScreenIdleTimer", "_resetLockScreenIdleTimer", "resetIdleTimerAndUndim", NULL };
    static int logged;
    for (int ci = 0; classes[ci]; ci++) {
        Class c = objc_getClass(classes[ci]);
        if (!c || !class_respondsToSelector(object_getClass((id)c), sel_registerName("sharedInstance"))) continue;
        id inst = ((id (*)(id, SEL))objc_msgSend)((id)c, sel_registerName("sharedInstance"));
        if (!inst) continue;
        for (int i = 0; plain[i]; i++) {
            SEL s = sel_registerName(plain[i]);
            if (class_respondsToSelector(c, s)) {
                ((void (*)(id, SEL))objc_msgSend)(inst, s);
                if (!logged) { logged = 1; logf_("idle timer reset with -[%s %s]", classes[ci], plain[i]); }
                return;
            }
        }
        SEL s1 = sel_registerName("resetIdleTimerAndUndim:");
        if (class_respondsToSelector(c, s1)) {
            ((void (*)(id, SEL, char))objc_msgSend)(inst, s1, 1);
            if (!logged) { logged = 1; logf_("idle timer reset with -[%s resetIdleTimerAndUndim:]", classes[ci]); }
            return;
        }
    }
    if (!logged) { logged = 1; logf_("no idle timer method found"); }
}

// A dump of the SpringBoard classes whose names suggest gestures, Control Center, the cover sheet, or the idle timer.
static void dump_classes(void) {
    FILE *f = fopen("/tmp/fbmode12.dump", "w");
    if (!f) return;
    unsigned n = 0; Class *all = objc_copyClassList(&n);
    static const char *pats[] = { "ControlCenter", "CoverSheet", "SystemGesture", "IdleTimer", "BacklightController", "HomeGesture", "MainSwitcher", NULL };
    for (unsigned i = 0; i < n; i++) {
        const char *name = class_getName(all[i]);
        int hit = 0;
        for (int p = 0; pats[p]; p++) if (strstr(name, pats[p])) hit = 1;
        if (!hit || strncmp(name, "SB", 2) != 0) continue;
        fprintf(f, "%s\n", name);
        unsigned mc = 0; Method *ms = class_copyMethodList(all[i], &mc);
        for (unsigned j = 0; j < mc; j++) fprintf(f, "    -%s  [%s]\n", sel_getName(method_getName(ms[j])), method_getTypeEncoding(ms[j]));
        free(ms);
    }
    free(all); fclose(f);
}

static int g_installed;
static void install_hooks(void) {
    if (g_installed) return;
    if (!objc_getClass("UIApplication") || !objc_getClass("UIWindow")) return;           // UIKit not ready yet
    g_installed = 1;
    HIDType = (int (*)(void *))dlsym(RTLD_DEFAULT, "IOHIDEventGetType");
    logf_("IOHIDEventGetType = %p", (void *)HIDType);
    hook_one("UIApplication", "_handleHIDEvent:", (IMP)new_app_hid, (IMP *)&orig_app_hid);
    hook_one("UIApplication", "sendEvent:", (IMP)new_app_send, (IMP *)&orig_app_send);
    hook_one("UIWindow", "sendEvent:", (IMP)new_win_send, (IMP *)&orig_win_send);
    hook_one("UIWindow", "setHidden:", (IMP)new_setHidden, (IMP *)&orig_setHidden);
    dump_classes();
}

static void tick(CFRunLoopTimerRef t, void *info) {
    install_hooks();
    int a = active();
    if (a != g_was_active) {
        struct stat st; int sr = stat(FLAG, &st);
        logf_("flag %s (stat rc %d, dropped so far %ld)", a ? "ON" : "OFF", sr, g_dropped);
        if (a) hide_all_windows(); else show_hidden_windows();
        g_was_active = a;
    }
    if (a) reset_idle();
}

__attribute__((constructor)) static void fbmode12_init(void) {
    logf_("FBMode12 loaded in pid %d", (int)getpid());
    CFRunLoopTimerRef t = CFRunLoopTimerCreate(NULL, CFAbsoluteTimeGetCurrent() + 2, 1.0, 0, 0, tick, NULL);
    CFRunLoopAddTimer(CFRunLoopGetMain(), t, kCFRunLoopCommonModes);
}
