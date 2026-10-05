#include <ApplicationServices/ApplicationServices.h>
#include <lauxlib.h>
#include <lua.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#define MODULE "portable-mouse-crossing-hid"
#define MAX_SCREENS 16

typedef struct {
    double x, y, w, h;
} ScreenRect;

typedef struct {
    ScreenRect frame, physical;
} Screen;

typedef struct {
    Screen screens[MAX_SCREENS];
    int count, previous;
    pthread_t thread;
    pthread_mutex_t lock;
    pthread_cond_t ready;
    bool started, readyFlag;
    CFRunLoopRef loop;
    CFMachPortRef tap;
    atomic_bool enabled;
    atomic_uint_fast64_t events, crossings;
} Mapper;

static double clamp(double value, double minimum, double maximum) {
    return fmax(minimum, fmin(maximum, value));
}

static int screenAt(Mapper *m, CGPoint point) {
    for (int i = 0; i < m->count; ++i) {
        ScreenRect f = m->screens[i].frame;
        if (point.x >= f.x && point.x < f.x + f.w && point.y >= f.y && point.y < f.y + f.h) return i;
    }
    return -1;
}

static CGEventRef handle(CGEventTapProxy proxy, CGEventType type, CGEventRef event, void *context) {
    (void)proxy;
    Mapper *m = context;
    if (type == kCGEventTapDisabledByTimeout || type == kCGEventTapDisabledByUserInput) {
        CGEventTapEnable(m->tap, true);
        return event;
    }
    CGPoint point = CGEventGetLocation(event);
    int current = screenAt(m, point);
    atomic_fetch_add(&m->events, 1);
    if (!atomic_load(&m->enabled) || type != kCGEventMouseMoved) {
        m->previous = current;
        return event;
    }
    int source = m->previous >= 0 ? m->previous : current;
    if (source < 0) return event;
    double dx = CGEventGetDoubleValueField(event, kCGMouseEventDeltaX);
    double dy = CGEventGetDoubleValueField(event, kCGMouseEventDeltaY);
    ScreenRect f = m->screens[source].frame;
    ScreenRect p = m->screens[source].physical;
    int axis = -1, direction = 0;
    if (dx < 0 && point.x <= f.x) { axis = 0; direction = -1; }
    else if (dx > 0 && point.x >= f.x + f.w - 1) { axis = 0; direction = 1; }
    else if (dy < 0 && point.y <= f.y) { axis = 1; direction = -1; }
    else if (dy > 0 && point.y >= f.y + f.h - 1) { axis = 1; direction = 1; }
    if (axis < 0) {
        m->previous = current >= 0 ? current : source;
        return event;
    }
    double edge = axis == 0 ? f.x + (direction > 0 ? f.w : 0) : f.y + (direction > 0 ? f.h : 0);
    double position = axis == 0 ? p.y + (point.y - f.y) / f.h * p.h : p.x + (point.x - f.x) / f.w * p.w;
    int target = -1;
    double bestDistance = INFINITY;
    for (int i = 0; i < m->count; ++i) {
        if (i == source) continue;
        ScreenRect t = m->screens[i].frame, tp = m->screens[i].physical;
        double targetEdge = axis == 0 ? t.x + (direction > 0 ? 0 : t.w) : t.y + (direction > 0 ? 0 : t.h);
        double overlap = axis == 0 ? fmin(f.y + f.h, t.y + t.h) - fmax(f.y, t.y) : fmin(f.x + f.w, t.x + t.w) - fmax(f.x, t.x);
        if (fabs(edge - targetEdge) > 2 || overlap <= 0) continue;
        double nearest = axis == 0 ? clamp(position, tp.y, tp.y + tp.h) : clamp(position, tp.x, tp.x + tp.w);
        double distance = fabs(position - nearest);
        if (distance < bestDistance) { target = i; bestDistance = distance; }
    }
    if (target < 0) {
        m->previous = current >= 0 ? current : source;
        return event;
    }
    ScreenRect t = m->screens[target].frame, tp = m->screens[target].physical;
    CGPoint mapped;
    if (axis == 0) {
        mapped = CGPointMake(clamp(point.x, t.x + 1, t.x + t.w - 1), clamp(t.y + (position - tp.y) / tp.h * t.h, t.y + 2, t.y + t.h - 2));
    } else {
        mapped = CGPointMake(clamp(t.x + (position - tp.x) / tp.w * t.w, t.x + 2, t.x + t.w - 2), clamp(point.y, t.y + 1, t.y + t.h - 1));
    }
    if (CGWarpMouseCursorPosition(mapped) == kCGErrorSuccess) {
        CGAssociateMouseAndMouseCursorPosition(true);
        CGEventSetLocation(event, mapped);
        m->previous = target;
        atomic_fetch_add(&m->crossings, 1);
    } else {
        m->previous = current;
    }
    return event;
}

static void *run(void *context) {
    Mapper *m = context;
    CGEventMask mask = CGEventMaskBit(kCGEventMouseMoved) | CGEventMaskBit(kCGEventLeftMouseDragged)
        | CGEventMaskBit(kCGEventRightMouseDragged) | CGEventMaskBit(kCGEventOtherMouseDragged);
    CFMachPortRef tap = CGEventTapCreate(kCGHIDEventTap, kCGHeadInsertEventTap, kCGEventTapOptionDefault, mask, handle, m);
    CFRunLoopSourceRef source = tap ? CFMachPortCreateRunLoopSource(NULL, tap, 0) : NULL;
    pthread_mutex_lock(&m->lock);
    m->tap = tap;
    m->loop = CFRunLoopGetCurrent();
    CFRetain(m->loop);
    if (source) CFRunLoopAddSource(m->loop, source, kCFRunLoopCommonModes);
    m->readyFlag = true;
    pthread_cond_signal(&m->ready);
    pthread_mutex_unlock(&m->lock);
    if (source) {
        CFRunLoopRun();
        CFRunLoopRemoveSource(m->loop, source, kCFRunLoopCommonModes);
        CFRelease(source);
    }
    return NULL;
}

