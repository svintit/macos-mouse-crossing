/* measure.c - measure actual pointer travel for a fixed stream of relative
   HID input on macOS. Standalone tool. Uses deprecated IOKit HID APIs that
   empirically still work. Does not post keyboard or button events. */

#define _DARWIN_C_SOURCE 1

#include <ApplicationServices/ApplicationServices.h>
#include <IOKit/hidsystem/IOHIDLib.h>
#include <IOKit/hidsystem/IOLLEvent.h>
#include <IOKit/hidsystem/event_status_driver.h>
#include <errno.h>
#include <limits.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <math.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define RESET_MS 350
#define SETTLE_MS 200
#define MAX_EVENTS 1000
#define MAX_REPEATS 10
#define MAX_TRIALS (MAX_REPEATS * 2)
#define MAX_DISPLAYS 16
#define MAX_DURATION_S 60.0

static volatile sig_atomic_t g_stop = 0;
static mach_timebase_info_data_t g_tb;

static void onSignal(int sig) {
    (void)sig;
    g_stop = 1;
}

typedef struct {
    double control_x, control_y;
    double crossing_x, crossing_y;
    int axis; /* 0 = x, 1 = y */
    int step;
    int events;
    int rate;
    int repeats;
    bool have_control, have_crossing, run;
} Options;

typedef struct {
    uint32_t count;
    CGDirectDisplayID ids[MAX_DISPLAYS];
    CGRect bounds[MAX_DISPLAYS];
} Topology;

typedef struct {
    int repeat;
    const char *condition;
    long input_dx;
    long input_dy;
    int events;
    int events_sent;
    double start_x, start_y;
    double end_x, end_y;
    double travel_axis;
    double delta_error;
    double max_lag_ms;
    bool clipped;
    bool interference;
    bool topology_changed;
} Trial;

typedef enum { RUN_OK, RUN_INTERRUPTED, RUN_ABORTED, RUN_API_ERROR } RunStatus;

static void usage(FILE *out) {
    fprintf(out,
        "Usage: measure --control X Y --crossing X Y [options] --run\n"
        "\n"
        "Measure actual pointer travel for a fixed stream of relative mouse\n"
        "input. Posts signed relative HID deltas at a fixed rate. It does not\n"
        "read the cursor to build each move. It restores the original pointer\n"
        "position on exit.\n"
        "\n"
        "Required for movement:\n"
        "  --control X Y   Start point for the control path (logical points).\n"
        "  --crossing X Y  Start point for the crossing path (logical points).\n"
        "  --run           Confirm real pointer movement. Without it, no move.\n"
        "\n"
        "Options:\n"
        "  --axis x|y      Travel axis to compare. Default: x.\n"
        "  --step N        Signed delta per event, abs(N) 1..100. Default: 5.\n"
        "  --events N      Events per trial, 1..1000. Default: 80.\n"
        "  --rate HZ       Events per second, 1..1000. Default: 125.\n"
        "  --repeats N     Paired repeats, 1..10. Default: 3.\n"
        "  -h, --help      Print this help. No permission check. No movement.\n"
        "\n"
        "Limits:\n"
        "  Total intended duration (reset, input, settle) must be <= 60 seconds.\n"
        "  Both start points must be finite and inside the online display bounds.\n"
        "  Requires post-event access. Does not request permission or prompt.\n"
        "\n"
        "Output:\n"
        "  One JSON object on stdout: parameters, per-trial control and crossing\n"
        "  records, and flags for clipping, interference, topology changes.\n"
        "\n"
        "Caveats:\n"
        "  input_counts are raw HID counts. travel_axis is in logical points.\n"
        "  These are different units. Compare crossing travel with the matched\n"
        "  control travel, not an assumed 1:1 conversion.\n"
        "  Injected events use their own HID source. This does not prove\n"
        "  equivalence to a physical mouse or to system acceleration.\n"
        "\n"
        "Keep your hands off the mouse during the run.\n");
}

static void initTimebase(void) {
    (void)mach_timebase_info(&g_tb);
}

static uint64_t nsToMach(double ns) {
    if (g_tb.numer == 0 || g_tb.denom == 0) return (uint64_t)ns;
    return (uint64_t)(ns * (double)g_tb.denom / (double)g_tb.numer);
}

static double machToNs(uint64_t ticks) {
    if (g_tb.numer == 0 || g_tb.denom == 0) return (double)ticks;
    return (double)ticks * (double)g_tb.numer / (double)g_tb.denom;
}

