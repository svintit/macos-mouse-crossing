# Portable mouse crossing for macOS

Keep the pointer aligned when crossing between monitors with different sizes, resolutions, scaling, or orientations.

macOS uses logical screen coordinates to decide where the pointer enters the next display. Those coordinates do not always match the physical screen edges. This kit maps the crossing position using calibrated physical display rectangles.

## What it does

- Matches the crossing height for left/right neighbors and the crossing position for above/below neighbors.
- Supports portrait, landscape, and laptop displays through per-device calibration.
- Uses the nearest target corner when physical screen areas do not overlap along an existing shared side.
- Handles crossings on a dedicated native input thread, without window-focus or accessibility work in the mouse callback.
- Disables the post-warp input-suppression interval that caused movement loss during fast crossings.
- Keeps native movement deltas unchanged. It does not change the system mouse speed or acceleration setting.

The kit does not rearrange windows or displays. Dragging keeps normal macOS behavior. Physical alignment still needs calibration, and small travel differences can remain.

## Set it up with an AI

Give this folder to an AI with local file and command access. Use this prompt:

> Read AI_SETUP.md. Adapt this kit to my connected displays and physical layout. Preserve my existing Hammerspoon configuration. Calibrate both ends of each shared edge. Verify equal relative input against actual travel within a screen and across screens. Ask before running pointer-moving measurements. Do not claim smoothness from coordinate-only tests.

## Requirements

- macOS, Hammerspoon, and Xcode Command Line Tools.
- Accessibility permission for Hammerspoon. Grant additional input permissions only if macOS requires them.
- Extended displays with touching edges in System Settings. Mirroring and diagonal-only neighbors are not supported.

Build on the destination Mac with `bash build.sh`. The archive contains source, not a prebuilt library. See AI_SETUP.md before installation.

## Limits

The kit supports up to 16 configured displays. Dragging keeps normal macOS behavior. An unknown display stops mapping until you configure it. Display names must be unique, or use display UUIDs.

The suppression fix uses a deprecated macOS API. It works on the source device, but each destination needs measurement. The setting affects Hammerspoon's process, not the entire system. Stopping mapping does not restore that process setting. Restart Hammerspoon to reset it.

The example configuration contains no personal calibration or display UUIDs. Your AI must not replace an existing init.lua.

Released under the [MIT License](LICENSE). AI assisted in creating this kit.
