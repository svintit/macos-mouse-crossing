# AI setup instructions

## Preserve the destination

Back up existing Hammerspoon files. Inspect existing mouse handlers before adding this one. Never run two crossing handlers together. Do not change mouse acceleration or display resolutions to mask a crossing defect.

Do not install this kit over a working setup without the user's request. Build in the kit directory. Do not overwrite a loaded native library in place. The build script replaces the file atomically; reload Hammerspoon after rebuilding.

## Discover displays

Load the Lua file without starting mapping in the Hammerspoon Console:

```lua
kit = dofile("/absolute/path/mouse-crossing-kit/mouse_crossing.lua")
hs.inspect(kit.inventory())
```

This returns names, UUIDs, current IDs, and logical frames. Use a UUID when names repeat. UUIDs and names are local configuration, not shareable defaults.

For reported physical sizes, run:

```sh
swift -e 'import CoreGraphics; var ids = [CGDirectDisplayID](repeating: 0, count: 16); var n: UInt32 = 0; CGGetActiveDisplayList(16, &ids, &n); for id in ids.prefix(Int(n)) { print(id, CGDisplayScreenSize(id), CGDisplayRotation(id)) }'
```

Check orientation against the physical screen. Reported sizes are starting estimates, not calibration truth. Ask about the visible panel edges, not bezels. Ask about each display's physical position, rotation, and offsets.

## Configure physical rectangles

Copy layout.example.lua to layout.lua. Replace every placeholder name with a discovered name or UUID. Set x, y, w, and h in millimetres. X increases rightward. Y increases downward. The origin is arbitrary, but all rectangles use the same origin.

Use visible panel dimensions. Effective calibrated dimensions can differ from reported dimensions. Keep the macOS logical rectangles unchanged. The native handler reads those rectangles automatically.

The example has a portrait display on the left, a landscape display on the right, and a centered laptop below it. It is not the source user's calibration. Rotate or rearrange the physical rectangles for the destination.

Each configured pair must share an edge in macOS with some logical overlap. The native handler accepts an edge mismatch of at most two points. It chooses the closest physical neighbor among those logical neighbors. Outside the physical overlap, it clamps to the nearest target corner. This permits crossing along an existing shared side, but cannot create a neighbor where macOS has no touching edge.

## Build and integrate

Run `bash build.sh`. Set HAMMERSPOON_APP if the app is installed elsewhere. The script uses the installed Hammerspoon Lua headers and the current architecture. Deprecated API warnings are expected. Other warnings are errors.

Add a small block to the existing init.lua, using the real kit path:

```lua
_G.portableMouseCrossing = dofile("/absolute/path/mouse-crossing-kit/mouse_crossing.lua")
local ok, err = portableMouseCrossing.start({directory = "/absolute/path/mouse-crossing-kit"})
if not ok then hs.printf("Mouse crossing stopped: %s", err) end
```

Keep the global reference. Check `portableMouseCrossing.status()`. Unknown displays stop mapping safely. Disconnected configured displays are ignored. A single connected display does not need mapping.

Use `portableMouseCrossing.setEnabled(false)` to disable mapping. Use `portableMouseCrossing.stop()` to remove its watcher and native handler. Remove the integration block to uninstall. Keep unrelated configuration intact.

## Calibrate two endpoints

First check the middle and both ends of each shared edge at low speed. Then check both directions. Do not assume that a reported centered physical arrangement produces the best effective calibration.

A constant error suggests an offset. An error that grows toward the ends suggests a width or height ratio. Use horizontal dimensions for above/below crossings. Use vertical dimensions for left/right crossings.

For an effective interval with start A and length L, the endpoints are A and A+L. To move its start by d0 and its end by d1, use:

- New start: A + d0.
- New length: L + d1 - d0.

To move only the right edge 10 mm right, increase width by 10 mm and leave x unchanged. To restore the left edge 10 mm right while keeping the right fixed, increase x by 10 mm and decrease width by 10 mm. Do not apply a symmetric correction unless both edges need it.

After an edit, stop the old instance and start it again to reload layout.lua. Keep the successful endpoint fixed while adjusting the other one.

## Verify input distance, not only coordinates

Ask the user to keep hands off the mouse. Read `build/measure --help`. Choose two safe start points using inventory frames: one control path inside a screen, one crossing path. Avoid screen edges beyond the intended crossing. Use the same direction, step, event count, and rate for each pair.

Example syntax only; replace coordinates for the destination:

```sh
build/measure --control 500 300 --crossing 20 300 --axis x --step -5 --events 80 --rate 125 --repeats 3 --run
```

Repeat at 500 Hz and in the opposite direction. For above/below neighbors, use axis y. The tool posts fixed relative HID deltas. It does not calculate each move from the current cursor position. It returns the pointer after completion.

Compare signed input_counts and travel_axis. Raw HID counts and logical points are different units. Compare crossing travel with the matched control travel, not an assumed universal 1:1 conversion. This synthetic HID source does not prove equivalence to every physical mouse or acceleration curve.

The source-device investigation found 400 counts produced 400 points within a screen. Before the suppression fix, crossing travel was 246 points at 125 Hz and 81 points at 500 Hz. After the fix, crossing travel was 396–401 points. These are historical results, not destination acceptance results.

Keep timing, direction, rate, and repeat count in the report. Flag clipping, user interference, and topology changes. Confirm physical alignment with the user after numerical checks. Never call a coordinate-only test a velocity test.

## Keep the critical behavior

The native HID event tap runs on its own thread. It does not run Lua, accessibility queries, window focus, or shell commands in the mouse callback.

Keep explicit cursor positioning. Changing only event coordinates failed physical alignment on the source device. Keep native movement deltas unchanged. Do not rescale the already-global crossing coordinate along the direction of travel.

Keep `CGSetLocalEventsSuppressionInterval(0)`. The measured loss came from the roughly 250 ms post-warp suppression interval. Setting an unrelated synthetic CGEventSource interval did not solve cursor-warp suppression. This legacy API needs a compatibility check on each macOS version. Do not hide a startup failure or silently return to the stuttering implementation.

If existing focus automation uses mouse event taps, inspect its cost. Prefer nonblocking or timer-based focus tracking. Do not remove the user's focus behavior without discussing that change.
