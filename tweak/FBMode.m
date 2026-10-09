// FBMode.m - a SpringBoard-only tweak (MobileSubstrate) for the framebuffer programs.
//
// While /tmp/fbmode exists and its modification time is under 5 seconds old, SpringBoard
//   1. drops touch events, so taps cannot reach the home screen hidden under our overlay, and
//   2. gets its idle timer reset every few seconds, so the screen does not auto-lock.
// Programs create the file, touch it about once a second, and delete it when they end. If a program dies,
// SpringBoard takes touches again within 5 seconds. With no flag the tweak does nothing.
// Details go to /tmp/fbmode.log (hooks installed, idle-timer methods found, events dropped).
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define FLAG "/tmp/fbmode"
#define LOGFILE "/tmp/fbmode.log"

static int (*HIDType)(void *);
static long g_dropped;
static int g_was_active;

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

static int is_touch(void *ev) { return HIDType && ev && HIDType(ev) == 11; }   // kIOHIDEventTypeDigitizer

#define HOOK_HID(NAME) \
    static void (*orig_##NAME)(id, SEL, void *); \
    static void new_##NAME(id self, SEL cmd, void *ev) { \
        if (active() && is_touch(ev)) { g_dropped++; return; } \
        orig_##NAME(self, cmd, ev); }
static int is_touch_uievent(id ev) {                            // UIEventTypeTouches == 0
    return ev && ((long (*)(id, SEL))objc_msgSend)(ev, sel_registerName("type")) == 0;
}
#define HOOK_SEND(NAME) \
    static void (*orig_##NAME)(id, SEL, id); \
    static void new_##NAME(id self, SEL cmd, id ev) { \
        if (active() && is_touch_uievent(ev)) { g_dropped++; return; } \
        orig_##NAME(self, cmd, ev); }

HOOK_HID(app_hid)
HOOK_HID(sb_hid)
HOOK_SEND(app_send)
HOOK_SEND(sb_send)
HOOK_SEND(win_send)

// System swipe gestures (Control Center from the bottom edge, Notification Center from the top edge) are handled
// by SpringBoard classes that do not use the touch path above. While the flag is fresh they refuse to start.
#define HOOK_BOOL0(NAME) \
    static BOOL (*orig_##NAME)(id, SEL); \
    static BOOL new_##NAME(id self, SEL cmd) { if (active()) { g_dropped++; return NO; } return orig_##NAME(self, cmd); }
#define HOOK_BOOL1(NAME) \
    static BOOL (*orig_##NAME)(id, SEL, id); \
    static BOOL new_##NAME(id self, SEL cmd, id a) { if (active()) { g_dropped++; return NO; } return orig_##NAME(self, cmd, a); }
#define HOOK_VOID_B(NAME) \
    static void (*orig_##NAME)(id, SEL, BOOL); \
    static void new_##NAME(id self, SEL cmd, BOOL a) { if (active()) { g_dropped++; return; } orig_##NAME(self, cmd, a); }
#define HOOK_VOID_BB(NAME) \
    static void (*orig_##NAME)(id, SEL, BOOL, id); \
    static void new_##NAME(id self, SEL cmd, BOOL a, id b) { if (active()) { g_dropped++; return; } orig_##NAME(self, cmd, a, b); }

// The gesture's "begin" stage takes a touch location (a CGPoint: two floats). Block it before SpringBoard starts any
// transition, which would show as a flicker. The return type differs per method, so the install code picks void or BOOL.
typedef struct { float x, y; } FBPoint;
#define HOOK_PT(NAME) \
    static void (*orig_##NAME##_v)(id, SEL, FBPoint); \
    static void new_##NAME##_v(id self, SEL cmd, FBPoint p) { if (active()) { g_dropped++; return; } orig_##NAME##_v(self, cmd, p); } \
    static BOOL (*orig_##NAME##_b)(id, SEL, FBPoint); \
    static BOOL new_##NAME##_b(id self, SEL cmd, FBPoint p) { if (active()) { g_dropped++; return NO; } return orig_##NAME##_b(self, cmd, p); }
HOOK_PT(nc_begin)
HOOK_PT(cc_begin)
HOOK_PT(cc_trans)

HOOK_BOOL0(cc_allow)
HOOK_BOOL0(cc_allow2)
HOOK_VOID_B(cc_present)
HOOK_VOID_BB(cc_present_c)
HOOK_BOOL1(nc_should_begin)
HOOK_BOOL0(nc_grabber)
HOOK_VOID_B(nc_present)
HOOK_VOID_BB(nc_present_c)

// Silent mode: while the flag is fresh every SpringBoard window is hidden, and a window that something tries to show
// (Notification Center, Control Center, a banner) is forced to stay hidden. SpringBoard then has nothing to draw,
// so it cannot fight a program for the display. When the flag ends, all hidden windows are shown again.
static CFMutableArrayRef g_hidden;
static void (*orig_setHidden)(id, SEL, BOOL);
static long g_forced_hidden;

static void remember(id w) {
    if (!g_hidden) g_hidden = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
    CFRange r = CFRangeMake(0, CFArrayGetCount(g_hidden));
    if (!CFArrayContainsValue(g_hidden, r, w)) CFArrayAppendValue(g_hidden, w);
}

static void new_setHidden(id self, SEL cmd, BOOL h) {
    if (active() && !h) {                                       // someone wants this window visible
        remember(self);
        g_forced_hidden++;
        orig_setHidden(self, cmd, 1);
        return;
    }
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
    logf_("silent mode off: showed %ld windows (forced hidden %ld times)", n, g_forced_hidden);
    CFArrayRemoveAllValues(g_hidden);
    g_forced_hidden = 0;
}

static void hook_one(const char *cls, const char *sel, IMP hook, IMP *orig, const char *parent_cls) {
    Class c = objc_getClass(cls);
    if (!c) { logf_("hook %s %s: class not found", cls, sel); return; }
    SEL s = sel_registerName(sel);
    Method m = class_getInstanceMethod(c, s);
    if (!m) { logf_("hook %s %s: method not found", cls, sel); return; }
    if (parent_cls) {                                  // skip if the subclass only inherits the parent's method
        Class p = objc_getClass(parent_cls);
        Method pm = p ? class_getInstanceMethod(p, s) : NULL;
        if (pm && method_getImplementation(pm) == method_getImplementation(m)) { logf_("hook %s %s: inherited from %s, skipped", cls, sel, parent_cls); return; }
    }
    *orig = method_getImplementation(m);                              // the original (own or inherited) implementation
    class_replaceMethod(c, s, hook, method_getTypeEncoding(m));       // plain runtime swizzle, no Substrate API needed
    logf_("hooked %s %s", cls, sel);
}

static void hook_point(const char *cls, const char *sel, IMP nv, IMP *ov, IMP nb, IMP *ob) {
    Class c = objc_getClass(cls);
    Method m = c ? class_getInstanceMethod(c, sel_registerName(sel)) : NULL;
    if (!m) { logf_("hook %s %s: not found", cls, sel); return; }
    const char *enc = method_getTypeEncoding(m);
    if (enc && enc[0] == 'v') hook_one(cls, sel, nv, ov, NULL);
    else if (enc && (enc[0] == 'c' || enc[0] == 'B')) hook_one(cls, sel, nb, ob, NULL);
    else logf_("hook %s %s: unsupported return type %s", cls, sel, enc ? enc : "?");
}

static int g_installed;
static void install_hooks(void) {
    if (g_installed) return;
    if (!objc_getClass("UIApplication") || !objc_getClass("UIWindow")) return;     // UIKit not ready yet
    g_installed = 1;
    hook_one("UIApplication", "_handleHIDEvent:", (IMP)new_app_hid, (IMP *)&orig_app_hid, NULL);
    hook_one("SpringBoard", "_handleHIDEvent:", (IMP)new_sb_hid, (IMP *)&orig_sb_hid, "UIApplication");
    hook_one("UIApplication", "sendEvent:", (IMP)new_app_send, (IMP *)&orig_app_send, NULL);
    hook_one("SpringBoard", "sendEvent:", (IMP)new_sb_send, (IMP *)&orig_sb_send, "UIApplication");
    hook_one("UIWindow", "sendEvent:", (IMP)new_win_send, (IMP *)&orig_win_send, NULL);
    hook_one("UIWindow", "setHidden:", (IMP)new_setHidden, (IMP *)&orig_setHidden, NULL);
    hook_point("SBNotificationCenterController", "beginPresentationWithTouchLocation:", (IMP)new_nc_begin_v, (IMP *)&orig_nc_begin_v, (IMP)new_nc_begin_b, (IMP *)&orig_nc_begin_b);
    hook_point("SBControlCenterController", "beginPresentationWithTouchLocation:", (IMP)new_cc_begin_v, (IMP *)&orig_cc_begin_v, (IMP)new_cc_begin_b, (IMP *)&orig_cc_begin_b);
    hook_point("SBControlCenterController", "beginTransitionWithTouchLocation:", (IMP)new_cc_trans_v, (IMP *)&orig_cc_trans_v, (IMP)new_cc_trans_b, (IMP *)&orig_cc_trans_b);
    hook_one("SBControlCenterController", "allowShowTransitionSystemGesture", (IMP)new_cc_allow, (IMP *)&orig_cc_allow, NULL);
    hook_one("SBControlCenterController", "_allowShowTransitionSystemGesture", (IMP)new_cc_allow2, (IMP *)&orig_cc_allow2, NULL);
    hook_one("SBControlCenterController", "presentAnimated:", (IMP)new_cc_present, (IMP *)&orig_cc_present, NULL);
    hook_one("SBControlCenterController", "presentAnimated:completion:", (IMP)new_cc_present_c, (IMP *)&orig_cc_present_c, NULL);
    hook_one("SBNotificationCenterController", "gestureRecognizerShouldBegin:", (IMP)new_nc_should_begin, (IMP *)&orig_nc_should_begin, NULL);
    hook_one("SBNotificationCenterController", "isGrabberEnabled", (IMP)new_nc_grabber, (IMP *)&orig_nc_grabber, NULL);
    hook_one("SBNotificationCenterController", "presentAnimated:", (IMP)new_nc_present, (IMP *)&orig_nc_present, NULL);
    hook_one("SBNotificationCenterController", "presentAnimated:completion:", (IMP)new_nc_present_c, (IMP *)&orig_nc_present_c, NULL);
}

static void dump_idle_methods(void) {
    static const char *classes[] = { "SBBacklightController", "SBLockScreenManager", "SBAwayController", "SBIdleTimerGlobalCoordinator" };
    for (unsigned k = 0; k < sizeof classes / sizeof classes[0]; k++) {
        Class c = objc_getClass(classes[k]);
        if (!c) { logf_("class %s: not found", classes[k]); continue; }
        unsigned n = 0; Method *ms = class_copyMethodList(c, &n);
        for (unsigned i = 0; i < n; i++) {
            const char *name = sel_getName(method_getName(ms[i]));
            if (strcasestr(name, "idle") || strcasestr(name, "dim") || strcasestr(name, "autolock") || strcasestr(name, "userevent"))
                logf_("%s -%s", classes[k], name);
        }
        free(ms);
    }
}

// One-time list of the classes that handle the system swipe gestures (Control Center, Notification Center),
// with their present/begin/gesture methods, so the tweak can block them.
static void dump_gesture_classes(void) {
    unsigned total = objc_getClassList(NULL, 0);
    Class *all = (Class *)malloc(sizeof(Class) * total);
    total = objc_getClassList(all, total);
    for (unsigned i = 0; i < total; i++) {
        const char *cn = class_getName(all[i]);
        if (!(strstr(cn, "ControlCenter") || strstr(cn, "NotificationCenter") || strstr(cn, "SystemGesture") ||
              strstr(cn, "GestureManager") || strstr(cn, "BottomEdge") || strstr(cn, "EdgeSwipe"))) continue;
        unsigned n = 0; Method *ms = class_copyMethodList(all[i], &n);
        logf_("CLASS %s (%u methods)", cn, n);
        for (unsigned k = 0; k < n; k++) {
            const char *name = sel_getName(method_getName(ms[k]));
            if (strcasestr(name, "present") || strcasestr(name, "beginTransition") || strcasestr(name, "begin") ||
                strcasestr(name, "gesture") || strcasestr(name, "activate") || strcasestr(name, "allowed") || strcasestr(name, "enabled"))
                logf_("   -%s", name);
        }
        free(ms);
    }
    free(all);
}

static id backlight_controller(void) {
    Class c = objc_getClass("SBBacklightController");
    return c ? ((id (*)(id, SEL))objc_msgSend)((id)c, sel_registerName("sharedInstance")) : NULL;
}

static void call_void(id obj, const char *sel) {
    SEL s = sel_registerName(sel);
    if (obj && class_respondsToSelector(object_getClass(obj), s)) ((void (*)(id, SEL))objc_msgSend)(obj, s);
}
static void call_bool(id obj, const char *sel, int v) {
    SEL s = sel_registerName(sel);
    if (obj && class_respondsToSelector(object_getClass(obj), s)) ((void (*)(id, SEL, char))objc_msgSend)(obj, s, (char)v);
}

// Keep the screen on: no idle dimming, no auto-lock, no idle sleep. Undo it all when the flag ends.
static void idle_prevent(int on) {
    id b = backlight_controller();
    if (!b) { logf_("idle: no SBBacklightController"); return; }
    SEL sf = sel_registerName("setIdleTimerDisabled:forReason:");
    if (class_respondsToSelector(object_getClass(b), sf)) ((void (*)(id, SEL, char, id))objc_msgSend)(b, sf, (char)on, (id)CFSTR("FBMode"));
    call_bool(b, "setIdleTimerDisabled:", on);
    call_void(b, on ? "preventIdleSleep" : "allowIdleSleep");
    if (on) call_bool(b, "resetIdleTimerAndUndim:", 1);
    logf_("idle sleep %s", on ? "prevented" : "allowed again");
}

static void tick(CFRunLoopTimerRef t, void *info) {
    install_hooks();                                    // retry until UIKit classes exist
    int a = active();
    if (a != g_was_active) {
        struct stat st; int sr = stat(FLAG, &st);
        if (sr != 0) logf_("flag %s: file missing (stat errno %d)", a ? "ON" : "OFF", errno);
        else logf_("flag %s: file age %ld s", a ? "ON" : "OFF", (long)(time(NULL) - st.st_mtime));
        logf_("flag %s (dropped so far: %ld)", a ? "ON" : "OFF", g_dropped);
        if (a) { hide_all_windows(); idle_prevent(1); } else { show_hidden_windows(); idle_prevent(0); }
        if (a) { static int dumped; if (!dumped && access("/tmp/fbmode_dump", F_OK) == 0) { dumped = 1; dump_gesture_classes(); dump_idle_methods(); } }
        g_was_active = a;
    }
    if (!a) return;
    call_bool(backlight_controller(), "resetIdleTimerAndUndim:", 1);
    Class c = objc_getClass("SBBacklightController");
    if (!c) return;
    id inst = ((id (*)(id, SEL))objc_msgSend)((id)c, sel_registerName("sharedInstance"));
    if (!inst) return;
    static const char *plain[] = { "resetIdleTimer", "resetLockScreenIdleTimer", "_resetLockScreenIdleTimer", "resetIdleTimerAndUndim", NULL };
    for (int i = 0; plain[i]; i++) {
        SEL s = sel_registerName(plain[i]);
        if (class_respondsToSelector(c, s)) { ((void (*)(id, SEL))objc_msgSend)(inst, s); return; }
    }
    SEL s1 = sel_registerName("resetIdleTimerAndUndim:");
    if (class_respondsToSelector(c, s1)) ((void (*)(id, SEL, char))objc_msgSend)(inst, s1, 1);
}

__attribute__((constructor)) static void fbmode_init(void) {
    HIDType = (int (*)(void *))dlsym(RTLD_DEFAULT, "IOHIDEventGetType");
    logf_("FBMode loaded in pid %d: IOHIDEventGetType=%p", (int)getpid(), (void *)HIDType);
    install_hooks();
    CFRunLoopTimerRef t = CFRunLoopTimerCreate(NULL, CFAbsoluteTimeGetCurrent() + 2, 3.0, 0, 0, tick, NULL);
    CFRunLoopAddTimer(CFRunLoopGetMain(), t, kCFRunLoopCommonModes);
}
