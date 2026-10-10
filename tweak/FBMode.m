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
#include <pthread.h>
#include <spawn.h>
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

// Kiosk mode: the flag file is not empty (the launcher writes one byte). The tweak then also blocks the home and
// power buttons, so there is no app switcher and no lock screen to get stuck on.
static int kiosk_flag(void) {
    struct stat st;
    return stat(FLAG, &st) == 0 && st.st_size > 0 && (time(NULL) - st.st_mtime) < 5;
}
static int (*HIDInt)(void *, unsigned);
static int wanted(void) { return access("/var/mobile/fbkiosk_on", F_OK) == 0; }   // the kiosk switch (kiosk.sh on)
static void screen_toggle(void);                 // defined below, after the backlight helpers
static int g_btn_logged;

// Button events are keyboard-type HID events (type 3). Field 0x30000 = usage page, 0x30001 = usage, 0x30002 = down.
static int is_blocked_button(void *ev) {
    if (!kiosk_flag() || !HIDType || !ev || HIDType(ev) != 3 || !HIDInt) return 0;
    int page = HIDInt(ev, 0x30000), usage = HIDInt(ev, 0x30001), down = HIDInt(ev, 0x30002);
    if (g_btn_logged < 60) { g_btn_logged++; logf_("button event: page 0x%x usage 0x%x down %d", page, usage, down); }
    if (page == 0x0C && usage == 0x30) {                            // power button: toggle the screen, never lock
        if (down) screen_toggle();
        return 1;
    }
    return page == 0x0C && usage == 0x40;                           // home button
}

static int is_touch(void *ev) { return HIDType && ev && HIDType(ev) == 11; }   // kIOHIDEventTypeDigitizer