/* Sleep until an absolute mach deadline. Checks the stop flag between chunks. */
static void sleepUntil(uint64_t deadline) {
    for (;;) {
        if (g_stop) return;
        uint64_t now = mach_absolute_time();
        if (now >= deadline) return;
        double rem_ns = machToNs(deadline - now);
        struct timespec ts;
        ts.tv_sec = (time_t)(rem_ns / 1e9);
        ts.tv_nsec = (long)(rem_ns - (double)ts.tv_sec * 1e9);
        if (ts.tv_sec < 0) ts.tv_sec = 0;
        if (ts.tv_nsec < 0) ts.tv_nsec = 0;
        if (ts.tv_sec == 0 && ts.tv_nsec < 1000) ts.tv_nsec = 1000;
        nanosleep(&ts, NULL);
    }
}

static bool readCursor(CGPoint *out) {
    CGEventRef e = CGEventCreate(NULL);
    if (e == NULL) return false;
    *out = CGEventGetLocation(e);
    CFRelease(e);
    return isfinite(out->x) && isfinite(out->y);
}

static bool anyButtonDown(void) {
    for (int b = 0; b <= 4; ++b) {
        if (CGEventSourceButtonState(kCGEventSourceStateCombinedSessionState, (CGMouseButton)b)) return true;
    }
    return false;
}

static bool captureTopology(Topology *t) {
    memset(t, 0, sizeof(*t));
    uint32_t n = 0;
    if (CGGetActiveDisplayList(MAX_DISPLAYS, t->ids, &n) != kCGErrorSuccess) return false;
    t->count = n;
    for (uint32_t i = 0; i < n; ++i) t->bounds[i] = CGDisplayBounds(t->ids[i]);
    return true;
}

static bool sameTopology(const Topology *a, const Topology *b) {
    if (a->count != b->count) return false;
    for (uint32_t i = 0; i < a->count; ++i) {
        if (a->ids[i] != b->ids[i]) return false;
        if (!CGRectEqualToRect(a->bounds[i], b->bounds[i])) return false;
    }
    return true;
}

static bool pointOnAnyOnlineDisplay(CGPoint p) {
    CGDirectDisplayID ids[MAX_DISPLAYS];
    uint32_t n = 0;
    if (CGGetOnlineDisplayList(MAX_DISPLAYS, ids, &n) != kCGErrorSuccess) return false;
    for (uint32_t i = 0; i < n; ++i) {
        if (CGRectContainsPoint(CGDisplayBounds(ids[i]), p)) return true;
    }
    return false;
}

/* Post one relative mouse move. Location stays zero; only dx/dy carry input. */
static kern_return_t postDelta(io_connect_t handle, SInt32 dx, SInt32 dy) {
    NXEventData data;
    memset(&data, 0, sizeof(data));
    data.mouseMove.dx = dx;
    data.mouseMove.dy = dy;
    IOGPoint loc;
    loc.x = 0;
    loc.y = 0;
    return IOHIDPostEvent(handle, NX_MOUSEMOVED, loc, &data, kNXEventDataVersion, 0, kIOHIDPostHIDManagerEvent);
}

static double estimateDuration(int repeats, int events, double rate) {
    double perTrial = (double)RESET_MS / 1000.0 + (double)events / rate + (double)SETTLE_MS / 1000.0;
    return perTrial * 2.0 * (double)repeats;
}

static bool parseInt(const char *s, int *out) {
    char *end = NULL;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' || v < INT_MIN || v > INT_MAX) return false;
    *out = (int)v;
    return true;
}

static bool parseDouble(const char *s, double *out) {
    char *end = NULL;
    errno = 0;
    double v = strtod(s, &end);
    if (end == s || *end != '\0' || !isfinite(v)) return false;
    *out = v;
    return true;
}

static bool argErr(const char *msg) {
    fprintf(stderr, "measure: %s\n", msg);
    fprintf(stderr, "Try 'measure --help'.\n");
    return false;
}

