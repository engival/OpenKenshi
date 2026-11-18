# Stereo 3D and VR (future, not scheduled)

Parked until after beta. Written 2026-11-02 so the idea and the scoping survive; nothing here is
committed work except the camera prep under "What to do now".

## Goal

**Stereoscopic play, not a VR game.** Like the old nvidia 3D Vision: the same RTS camera, mouse and
UI, with depth. No head-tracked camera, no motion controls, no room scale.

Later, as an experiment: eye tracking ("where you look") for focus effects such as depth of field or
selecting by gaze. That needs a headset with eye tracking (Valve Index has none; Steam Frame does).

## Hardware in hand / expected

- **Valve Index** (SteamVR, on this Linux machine). The test target for everything below.
- **Steam Frame** (hoped for, early 2027): as announced, a standalone ARM headset (Snapdragon) running
  SteamOS, with eye tracking, and streaming from a PC. Two ways to use it: stream from the desktop
  client (same path as the Index), or a native ARM build running on the headset. The native build
  is a separate project (ARM build, Vulkan or GLES drivers there, a much smaller GPU budget).

## Rendering: two eyes (needed for every output below)

1. **One camera value, passed in.** Every scene renderer draws from a camera it is given, never from
   the frame's globals. Most modules already take `(mvp, cam)`; the rest is `main.cpp` (`build_mvp`
   and `camera_basis` read the window size, `kFovY` and the camera state) and the two separate
   `Camera` structs in `fogvolume.hpp` and `particles.hpp`.
2. **Draw scene + fog + particles + post twice**, once per eye, into two targets. Run the shadow pass
   and the CPU culling once and share them (cull with a frustum wide enough for both eyes). GPU cost
   about doubles; CPU barely changes.
3. **Off-axis projection** (asymmetric frustums, parallel eye axes), not toe-in. The convergence
   (screen) plane sits at the RTS camera's focus point: what you look at is at screen depth, the
   world opens in front of and behind it. Eye separation = a config key.
4. **UI depth.** The flat HUD stays at screen depth. Anything placed on the world (nameplates and
   markers from `head_screen()`, damage floaters, order markers) gets per-eye disparity at its world
   depth, or it reads as behind the thing it labels. The cursor becomes a 3D cursor drawn at the
   depth under it. Picking stays on the centre (mono) camera.
5. **Screen-space effects** (post, occluder fade dither, anything reading the depth buffer) run per
   eye with that eye's matrices.

## Outputs, cheapest first

- **Side-by-side / top-bottom / anaglyph** in the normal window. No SDK. Covers 3D TVs and the
  display glasses (XREAL, Viture...) that take an SBS signal, which is today's closest thing to
  3D Vision. Testable headless with `--screenshot`. A config key, no option UI.
- **OpenXR "virtual 3D screen".** One quad composition layer per eye (`XrCompositionLayerQuad` with
  `eyeVisibility` left / right), world-locked a few metres in front of the user. The compositor
  keeps the quad stable as the head moves, so the game needs no VR frame rate and causes no sim
  sickness; the game camera ignores head tracking. Later, an optional curved/larger screen.
  - Library: the OpenXR loader (Apache-2.0). Runtimes: SteamVR (the player's install) or Monado
    (FOSS; also has a simulated device for headless tests).
  - **GL backend:** easiest. OpenXR hands out GL textures; sokol takes external GL textures as
    `sg_image_desc.gl_textures` (injected), so each eye renders straight into the XR swapchain image.
    The XR session needs the GLX display/context, which can be read from the current context.
    **Check first:** how well SteamVR's OpenXR supports OpenGL sessions on Linux.
  - **Vulkan backend:** OpenXR wants to create (or approve) the Vulkan instance and device.
    `sg_setup` accepts an external device (`sg_vulkan_environment`), but `sokol_app` creates its own
    for the window. At the vendored sokol (`third_party/sokol/VERSION`) there is no injection of
    external Vulkan images and no native image query, so the eye images can't be handed over without
    a sokol change or a newer upstream. Re-check when updating sokol.
- **Full VR camera** (head-tracked eyes with the HMD's own projections): out of scope; the RTS camera
  is the point.

## Eye tracking (Steam Frame, experiment)

OpenXR exposes gaze through `XR_EXT_eye_gaze_interaction` (a pose action). Whether SteamVR exposes the
Frame's eye tracking to apps this way is unknown; check when the hardware arrives. Ideas to try: depth
of field focused at the gaze depth, gaze-picked hover (nameplates, tooltips), convergence plane at the
gaze depth instead of the camera focus. Foveated rendering is a different (perf) use of the same data.

## What to do now

Only the camera prep: one camera struct, resolved from the globals once per frame, passed to new code
and to code being touched anyway. No stereo code, no SDKs, no options.