#define HOOK_HID(NAME) \
    static void (*orig_##NAME)(id, SEL, void *); \
    static void new_##NAME(id self, SEL cmd, void *ev) { \
        if (active() && (is_touch(ev) || is_blocked_button(ev))) { g_dropped++; return; } \
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

// Kiosk backstop: SpringBoard must not lock the device from any source (button, timer, a call) while the kiosk runs.
static void (*orig_lock)(id, SEL, int, id);
static void new_lock(id self, SEL cmd, int src, id opts) {
    if ((active() && kiosk_flag()) || wanted()) { g_dropped++; return; }
    orig_lock(self, cmd, src, opts);
}

// Home button, blocked at its source while the kiosk runs. Signatures come from the method dump:
// SpringBoard: _menuButtonDown:/_menuButtonUp:/menuButtonDown:/menuButtonUp: take one pointer, handleMenuDoubleTap and
// _handleMenuButtonEvent and _menuButtonWasHeld take nothing, isMenuDoubleTapAllowed returns BOOL.
// SBUIController: clickedMenuButton and handleMenuDoubleTap return BOOL (YES = "handled"), _toggleSwitcher returns void,
// _activateAppSwitcherFromSide: takes an int and returns BOOL.
#define KHOOK_V1(NAME) \
    static void (*o_##NAME)(id, SEL, void *); \
    static void n_##NAME(id s, SEL c, void *a) { if (active() && kiosk_flag()) { g_dropped++; return; } o_##NAME(s, c, a); }
#define KHOOK_V0(NAME) \
    static void (*o_##NAME)(id, SEL); \
    static void n_##NAME(id s, SEL c) { if (active() && kiosk_flag()) { g_dropped++; return; } o_##NAME(s, c); }
#define KHOOK_B0(NAME, RET) \
    static BOOL (*o_##NAME)(id, SEL); \
    static BOOL n_##NAME(id s, SEL c) { if (active() && kiosk_flag()) { g_dropped++; return RET; } return o_##NAME(s, c); }
#define KHOOK_B1I(NAME, RET) \
    static BOOL (*o_##NAME)(id, SEL, int); \
    static BOOL n_##NAME(id s, SEL c, int a) { if (active() && kiosk_flag()) { g_dropped++; return RET; } return o_##NAME(s, c, a); }

KHOOK_V1(sb_menu_down)
KHOOK_V1(sb_menu_up)
KHOOK_V1(sb_menu_down_gs)
KHOOK_V1(sb_menu_up_gs)
KHOOK_V0(sb_menu_double)
KHOOK_V0(sb_menu_event)
KHOOK_V0(sb_menu_held)
KHOOK_B0(sb_double_allowed, NO)
KHOOK_B0(ui_clicked_menu, YES)
KHOOK_B0(ui_menu_double, YES)
KHOOK_V0(ui_toggle_switcher)
KHOOK_B1I(ui_switcher_side, NO)

// The lock screen at boot is created by lower-level calls than lockUIFromSource:. While the kiosk is wanted, SpringBoard
// must never enter the locked state at all, so refuse those too. Signatures come from the method dump.
static void (*orig_set_locked)(id, SEL, char);
static void new_set_locked(id self, SEL cmd, char locked) { if (locked && wanted()) { g_dropped++; return; } orig_set_locked(self, cmd, locked); }
static void (*orig_lock_ui)(id, SEL);
static void new_lock_ui(id self, SEL cmd) { if (wanted()) { g_dropped++; return; } orig_lock_ui(self, cmd); }
static void (*orig_activate_ls)(id, SEL, char, char, char, char, char);
static void new_activate_ls(id self, SEL cmd, char a, char b, char c, char d, char e) {
    if (wanted()) { g_dropped++; return; }
    orig_activate_ls(self, cmd, a, b, c, d, e);
}

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
    {
        Class lc = objc_getClass("SBLockScreenManager");
        Method lm = lc ? class_getInstanceMethod(lc, sel_registerName("lockUIFromSource:withOptions:")) : NULL;
        const char *enc = lm ? method_getTypeEncoding(lm) : NULL;
        if (enc && enc[0] == 'v') hook_one("SBLockScreenManager", "lockUIFromSource:withOptions:", (IMP)new_lock, (IMP *)&orig_lock, NULL);
        else logf_("hook SBLockScreenManager lockUIFromSource:withOptions:: %s", lm ? "unexpected return type" : "not found");
    }
    hook_one("SBLockScreenManager", "_setUILocked:", (IMP)new_set_locked, (IMP *)&orig_set_locked, NULL);
    hook_one("SBLockScreenManager", "_lockUI", (IMP)new_lock_ui, (IMP *)&orig_lock_ui, NULL);
    hook_one("SBLockScreenManager", "_activateLockScreenAnimated:automatically:inScreenOffMode:dimInAnimation:dismissNotificationCenter:", (IMP)new_activate_ls, (IMP *)&orig_activate_ls, NULL);
    hook_one("SpringBoard", "_menuButtonDown:", (IMP)n_sb_menu_down, (IMP *)&o_sb_menu_down, NULL);
    hook_one("SpringBoard", "_menuButtonUp:", (IMP)n_sb_menu_up, (IMP *)&o_sb_menu_up, NULL);
    hook_one("SpringBoard", "menuButtonDown:", (IMP)n_sb_menu_down_gs, (IMP *)&o_sb_menu_down_gs, NULL);
    hook_one("SpringBoard", "menuButtonUp:", (IMP)n_sb_menu_up_gs, (IMP *)&o_sb_menu_up_gs, NULL);
    hook_one("SpringBoard", "handleMenuDoubleTap", (IMP)n_sb_menu_double, (IMP *)&o_sb_menu_double, NULL);
    hook_one("SpringBoard", "_handleMenuButtonEvent", (IMP)n_sb_menu_event, (IMP *)&o_sb_menu_event, NULL);
    hook_one("SpringBoard", "_menuButtonWasHeld", (IMP)n_sb_menu_held, (IMP *)&o_sb_menu_held, NULL);
    hook_one("SpringBoard", "isMenuDoubleTapAllowed", (IMP)n_sb_double_allowed, (IMP *)&o_sb_double_allowed, NULL);
    hook_one("SBUIController", "clickedMenuButton", (IMP)n_ui_clicked_menu, (IMP *)&o_ui_clicked_menu, NULL);
    hook_one("SBUIController", "handleMenuDoubleTap", (IMP)n_ui_menu_double, (IMP *)&o_ui_menu_double, NULL);
    hook_one("SBUIController", "_toggleSwitcher", (IMP)n_ui_toggle_switcher, (IMP *)&o_ui_toggle_switcher, NULL);
    hook_one("SBUIController", "_activateAppSwitcherFromSide:", (IMP)n_ui_switcher_side, (IMP *)&o_ui_switcher_side, NULL);
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

// One-time list of home-button and app-switcher methods, so the tweak can block the home button at its source.
static void dump_button_methods(void) {
    static const char *classes[] = { "SpringBoard", "SBUIController", "SBAppSwitcherController", "SBHomeButtonController",
                                     "SBPressGestureRecognizer", "SBMenuButtonController", "SBMainSwitcherViewController" };
    for (unsigned k = 0; k < sizeof classes / sizeof classes[0]; k++) {
        Class c = objc_getClass(classes[k]);
        if (!c) { logf_("class %s: not found", classes[k]); continue; }
        unsigned n = 0; Method *ms = class_copyMethodList(c, &n);
        logf_("CLASS %s (%u methods)", classes[k], n);
        for (unsigned i = 0; i < n; i++) {
            const char *name = sel_getName(method_getName(ms[i]));
            if (strcasestr(name, "menu") || strcasestr(name, "home") || strcasestr(name, "switcher") ||
                strcasestr(name, "doubletap") || strcasestr(name, "doublepress") || strcasestr(name, "buttonDown") || strcasestr(name, "buttonUp"))
                logf_("   -%s  [%s]", name, method_getTypeEncoding(ms[i]));
        }
        free(ms);
    }
}

// One-time list of the screen on/off and lock methods, to build a power button that never locks.
static void dump_power_methods(void) {
    static const char *classes[] = { "SBBacklightController", "SBLockScreenManager", "SBAwayController", "SBPowerDownController" };
    for (unsigned k = 0; k < sizeof classes / sizeof classes[0]; k++) {
        Class c = objc_getClass(classes[k]);
        if (!c) { logf_("class %s: not found", classes[k]); continue; }
        unsigned n = 0; Method *ms = class_copyMethodList(c, &n);
        logf_("CLASS %s (%u methods)", classes[k], n);
        for (unsigned i = 0; i < n; i++) {
            const char *name = sel_getName(method_getName(ms[i]));
            if (strcasestr(name, "screen") || strcasestr(name, "backlight") || strcasestr(name, "turn") ||
                strcasestr(name, "sleep") || strcasestr(name, "wake") || strcasestr(name, "lock") || strcasestr(name, "power"))
                logf_("   -%s  [%s]", name, method_getTypeEncoding(ms[i]));
        }
        free(ms);
    }
    Class sb = objc_getClass("SpringBoard");
    unsigned n = 0; Method *ms = sb ? class_copyMethodList(sb, &n) : NULL;
    for (unsigned i = 0; i < n; i++) {
        const char *name = sel_getName(method_getName(ms[i]));
        if (strcasestr(name, "lockButton") || strcasestr(name, "powerButton")) logf_("   SpringBoard -%s  [%s]", name, method_getTypeEncoding(ms[i]));
    }
    free(ms);
}

// Screen off/on without locking. "Off" = backlight factor 0. A marker file tells the programs to ignore touches and skip drawing.
#define SCREEN_OFF_FLAG "/tmp/fbscreen_off"
static int g_screen_off;
static void screen_set(int off) {
    id b = backlight_controller();
    if (!b) return;
    SEL set = sel_registerName("setBacklightFactor:source:");
    int has_set = class_respondsToSelector(object_getClass(b), set);
    if (off) {
        if (has_set) ((void (*)(id, SEL, float, int))objc_msgSend)(b, set, 0.0f, 1);
        FILE *f = fopen(SCREEN_OFF_FLAG, "w"); if (f) fclose(f);
        chmod(SCREEN_OFF_FLAG, 0644);
    } else {
        unlink(SCREEN_OFF_FLAG);
        // Only the brightness was lowered, the display was never blanked. So the brightness is all that has to come back.
        // turnOnScreenFullyWithBacklightSource: waits for the next display commit, which a silent SpringBoard does not
        // make until a timeout, and that was the 1-2 second delay when waking.
        if (has_set) ((void (*)(id, SEL, float, int))objc_msgSend)(b, set, 1.0f, 1);
        else {
            SEL on = sel_registerName("turnOnScreenFullyWithBacklightSource:");
            if (class_respondsToSelector(object_getClass(b), on)) ((void (*)(id, SEL, int))objc_msgSend)(b, on, 1);
        }
    }
    g_screen_off = off;
    logf_("screen %s", off ? "OFF" : "ON");
}
static int sb_screen_off(void) {
    id b = backlight_controller();
    SEL s = sel_registerName("screenIsOff");
    return b && class_respondsToSelector(object_getClass(b), s) && ((char (*)(id, SEL))objc_msgSend)(b, s);
}
static void screen_toggle(void) { screen_set(!(g_screen_off || sb_screen_off())); }

// Start the kiosk ourselves instead of waiting for the late boot job: run the FB Tools stub, which runs as root and
// starts the kiosk loop. Tried again after a minute if no program has shown up.
extern char **environ;
static int g_spawn_tries; static time_t g_last_spawn;
static void spawn_kiosk(void) {
    if (g_spawn_tries >= 5 || time(NULL) - g_last_spawn < 60) return;
    g_spawn_tries++; g_last_spawn = time(NULL);
    pid_t pid = 0; char *argv[] = { "FBTools", NULL };
    int rc = posix_spawn(&pid, "/Applications/FBTools.app/FBTools", NULL, NULL, argv, environ);
    logf_("kiosk wanted, no program running: starting the kiosk (try %d, rc %d)", g_spawn_tries, rc);
}

// Kiosk wanted but no program running yet (the boot gap): no lock screen, screen on.
static int g_was_wanted;
static void unlock_and_wake(void) {
    Class lc = objc_getClass("SBLockScreenManager");
    id lm = lc ? ((id (*)(id, SEL))objc_msgSend)((id)lc, sel_registerName("sharedInstance")) : NULL;
    if (lm && class_respondsToSelector(object_getClass(lm), sel_registerName("isUILocked")) &&
        ((char (*)(id, SEL))objc_msgSend)(lm, sel_registerName("isUILocked"))) {
        SEL un = sel_registerName("unlockUIFromSource:withOptions:");
        if (class_respondsToSelector(object_getClass(lm), un)) ((void (*)(id, SEL, int, id))objc_msgSend)(lm, un, 1, NULL);
        logf_("kiosk wanted: device was locked, unlocking");
    }
    if (!g_screen_off && sb_screen_off()) {
        id b = backlight_controller();
        SEL on = sel_registerName("turnOnScreenFullyWithBacklightSource:");
        if (b && class_respondsToSelector(object_getClass(b), on)) ((void (*)(id, SEL, int))objc_msgSend)(b, on, 1);
        logf_("kiosk wanted: screen was off, turning it on");
    }
}

static void tick(CFRunLoopTimerRef t, void *info) {
    install_hooks();                                    // retry until UIKit classes exist
    if (access("/tmp/fbwake", F_OK) == 0) {            // a program (the alarm) asks for the screen to come on
        unlink("/tmp/fbwake");
        if (g_screen_off || sb_screen_off()) screen_set(0);
    }
    int w = wanted();
    if (w != g_was_wanted) {
        logf_("kiosk %s", w ? "wanted (no lock screen, screen stays on)" : "no longer wanted");
        if (w) idle_prevent(1); else if (!active()) idle_prevent(0);
        g_was_wanted = w;
    }
    if (w && !g_screen_off) unlock_and_wake();
    if (w && !active()) spawn_kiosk();
    int a = active();
    if (a != g_was_active) {
        struct stat st; int sr = stat(FLAG, &st);
        if (sr != 0) logf_("flag %s: file missing (stat errno %d)", a ? "ON" : "OFF", errno);
        else logf_("flag %s: file age %ld s", a ? "ON" : "OFF", (long)(time(NULL) - st.st_mtime));
        logf_("flag %s (dropped so far: %ld)", a ? "ON" : "OFF", g_dropped);
        if (a) { hide_all_windows(); idle_prevent(1); } else { if (g_screen_off) screen_set(0); show_hidden_windows(); idle_prevent(0); }
        if (a) { static int dumped; if (!dumped && access("/tmp/fbmode_dump", F_OK) == 0) { dumped = 1; dump_gesture_classes(); dump_idle_methods(); dump_button_methods(); dump_power_methods(); } }
        g_was_active = a;
    }
    if (!a || g_screen_off) return;
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

// SpringBoard's main run loop is busy for the first ~30 s of its start, so a timer on it cannot start the kiosk early.
// A plain thread can.
static void *early_kiosk_thread(void *arg) {
    (void)arg;
    for (;;) {
        sleep(1);
        if (wanted() && !active()) spawn_kiosk();
    }
    return NULL;
}

__attribute__((constructor)) static void fbmode_init(void) {
    HIDType = (int (*)(void *))dlsym(RTLD_DEFAULT, "IOHIDEventGetType");
    HIDInt = (int (*)(void *, unsigned))dlsym(RTLD_DEFAULT, "IOHIDEventGetIntegerValue");
    logf_("FBMode loaded in pid %d: IOHIDEventGetType=%p", (int)getpid(), (void *)HIDType);
    install_hooks();
    { pthread_t th; if (pthread_create(&th, NULL, early_kiosk_thread, NULL) == 0) pthread_detach(th); }
    CFRunLoopTimerRef t = CFRunLoopTimerCreate(NULL, CFAbsoluteTimeGetCurrent() + 2, 3.0, 0, 0, tick, NULL);
    CFRunLoopAddTimer(CFRunLoopGetMain(), t, kCFRunLoopCommonModes);
}