static bool parseArgs(int argc, char **argv, Options *o) {
    for (int i = 1; i < argc; ++i) {
        const char *a = argv[i];
        if (strcmp(a, "--run") == 0) {
            o->run = true;
        } else if (strcmp(a, "--axis") == 0) {
            if (i + 1 >= argc) return argErr("--axis needs a value");
            const char *v = argv[++i];
            if (strcmp(v, "x") == 0) o->axis = 0;
            else if (strcmp(v, "y") == 0) o->axis = 1;
            else return argErr("--axis must be x or y");
        } else if (strcmp(a, "--step") == 0) {
            if (i + 1 >= argc) return argErr("--step needs a value");
            if (!parseInt(argv[++i], &o->step)) return argErr("--step must be an integer");
        } else if (strcmp(a, "--events") == 0) {
            if (i + 1 >= argc) return argErr("--events needs a value");
            if (!parseInt(argv[++i], &o->events)) return argErr("--events must be an integer");
        } else if (strcmp(a, "--rate") == 0) {
            if (i + 1 >= argc) return argErr("--rate needs a value");
            if (!parseInt(argv[++i], &o->rate)) return argErr("--rate must be an integer");
        } else if (strcmp(a, "--repeats") == 0) {
            if (i + 1 >= argc) return argErr("--repeats needs a value");
            if (!parseInt(argv[++i], &o->repeats)) return argErr("--repeats must be an integer");
        } else if (strcmp(a, "--control") == 0) {
            if (i + 2 >= argc) return argErr("--control needs X Y");
            if (!parseDouble(argv[i + 1], &o->control_x) || !parseDouble(argv[i + 2], &o->control_y))
                return argErr("--control coordinates must be finite numbers");
            i += 2;
            o->have_control = true;
        } else if (strcmp(a, "--crossing") == 0) {
            if (i + 2 >= argc) return argErr("--crossing needs X Y");
            if (!parseDouble(argv[i + 1], &o->crossing_x) || !parseDouble(argv[i + 2], &o->crossing_y))
                return argErr("--crossing coordinates must be finite numbers");
            i += 2;
            o->have_crossing = true;
        } else {
            fprintf(stderr, "measure: unknown argument '%s'\n", a);
            fprintf(stderr, "Try 'measure --help'.\n");
            return false;
        }
    }
    return true;
}

static const char *validate(const Options *o) {
    if (!o->run) return "missing --run";
    if (!o->have_control) return "missing --control X Y";
    if (!o->have_crossing) return "missing --crossing X Y";
    if (o->step == 0 || o->step < -100 || o->step > 100) return "--step must be 1..100 in absolute value";
    if (o->events < 1 || o->events > MAX_EVENTS) return "--events must be 1..1000";
    if (o->rate < 1 || o->rate > 1000) return "--rate must be 1..1000";
    if (o->repeats < 1 || o->repeats > MAX_REPEATS) return "--repeats must be 1..10";
    if (!pointOnAnyOnlineDisplay(CGPointMake(o->control_x, o->control_y)))
        return "--control is outside the online display bounds";
    if (!pointOnAnyOnlineDisplay(CGPointMake(o->crossing_x, o->crossing_y)))
        return "--crossing is outside the online display bounds";
    if (estimateDuration(o->repeats, o->events, (double)o->rate) > MAX_DURATION_S)
        return "estimated duration exceeds 60 seconds";
    return NULL;
}