static void stop(Mapper *m) {
    if (!m->started) return;
    if (m->tap) CGEventTapEnable(m->tap, false);
    CFRunLoopPerformBlock(m->loop, kCFRunLoopCommonModes, ^{ CFRunLoopStop(m->loop); });
    CFRunLoopWakeUp(m->loop);
    pthread_join(m->thread, NULL);
    if (m->tap) { CFMachPortInvalidate(m->tap); CFRelease(m->tap); m->tap = NULL; }
    CFRelease(m->loop);
    m->loop = NULL;
    pthread_mutex_destroy(&m->lock);
    pthread_cond_destroy(&m->ready);
    m->started = false;
}

static int stopLua(lua_State *L) {
    stop(luaL_checkudata(L, 1, MODULE));
    return 0;
}

static int setEnabled(lua_State *L) {
    Mapper *m = luaL_checkudata(L, 1, MODULE);
    atomic_store(&m->enabled, lua_toboolean(L, 2));
    return 0;
}

static int status(lua_State *L) {
    Mapper *m = luaL_checkudata(L, 1, MODULE);
    lua_newtable(L);
    lua_pushboolean(L, m->started && m->tap && CGEventTapIsEnabled(m->tap)); lua_setfield(L, -2, "running");
    lua_pushinteger(L, atomic_load(&m->events)); lua_setfield(L, -2, "events");
    lua_pushinteger(L, atomic_load(&m->crossings)); lua_setfield(L, -2, "crossings");
    lua_pushstring(L, "native-hid-thread"); lua_setfield(L, -2, "backend");
    return 1;
}

static double numberField(lua_State *L, int table, const char *key) {
    lua_getfield(L, table, key);
    double value = luaL_checknumber(L, -1);
    lua_pop(L, 1);
    luaL_argcheck(L, isfinite(value), 1, "screen coordinates must be finite");
    return value;
}

static ScreenRect rectField(lua_State *L, const char *key) {
    lua_getfield(L, -1, key);
    luaL_checktype(L, -1, LUA_TTABLE);
    int table = lua_gettop(L);
    ScreenRect rect = {numberField(L, table, "x"), numberField(L, table, "y"), numberField(L, table, "w"), numberField(L, table, "h")};
    luaL_argcheck(L, rect.w > 2 && rect.h > 2, 1, "screen dimensions must exceed two");
    lua_pop(L, 1);
    return rect;
}

static int start(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    size_t count = lua_rawlen(L, 1);
    luaL_argcheck(L, count <= MAX_SCREENS, 1, "too many screens");
    Screen screens[MAX_SCREENS];
    for (size_t i = 0; i < count; ++i) {
        lua_rawgeti(L, 1, i + 1);
        luaL_checktype(L, -1, LUA_TTABLE);
        screens[i].frame = rectField(L, "frame");
        screens[i].physical = rectField(L, "physical");
        lua_pop(L, 1);
    }
    if (CGSetLocalEventsSuppressionInterval(0) != kCGErrorSuccess) {
        return luaL_error(L, "cannot disable cursor-warp input suppression");
    }
    Mapper *m = lua_newuserdatauv(L, sizeof(*m), 0);
    *m = (Mapper){0};
    luaL_setmetatable(L, MODULE);
    m->count = (int)count;
    m->previous = -1;
    for (size_t i = 0; i < count; ++i) m->screens[i] = screens[i];
    atomic_init(&m->enabled, true);
    atomic_init(&m->events, 0);
    atomic_init(&m->crossings, 0);
    pthread_mutex_init(&m->lock, NULL);
    pthread_cond_init(&m->ready, NULL);
    if (pthread_create(&m->thread, NULL, run, m) != 0) {
        pthread_mutex_destroy(&m->lock);
        pthread_cond_destroy(&m->ready);
        return luaL_error(L, "cannot start cursor thread");
    }
    m->started = true;
    pthread_mutex_lock(&m->lock);
    while (!m->readyFlag) pthread_cond_wait(&m->ready, &m->lock);
    pthread_mutex_unlock(&m->lock);
    if (!m->tap) { stop(m); return luaL_error(L, "cannot create HID mouse event tap"); }
    return 1;
}

int luaopen_portable_mouse_crossing_hid(lua_State *L) {
    luaL_newmetatable(L, MODULE);
    lua_pushcfunction(L, stopLua); lua_setfield(L, -2, "__gc");
    lua_newtable(L);
    lua_pushcfunction(L, stopLua); lua_setfield(L, -2, "stop");
    lua_pushcfunction(L, setEnabled); lua_setfield(L, -2, "setEnabled");
    lua_pushcfunction(L, status); lua_setfield(L, -2, "status");
    lua_setfield(L, -2, "__index");
    lua_pop(L, 1);
    lua_newtable(L);
    lua_pushcfunction(L, start); lua_setfield(L, -2, "start");
    return 1;
}
