# UU Remote + VITURE Pro 2 Integration Plan

## Goal

Use a vivo X100 as a thin XR client:

Windows workstation -> UU Remote -> vivo X100 -> UxSpace spatial compositor -> VITURE Pro 2.

The workstation provides compute. The phone provides remote-stream decoding, input routing,
VITURE head tracking, and spatial composition.

## Repositories

- uxspace: primary Android/XR integration target
- viture-v2: VITURE Pro 2 IMU / pose-prediction reference
- DisplayMirror: MediaProjection / VirtualDisplay fallback reference
- uurc-web: protocol/WebRTC research for a later native UU XR client

## V1 preferred path: run UU Remote directly on a trusted VirtualDisplay

UxSpace already provides the required primitives:

1. A trusted VirtualDisplay backed by SurfaceTexture / OpenGL texture.
2. Shell-uid launch onto a chosen display using am start --display.
3. Per-display touch, scroll, key, and text injection.
4. VITURE head tracking driving the spatial camera.
5. A Presentation on the physical VITURE external display.

Pipeline:

UU Remote Activity
  -> trusted VirtualDisplay
  -> Surface
  -> SurfaceTexture
  -> external OES GL texture
  -> UxSpace WorkspaceRenderer
  -> head-tracked virtual quad
  -> VITURE Pro 2

This path avoids an additional MediaProjection copy/capture stage.

### First experiment

1. Build current UxSpace on vivo X100.
2. Complete the one-time Wireless Debugging privilege bootstrap.
3. Connect VITURE Pro 2.
4. Confirm FREE / world-locked mode works.
5. Launch the installed UU Remote launcher activity into one UxSpace app window.
6. Connect to the Windows workstation.
7. Verify:
   - remote video is visible;
   - hardware decoding continues normally;
   - mouse/touch input reaches UU Remote;
   - keyboard input reaches the workstation;
   - audio behavior is acceptable;
   - virtual screen remains world-locked while turning the head.

### Primary risk

UU Remote may mark its rendering surface/window secure, or use a protected hardware-decoder
path that cannot be sampled through the current virtual-display flags. Symptom: the app UI may
appear but the remote video area is black, or the complete virtual display may be redacted.

Current UxSpace trusted-display flags are PUBLIC + OWN_CONTENT_ONLY + PRESENTATION + TRUSTED.
Do not add SECURE pre-emptively. Test first. If UU uses protected content, evaluate whether a
secure trusted display can legally and technically feed the current SurfaceTexture pipeline.

## V1 fallback: Android single-app MediaProjection

If direct trusted-display rendering fails:

UU Remote on the phone display
  -> Android app-only MediaProjection
  -> VirtualDisplay
  -> SurfaceTexture
  -> GL texture
  -> UxSpace spatial renderer
  -> VITURE Pro 2

Keep capture Surface-to-Surface. Do not read frames into Bitmap/CPU memory.

The AR render loop and remote-desktop frame loop must remain independent. Head pose should update
at the glasses rate even when the remote video is 30/60 fps or temporarily stalls.

## V1 UX target

- one UU Remote virtual monitor;
- FREE 3DoF world lock;
- recenter;
- screen distance;
- screen size;
- optional curvature later;
- phone trackpad;
- Bluetooth keyboard/mouse;
- fullscreen mode for UU;
- configurable virtual-display DPI for readable text.

## V2: native UU XR client research

Use the forked uurc-web only as a research/reference path.

Target pipeline:

UU host
  -> UU signaling / WebRTC
  -> Android native client
  -> MediaCodec hardware decode directly to SurfaceTexture
  -> UxSpace GL renderer
  -> VITURE Pro 2

This can remove the official UU Android UI and the MediaProjection/secondary-display integration
layer, but it has much higher protocol-maintenance and compatibility risk.

## Development milestones

### M0 Hardware baseline
- UxSpace builds and installs on vivo X100.
- VITURE Pro 2 appears as external display.
- VITURE head pose works.
- FREE mode is stable.

### M1 Direct UU window
- Discover UU launcher package/activity via PackageManager.
- Launch UU to a trusted per-app VirtualDisplay.
- Confirm rendered video and input.

### M2 Office usability
- Make UU launch a first-class shortcut/profile.
- Add preferred 16:9 monitor size/distance presets.
- Tune DPI and text readability.
- Validate BT keyboard/mouse and phone trackpad.

### M3 Latency
- Measure remote stream latency separately from head-motion latency.
- Keep GL/head tracking at display cadence.
- Evaluate pose prediction using viture-v2 ideas if needed.

### M4 Fallback capture
- Implement MediaProjectionSource only if direct launch is incompatible.
- Support Android single-app capture.
- Keep GPU-only SurfaceTexture path.

### M5 Native research
- Inspect uurc-web signaling/WebRTC architecture.
- Prototype direct video SurfaceTexture decode only after V1 is usable.

## vivo X100 specific checks

- USB-C DP Alt Mode + USB data simultaneous operation with Pro 2.
- OriginOS Wireless Debugging behavior and persistence.
- Background/foreground restrictions on the UxSpace service.
- 90/120 Hz external display mode exposed to Android.
- USB reconnect behavior.
- Thermal/battery behavior during long H.265/AV1 decode + GL composition sessions.