static RunStatus runTrials(io_connect_t handle, const Options *o, const Topology *baseline,
                           Trial *trials, int *trialCount, const char **failMsg) {
    int count = 0;
    RunStatus status = RUN_OK;
    bool stop = false;

    int dx = 0, dy = 0;
    if (o->axis == 0) dx = o->step;
    else dy = o->step;

    uint64_t intervalTicks = nsToMach(1e9 / (double)o->rate);
    if (intervalTicks < 1) intervalTicks = 1;
    uint64_t hardDeadline = mach_absolute_time()
        + nsToMach((estimateDuration(o->repeats, o->events, (double)o->rate) + 15.0) * 1e9);

    for (int r = 0; r < o->repeats && !stop; ++r) {
        for (int c = 0; c < 2 && !stop; ++c) {
            if (g_stop) { status = RUN_INTERRUPTED; stop = true; break; }

            Topology before;
            if (!captureTopology(&before) || !sameTopology(&before, baseline)) {
                *failMsg = "display topology changed";
                status = RUN_ABORTED;
                stop = true;
                break;
            }

            Trial tr;
            memset(&tr, 0, sizeof(tr));
            tr.repeat = r;
            tr.condition = (c == 0) ? "control" : "crossing";
            tr.events = o->events;

            double sx = (c == 0) ? o->control_x : o->crossing_x;
            double sy = (c == 0) ? o->control_y : o->crossing_y;

            if (CGWarpMouseCursorPosition(CGPointMake(sx, sy)) != kCGErrorSuccess) {
                *failMsg = "CGWarpMouseCursorPosition failed";
                status = RUN_API_ERROR;
                stop = true;
                break;
            }
            CGAssociateMouseAndMouseCursorPosition(true);

            /* Seed a zero-delta mouseMoved so the stream starts clean. */
            if (postDelta(handle, 0, 0) != KERN_SUCCESS) {
                *failMsg = "IOHIDPostEvent seed failed";
                status = RUN_API_ERROR;
                stop = true;
                break;
            }

            /* A warp can suppress local input for a moment. Wait a fixed reset. */
            sleepUntil(mach_absolute_time() + nsToMach((double)RESET_MS * 1e6));
            if (g_stop) { status = RUN_INTERRUPTED; stop = true; break; }

            CGPoint sp;
            if (!readCursor(&sp)) {
                *failMsg = "cannot read cursor position";
                status = RUN_API_ERROR;
                stop = true;
                break;
            }
            tr.start_x = sp.x;
            tr.start_y = sp.y;

            uint64_t base = mach_absolute_time();
            for (int i = 0; i < o->events; ++i) {
                if (g_stop) { status = RUN_INTERRUPTED; break; }
                if (mach_absolute_time() > hardDeadline) {
                    *failMsg = "run exceeded its time bound";
                    status = RUN_ABORTED;
                    break;
                }
                uint64_t deadline = base + intervalTicks * (uint64_t)i;
                sleepUntil(deadline);
                if (g_stop) { status = RUN_INTERRUPTED; break; }
                if (postDelta(handle, dx, dy) != KERN_SUCCESS) {
                    *failMsg = "IOHIDPostEvent failed";
                    status = RUN_API_ERROR;
                    break;
                }
                tr.events_sent++;
                tr.input_dx += dx;
                tr.input_dy += dy;
                uint64_t now = mach_absolute_time();
                if (now > deadline) {
                    double lagMs = machToNs(now - deadline) / 1e6;
                    if (lagMs > tr.max_lag_ms) tr.max_lag_ms = lagMs;
                }
            }

            if (status == RUN_API_ERROR) {
                trials[count++] = tr;
                stop = true;
                break;
            }

            Topology after;
            tr.topology_changed = !captureTopology(&after) || !sameTopology(&after, baseline);

            sleepUntil(mach_absolute_time() + nsToMach((double)SETTLE_MS * 1e6));

            CGPoint ep;
            if (!readCursor(&ep)) {
                *failMsg = "cannot read cursor position";
                status = RUN_API_ERROR;
                trials[count++] = tr;
                stop = true;
                break;
            }
            tr.end_x = ep.x;
            tr.end_y = ep.y;
            tr.travel_axis = (o->axis == 0) ? (ep.x - sp.x) : (ep.y - sp.y);
            long countsAxis = (o->axis == 0) ? tr.input_dx : tr.input_dy;
            tr.delta_error = tr.travel_axis - (double)countsAxis;
            tr.clipped = !pointOnAnyOnlineDisplay(ep) || !pointOnAnyOnlineDisplay(sp);
            tr.interference = anyButtonDown();

            trials[count++] = tr;

            if (tr.topology_changed) {
                *failMsg = "display topology changed during a trial";
                status = RUN_ABORTED;
                stop = true;
                break;
            }
            if (status != RUN_OK) { stop = true; break; }
        }
    }

    *trialCount = count;
    return status;
}

static void printJSON(const Options *o, const Trial *trials, int n,
                      const char *status, const char *error, bool restored) {
    printf("{\n");
    printf("  \"tool\": \"measure\",\n");
    printf("  \"schema\": 1,\n");
    printf("  \"status\": \"%s\",\n", status);
    if (error != NULL) printf("  \"error\": \"%s\",\n", error);
    printf("  \"restored\": %s,\n", restored ? "true" : "false");
    printf("  \"parameters\": {\n");
    printf("    \"axis\": \"%s\",\n", (o->axis == 0) ? "x" : "y");
    printf("    \"step\": %d,\n", o->step);
    printf("    \"events\": %d,\n", o->events);
    printf("    \"rate_hz\": %d,\n", o->rate);
    printf("    \"repeats\": %d,\n", o->repeats);
    printf("    \"control\": {\"x\": %.3f, \"y\": %.3f},\n", o->control_x, o->control_y);
    printf("    \"crossing\": {\"x\": %.3f, \"y\": %.3f},\n", o->crossing_x, o->crossing_y);
    printf("    \"reset_ms\": %d,\n", RESET_MS);
    printf("    \"settle_ms\": %d,\n", SETTLE_MS);
    printf("    \"duration_estimate_s\": %.3f\n",
           estimateDuration(o->repeats, o->events, (double)o->rate));
    printf("  },\n");
    printf("  \"caveats\": [\n");
    printf("    \"input_counts are raw HID counts; travel_axis is in logical points; the units differ\",\n");
    printf("    \"injected events use their own HID source; not equivalent to a physical mouse or acceleration curve\"\n");
    printf("  ],\n");
    printf("  \"trials\": [\n");
    for (int i = 0; i < n; ++i) {
        const Trial *t = &trials[i];
        printf("    {\n");
        printf("      \"repeat\": %d,\n", t->repeat);
        printf("      \"condition\": \"%s\",\n", t->condition);
        printf("      \"input_counts\": {\"x\": %ld, \"y\": %ld},\n", t->input_dx, t->input_dy);
        printf("      \"events\": %d,\n", t->events);
        printf("      \"events_sent\": %d,\n", t->events_sent);
        printf("      \"start\": {\"x\": %.3f, \"y\": %.3f},\n", t->start_x, t->start_y);
        printf("      \"end\": {\"x\": %.3f, \"y\": %.3f},\n", t->end_x, t->end_y);
        printf("      \"travel_axis\": %.3f,\n", t->travel_axis);
        printf("      \"delta_error\": %.3f,\n", t->delta_error);
        printf("      \"max_lag_ms\": %.3f,\n", t->max_lag_ms);
        printf("      \"clipped\": %s,\n", t->clipped ? "true" : "false");
        printf("      \"interference\": %s,\n", t->interference ? "true" : "false");
        printf("      \"topology_changed\": %s\n", t->topology_changed ? "true" : "false");
        printf("    }%s\n", (i + 1 < n) ? "," : "");
    }
    printf("  ]\n");
    printf("}\n");
}

int main(int argc, char **argv) {
    Options opt;
    memset(&opt, 0, sizeof(opt));
    opt.axis = 0;
    opt.step = 5;
    opt.events = 80;
    opt.rate = 125;
    opt.repeats = 3;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(stdout);
            return 0;
        }
    }

    if (!parseArgs(argc, argv, &opt)) return 2;

    const char *verr = validate(&opt);
    if (verr != NULL) {
        fprintf(stderr, "measure: %s\n", verr);
        fprintf(stderr, "Try 'measure --help'.\n");
        return 2;
    }

    initTimebase();

    if (IOHIDCheckAccess(kIOHIDRequestTypePostEvent) != kIOHIDAccessTypeGranted) {
        printJSON(&opt, NULL, 0, "error", "post-event access not granted", false);
        return 3;
    }
    if (!CGPreflightPostEventAccess()) {
        printJSON(&opt, NULL, 0, "error", "CGPreflightPostEventAccess denied", false);
        return 3;
    }
    if (anyButtonDown()) {
        printJSON(&opt, NULL, 0, "error", "a mouse button is already down", false);
        return 3;
    }

    Topology baseline;
    if (!captureTopology(&baseline)) {
        printJSON(&opt, NULL, 0, "error", "cannot read display topology", false);
        return 4;
    }

    CGPoint orig;
    if (!readCursor(&orig)) {
        printJSON(&opt, NULL, 0, "error", "cannot read cursor position", false);
        return 4;
    }

    signal(SIGINT, onSignal);
    signal(SIGTERM, onSignal);

    fprintf(stderr, "measure: keep your hands off the mouse. Starting in 1 second.\n");
    struct timespec pre = {1, 0};
    nanosleep(&pre, NULL);

    NXEventHandle handle = NXOpenEventStatus();
    if (handle == MACH_PORT_NULL) {
        printJSON(&opt, NULL, 0, "error", "NXOpenEventStatus failed", false);
        return 4;
    }

    /* Zero the post-warp suppression interval so our own input is not dropped. */
    if (CGSetLocalEventsSuppressionInterval(0.0) != kCGErrorSuccess) {
        NXCloseEventStatus(handle);
        printJSON(&opt, NULL, 0, "error", "cannot disable cursor-warp suppression", false);
        return 4;
    }

    Trial trials[MAX_TRIALS];
    int trialCount = 0;
    const char *failMsg = NULL;

    RunStatus st = runTrials(handle, &opt, &baseline, trials, &trialCount, &failMsg);

    bool restored = false;
    if (CGWarpMouseCursorPosition(orig) == kCGErrorSuccess) {
        CGAssociateMouseAndMouseCursorPosition(true);
        restored = true;
    }
    NXCloseEventStatus(handle);

    const char *status = "ok";
    int code = 0;
    if (st == RUN_INTERRUPTED) { status = "interrupted"; code = 130; }
    else if (st == RUN_ABORTED) { status = "aborted"; code = 5; }
    else if (st == RUN_API_ERROR) { status = "error"; code = 4; }

    printJSON(&opt, trials, trialCount, status, failMsg, restored);
    return code;
}
