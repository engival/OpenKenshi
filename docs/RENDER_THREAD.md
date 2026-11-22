# Render thread, shadow caching, Vulkan and OpenXR: design and options

Written 2026-11-14 (item kenshi-k16.2.205.12, user 2026-11-14: "Yes, a render thread makes sense"), on base `client`
**eaa758df**. Design only: no code. It answers the question in kenshi-k16.2.205.9 — draw submission off the main thread
for 144 fps — and ends (section 8) with the cost of starting now against starting after the playable loop, for the user
to decide.

Numbers are from the headroom report `desktop-findings/kenshi-k16.2.205.7/REPORT.md` (cited as REPORT §N) unless marked
otherwise; kenshi-k16.2.205.8's close reason is the one post-report measurement. **Estimates are arithmetic over those
stage costs and are marked "est."**; run-to-run noise on this shared desktop is ±1.5-2 ms (REPORT §1, §6). The report
predates the instanced character draw (205.8, merged 51bfa808) and the pose/tunable work in flight (kenshi-k16.2.205.11),
both of which lower the numbers below; they are named where they matter and not folded into arithmetic I did not measure.

## 1. Today's frame: compute and submit

`main.cpp:frame()` is the sokol_app frame callback (`main.cpp:11069`). One thread does everything: the world update, then
every GL/sokol call from `GPU_BEGIN_FRAME` (`main.cpp:6455`) to `sg_commit()` (`main.cpp:6830`). The dev stage timer
(`cpustage.hpp`) marks the split; REPORT §2 measured it.

| stage (mark at `main.cpp:`) | class | Heft AMD 1080p | Heng AMD 1080p | what it does |
|---|---|---|---|---|
| futures+script (6052) | compute + load | ~0 steady | ~0 steady | asset futures, script |
| physics+poll (6063) | compute | — | — | Jolt, local server |
| camera (6072) | compute | — | — | camera update |
| sim.update (6077) | compute (decode) | 0.90 | 0.96 | wire snapshot decode/apply; 0.45-1.2 s stalls (REPORT §4) |
| sim.nav+squads (6082) | compute | — | — | nav tiles, squads |
| sim.peek+hover (6103) | compute | 0.61 | 0.85 | cursor pick/peek, 92-353 ms on a camera jump |
| update_ui (6106) | compute | — | — | UI layout/hover/state |
| chars.update (6110) | compute | 2.51 | 3.02 | animation pose per character |
| sounds+trails (6147) | compute | — | — | audio posts, debug trails |
| sky+weather+audio+shadow.update (6227) | compute | — | — | shadow cascade maths, weather blend, audio |
| props (6272) | compute | 0.75 | 0.60 | prop list rebuild/diff |
| buildings.update (6339) | compute **+ upload** | 1.26 | 0.97 | frame instance list; `sg_write_buffer_transient` (`worldbuildings.cpp:1809`) |
| foliage+grass.update (6352) | compute **+ upload** | ~0 | ~0 (98 ms spikes) | foliage/grass cell buffers, `sg_make_buffer` (`foliage.cpp:283`, `grass.cpp:542`) |
| particles+blood (6454) | compute **+ upload** | — | — | particle sim; instance buffer write (`particles.cpp:1054`) |
| shadow.pass (6498) | **submit** | 1.64 | 3.63 | 4 cascades of world casters + characters (`main.cpp:6468`) |
| portraits+reflection (6527) | **submit** | 0.00 | 2.55 | portraits; planar reflection redraws sky+world+chars (`main.cpp:6505`) |
| post.begin+mask (6562) | **submit** | — | — | HDR begin, interior-mask pass (`main.cpp:6552`) |
| draw.buildings / foliage / grass (6580/6590/6594) | **submit** | 0.08 / 0.04 / 0.00 | 0.49 / 0.53 / — | scene pass |
| draw.chars (6598) | **submit** | 1.65 | 1.46 | body+armour+hair+items draws |
| draw.items (6602) | **submit** | 0.18 | 0.21 | attachment items |
| draw.water+glass+misc (6634) | **submit** | 0.00 | 0.44 | water, glass, fade band |
| idpass (6647) | **submit + readback** | 0.09 | 0.12 | cursor id pass, PBO readback (`main.cpp:6641`, `idbuffer.cpp:60`) |
| post.endScene+shafts (6674) | **submit** | — | — | AO, shafts, dust |
| fog+particles.pass (6702) | **submit** | — | — | fog volumes, weather particles |
| exposure+post (6732) | **submit** | — | — | luminance chain, composite, present pass |
| overlay+ui.draw (6815) | **submit** (sgl) | 0.25 | 0.24 | HUD, minimap, window map, kui windows, cursor |
| commit (6855) | **submit** | (unmeasured separately) | | `sg_commit()`, swap done by sokol_app |

Frame `total` (work, not the presented rate): Heft AMD 1080p **10.69 ms mean**, Intel 12.46; Heng AMD **17.46**, Intel
29.31 (GPU-bound there: 24-40 ms, REPORT §2). Submit-only stages at Heft AMD sum to **3.93 ms** of 10.69; at Heng AMD to
**9.67** of 17.46 (the reflection is a second full world submit).

The uploads are the interesting seam. The per-frame uploads are five, and four sit in the *compute* region:
worldbuildings' instance buffer (`worldbuildings.cpp:1809`), foliage cells (`foliage.cpp:283`), grass cells
(`grass.cpp:542`) and particle instances (`particles.cpp:1054`); the fifth, the characters' skins/instances/list
buffers (`characters.cpp:4191`), is already inside the GL region (`main.cpp:6466`) though its CPU build is `buildSet`
(`characters.cpp:4213`). The only other in-frame upload is the weather atlas created when its load future resolves
(`weathermap.cpp:230`, one-shot); everything else that writes GPU resources does it at init/load or inside a pass. That
is what makes a render-thread split tractable (section 6).

Draw counts, REPORT §2: Heft ~3,900 a frame, Heng ~9,100 (characters one draw a part, shadows repeated per cascade).
205.8 instanced them: on its own chars camera the main pass went 1,148 → 243 draws and character shadows 2,498 → 481,
frame median Intel 13.93 → 11.09 ms, `draw.chars` 2.64 → 0.63, `shadow.chars` 2.03 → 0.27 (205.8 close reason). So the
character share of the submit above is now several times smaller; the world submit (shadow casters, buildings, terrain,
foliage, water, UI) is what remains.

## 2. Options

Each option names what leaves the main thread, the expected frame (est., REPORT §2 stage costs), the sokol work it
needs (`up/` = what upstream would want, `ours/` = local only; the clone flow is kenshi-k16.2.247, the vendored copy is
never hand-edited) and its risk. "Main" is the thread that runs the sim, pose, UI state and picking; "render" is the
thread that owns the GL/Vulkan context and sokol.

### 2.1 (a) A render thread that owns the context and sokol; a frame packet, double-buffered, one frame of latency

**What it is.** sokol_app's loop thread becomes the render thread. sokol_app's contract is already "all callbacks on one
thread, which need not be the one that called `sokol_main`" (`sokol_app.h:231`), and it does the present itself
(`sokol_app.h:13104` swaps GL buffers, `_sapp_vk_frame` presents on Vulkan), so the clean split is to run `frame()` as
the render body and move the *game* (today's compute region) to a second thread — no sokol patch is needed for the
thread itself. The logic thread builds a **frame packet**: cameras, light/shadow/AO/exposure parameters, the per-pass
CPU data (character skins/instances/draw lists, building instances, foliage cells, particle instances, UI command list),
and the uploads (CPU bytes). Two packet slots: the logic thread fills slot N+1 while the render thread consumes slot N;
each renderer's GPU-facing buffers are double-buffered with the packet. The render thread does all `sg_*`/`sgl_*`, the
uploads, and returns the GPU timer, draw stats, cursor-id readback and screenshots to the logic thread one
frame later (most are already two frames late).

**What moves.** Every submit stage in section 1 (Heft AMD 3.93 ms, Heng AMD 9.67 ms), plus the upload calls inside
buildings/foliage/grass/particles update and the char buffer write.

**Expected (est.).** Main = total − submit; render = submit + GPU-bound time.

| place / card | main before | main after (est.) | render (est.) | bound by |
|---|---|---|---|---|
| Heft AMD 1080p | 10.69 | **6.8** | ~4-5 (GPU 3.28) | main, just inside 144's 6.9 |
| Heft Intel 1080p | 12.46 | 6.9 | ~6-9 (GPU 8.30) | render/GPU, ~110-140 |
| Heng AMD 1080p | 17.46 | **7.8** | ~10-11 (GPU 8.06) | render, ~95-100 |
| Heng Intel 1080p | 29.31 | 8.9 | 20-40 (GPU-bound) | GPU, unchanged |

So (a) alone reaches 144 at Heft 1080p on the AMD card (and more comfortably once 205.8's already measured cuts are
folded in), but at Heng the render thread replaces the main thread as the cap: 144 at Heng needs the submit itself
smaller — section 3's shadow cache and/or 205.11's compute cuts on the main side. On the Intel card, Heng stays a GPU
problem (REPORT §5) and no thread split changes it.

**Sokol changes.** The split itself: none. Packet/presentation boundary: none (sapp presents on the render thread).
What does need sokol work later: exposing the context to a *third* thread (for OpenXR, section 5); a manual-present or
external-context API would be an `up/` candidate (a general engine feature), but is not needed for (a).

**Risks.** (1) The logic thread must not touch GL/sokol: every new upload or overlay draw has to go through the packet;
enforce by building the logic target without the sokol headers where possible. (2) The packet must copy or freeze
everything the renderer reads; today renderers read live `app` state (e.g. `app.buildings.render(mvp, light)`,
`main.cpp:6579`, reads the building set the next update rewrites) — the double-buffered packet per renderer is the
work, and a missed field is a tearing/lifetime bug, not a compile error. (3) Input events arrive on the render thread
(they are sokol_app events), so they need a queue to the logic thread and the UI must have a single owner (section 6).
(4) One frame of added display latency (16.7 ms at 60, 11.1 at 90) — irrelevant for the RTS camera, material for
head-tracked VR (section 5). (5) A main-thread stall (decode, load) no longer freezes the window: the render thread
redraws the last packet — a user-visible win against REPORT §4's 0.45-1.2 s stalls, and a test in section 7.

### 2.2 (b) Parallel compute on a job pool; submission stays on one thread

**What it is.** Keep `frame()` single-threaded for GL, but run the per-character/per-object compute on N workers:
pose (`characters.update`, 2.5-3.0 ms), cascade culling and draw-list building, `worldbuildings.update` (1.0-1.3),
`props` (0.6-0.75), `sim.peek+hover` (0.6-0.85), the snapshot decode/apply (`sim.update`, 0.9-1.0; kenshi-k16.2.205.10
is already moving the decode off this thread), particles. Results merge into the same per-frame lists before the submit.

**What moves.** Compute only: at Heft AMD ~6.0 ms of listed compute, at Heng ~6.4. With four workers and a per-character
split (the pose scales almost linearly; the report's `nosample` shows 1.5 ms of it is sampling/blending), the wall time
falls to roughly the largest task plus merge — est. 2-3 ms saved.

**Expected (est.).** Heft AMD 10.69 → **8.1-8.7** (~115-125 fps); Heng AMD 17.46 → **14.5-15.5** (~65-70). 60 fps
everywhere on AMD with room; 144 not reachable alone.

**Sokol changes.** None (all GL stays on the current thread), *except* that the upload sites in the compute region must
stay on the GL thread — either the workers produce plain CPU data and the main thread uploads (a small refactor of the
same sites), or the stage's upload moves after the merge.

**Risks.** Moderate to high: the client's state is one global `app`; the per-character work reads and writes shared
structures (`Characters`, `SimView`, `WorldBuildings`), so the split needs ownership rules and a test for determinism
(replay the same wire dump twice and compare the frame's stage JSON/screenshots). Data races here are silent and
intermittent, and hard to see on the shared desktop. The gain is also partly duplicated by 205.11's tunables (pose
cache, dirty tracking on props/buildings), which are cheaper to finish.

### 2.3 (c) Multi-threaded command recording (Vulkan only)

**What it is.** Record the frame's command buffers on N threads, one per pass or per draw-list chunk, then submit the
primary buffer. Only Vulkan can do this; GL has no command buffers.

**What sokol would need.** sokol's Vulkan backend records every pass into one per-frame command buffer from one thread
(`sokol_gfx.h:23242` allocates `frame.slot[i].command_buffer`; `sg_begin_pass`/`sg_apply_*`/`sg_draw` record into it
immediately; `_sg_vk_commit` submits, `sokol_gfx.h:24367`). Parallel recording needs secondary command buffers (one per
worker and per pass), per-thread pipeline/descriptor state, correct render-pass inheritance and barriers, and a
swapchain pass that can be inherited. That is a real sokol feature, not a local hack: `up/render-thread-recording`
(the multi-threaded-recording design upstream has discussed) for the API and behaviour, `ours/` only for shortcuts we
would not offer. It also wants the client's draw lists to be built thread-safely (option (b)'s lists, without the
single-threaded submit).

**Expected (est.).** Only the submit splits: Heft AMD 10.69 → ~7.7 (submit 3.93/4 ≈ 1.0); Heng AMD 17.46 → ~10.2
(9.67/4 ≈ 2.4). Combined with (b): Heft ~5.2, Heng ~7.7 (optimistic upper bound; per-thread driver behaviour and
barriers will eat part of it). 144 needs (b)+(c), or (a).

**Risks.** Highest of the three: it is blocked on Vulkan being a working renderer (kenshi-k16.2.246, 170, 37), it is a
new sokol feature with real correctness surface (descriptor reuse, barriers, layout transitions), and it is worth doing
only if the Vulkan backend is the chosen main path. It is the highest-risk/lowest-certainty way to reach 144.

### 2.4 (d) Combinations

| combination | Heft AMD main (est.) | Heng AMD main (est.) | backend | 144? |
|---|---|---|---|---|
| (a) render thread | 6.8 | 7.8 (render-bound ~10) | GL or Vulkan | Heft yes, Heng no |
| (a) + shadow cache (§3) | 6.4 | 7.8, render ~7-8 | either | Heft yes (comfortably post-205.8), Heng ~135 |
| (a) + §3 + (b) or 205.11 | 6.0-6.4 | 6.5-7 | either | both places, AMD 1080p |
| (b) alone | 8.1-8.7 | 14.5-15.5 | either | no |
| (b) + (c) | 5.2 | 7.7 | Vulkan | borderline |
| (a) + (c) | 6.8 | 7.8, render ~6-7/4 recorders | Vulkan | Heng borderline |

Recommendation: **(a) first, §3 next, (b) only if the numbers still bind after 205.11, (c) only if Vulkan becomes the
main backend and the submit still binds.** (a) is the only option that is backend-neutral, needs no sokol patch for the
thread split, and gives the stall-robustness win as a side effect.

## 3. Shadow-map caching

**Today.** Four cascades share one 6144² depth atlas of 3x3 2048² tiles; 8 tiles hold two full sets (tile (2,2) unused),
so a sun step draws the next set while the shading crossfades the old one (`shadow.hpp:43-48, 83-107`). With stagger off
(the default, `shadow.hpp:81`), `Shadow::update` sets `render_[c]` true for every cascade every frame (`shadow.cpp:194-195`):
all four tiles are cleared and every caster — terrain, buildings, features, foliage (`main.cpp:6478-6482`) and characters
(`main.cpp:6486-6493`) — is submitted into each, **even on frames where the cascade matrices are identical**. REPORT §2:
985 world shadow draws + 1,096 character-shadow draws at Heft; 1,806-1,817 + 3,199-3,216 at Heng; CPU 2.34 ms (Intel
Heft) / 3.63 (AMD Heng); GPU 5.61 (Intel Heft) / 9.56-15.90 (Intel Heng) / 1.34-2.07 (AMD).

`render.shadow_crossfade` (`shadow.cpp:117-137`) already limits *when* the light direction changes (a 0.25° step,
`shadow.cpp:26`, then a 0.75 s fade). Between steps only the camera moves, and the cascade centre is snapped to whole
texels (`shadow.cpp:210`), so the static casters' tiles are unchanged for long stretches. The cache is therefore a
natural extension of what the crossfade already built.

**Proposal.** Cache the static casters (terrain, buildings, features, foliage) per (atlas set, cascade) — 8 tiles —
and redraw a tile only when its key changes. Characters are never cached. Two ways to combine the cached statics with
the per-frame character shadows:

1. **Separate dynamic map (recommended).** The static tiles stay the cached map; characters draw into their own
   per-cascade depth atlas, cleared every frame. The shading samples both with the same cascade lookup and takes the
   nearer. Costs: one sampler and the two-map min in `shaders/shadow.inc`; character shadow draws stay as today (481
   after 205.8 on its camera).
2. **Restore by depth write.** Before drawing characters into a cached cascade, restore its tile from the cache: a
   fullscreen quad per cascade writing `gl_FragDepth` from the cached depth (portable through sokol, unlike a depth
   blit, which has no sokol API and is backend-dependent). Costs: 4 extra fullscreen draws (at most) a frame and no
   shader change; the character depth never touches the static cache.

**Cache key / invalidation.** The tile's `lvp` and depth range (camera position — including `camLz`, which is *not*
texel-snapped today, `shadow.cpp:210-211,230`; snapping it too would make the key stable and is under the caster bias
cap, `shadow.cpp:20,217-218` — measure the visual delta), the sun step (`beginStep` flips the set, `shadow.cpp:302-317`),
`Shadow::invalidate()` (`shadow.cpp:345`), streaming (a terrain/foliage/building cell enters or leaves), and any animated
or stateful caster: a door's open pose, a wind-aimed part's turn (`worldbuildings.cpp:1823` motion_ per part), a
destroyed/built part, a limb. Characters excluded.

**Fit with `render.shadow_crossfade`.** The cache is per set, so a sun step rebuilds the statics into the other set once
(the step is seconds apart even at 5x: 0.25° at the game's sun rate), and the fade then blends two complete maps exactly
as today. While a fade runs, the previous set's *character* shadows are from the step frame (up to 0.75 s stale) — today's
behaviour too; optionally redraw characters into both sets during the fade (double the character shadow draws for 0.75 s).

**Expected (est., REPORT §2).** Static-caster submit leaves most frames: Heft AMD `shadow.pass` 1.64 → ~0.7, Heng 3.63 →
~2.3; GPU: Heft AMD 1.34 − character_shadows 0.42 ≈ 0.9 → ~0.4, Intel Heft 4.9 → ~0.7, Heng AMD ~0.95 → ~0.5, Intel Heng
7.4-12.3 → ~2-3.5; and 985 / 1,806 world draws a frame go away on cached frames. The Intel Heng shadow pass is the largest
GPU number in the report, so this is the largest GPU win available — and it is independent of (a), shrinking the render
thread's frame too.

**Risk.** A cache bug is a missing or stale shadow, invisible in a still frame and visible when moving. Verify with
`tools/scene_test.sh` (includes shadow scenes) plus a moving-camera script comparing a cache-on/off build frame by frame;
keep a config key to disable the cache.

## 4. The Vulkan backend (kenshi-k16.2.246)

**What it adds on top of each option.** Vulkan does not change the *client-side* submit code (a draw call is a draw call);
it lowers the driver cost per draw (no GL validation/state translation), which matters most for the ~3,900-9,100 direct
draws the report measured before 205.8's instancing. It is a prerequisite for (c) (command recording is a Vulkan concept)
and the preferred backend for OpenXR. sokol's Vulkan backend has its own per-draw cost (a uniform block per draw; the
`VULKAN_UNIFORM_BUFFER_OVERFLOW` of k16.2.170 skips draws when it runs out), so the backend must be measured, not assumed
faster, on this client.

**What to do first, in order:** (1) k16.2.170 (uniform buffer sizing, draws skipped) and k16.2.37 (3D flipped) via the
sokol clone flow (k16.2.247: `master` = upstream, one branch a fix, `up/` for what upstream would want, `ours/` for what
only we need, vendored copy a straight copy of `kenshi`, `VERSION.txt` records both hashes, commits with the GitHub noreply
email); (2) the Heft frame on Vulkan vs GL, CPU and GPU, before and after 205.8's instancing (k16.2.246's acceptance);
(3) then this design's split, which is backend-neutral — sokol_app owns the present on Vulkan too (`_sapp_vk_frame`), so
(a) needs no Vulkan-specific patch; (4) (c) only if the Vulkan submit still binds after (a) and the instancing.

**OpenXR's Vulkan path** needs external images (`VR.md`): the vendored sokol has no injection of external Vulkan images
and no native image query, so that is an `up/` change (or a newer upstream) — a separate, later piece of sokol work.

## 5. OpenXR and stereo (90 fps, two views)

**Scope note first.** `docs/VR.md` (2026-11-02) scopes this as *stereoscopic play, not a VR game*: two eyes, the RTS
camera, no head-tracked camera and no VR frame rate; the OpenXR output is one quad composition layer per eye. This item's
brief ("90 fps stereo, pose prediction") describes head-tracked VR, which VR.md puts out of scope. Both keep the rule
below; only the head-tracked one constrains latency. **This scope question should be settled before any XR work is
scheduled** (see Open questions).

**The Camera-per-view rule.** Renderers take a `Camera` (`camera.hpp`) and never the window size or main.cpp's camera
globals; `current_camera` (`main.cpp:2218`) builds it once a frame. A stereo frame builds two cameras (off-axis,
asymmetric frustums, eye separation a config key — VR.md "Rendering" 2-3), culls with the union frustum once, runs the
shadow pass once (it is view-independent) and runs the world/fog/particles/post passes per eye. In the packet, the
camera field becomes a small array of cameras, so both (a) and the current single-thread path carry it unchanged.

**The 90 fps budget.** 11.1 ms for the eye pair. REPORT §2's AMD GPU total is 8.06 ms at Heng (reflection 1.86 + shadows
2.07 + ...) and 3.28 at Heft; two eyes roughly double the per-view passes, so **est.** Heng ≈ 14-16 ms and Heft ≈ 6-7 ms
on the AMD card before any saving. Heng stereo at 90 fps therefore needs §3's shadow cache (it removes ~0.5-1 ms of GPU
and is shared between eyes anyway), the instanced draw, and probably a reduced eye resolution; Heft is close. The Intel
card is not a stereo target (Heng GPU 24-40 ms today).

**Latency: packet vs pose.** The world is already ~100 ms behind the newest wire sample (CLIENT.md, "Live sim link": 2
ticks at 1x) — that delay is the sim's, not the renderer's. Head-tracked stereo adds a *second* latency that must not be
solved by that ~100 ms: head pose → photons, target ≤ ~20-25 ms. A one-frame packet adds 11.1 ms at 90 Hz. Two ways to
keep it in budget, both compatible with (a):
- **Late pose latch.** The XR pose is not part of the packet's world data; the render thread samples the latest predicted
  pose (a lock-free "latest pose" cell written by the XR thread/runtime) immediately before building the two view matrices
  and paints the packet's world from it. The world content is one frame old; the head orientation is not.
- **Reprojection (timewarp).** If even that is too much, re-project the two eye images with the newest pose in the
  composite (a per-eye fullscreen pass using depth). More work, the standard fallback.
For VR.md's quad-layer plan neither is needed: the compositor keeps the quad stable, so packet latency is invisible. That
is another reason to keep the XR scope decision separate from the render-thread decision.

**What XR needs from sokol (later).** GL: external GL textures as `sg_image_desc.gl_textures` (VR.md says this works) and
the session's GLX context — which the render thread already owns under (a). Vulkan: external image injection, a sokol
change. Composition layers are outside the client's sokol use. So (a) *helps* XR (the context owner is defined), but XR
is not a reason to do (a) now.

## 6. What breaks or must change

- **Uploads.** The four compute-region sites move to the render thread as packet-driven uploads — worldbuildings'
  instance buffer (`worldbuildings.cpp:1809`), foliage cells (`foliage.cpp:283`), grass cells (`grass.cpp:542`), particle
  instances (`particles.cpp:1054`) — and the characters' CPU build moves to the logic thread while their upload stays
  render-side (`buildSet`/`uploadSet`, `characters.cpp:4213`/`4191`). Texture/image creation for streamed terrain chunks
  happens inside `Terrain::render` (`terrain.cpp:181`) and the weather atlas when its future resolves
  (`weathermap.cpp:230`) and is already render-side. Everything else is init/load. A new upload written on the logic
  thread must be a review checklist item; the compiler will not catch it. A dev build does: `client/src/dev/sgowner.cpp`
  (kenshi-k16.2.205.12.19) hooks every traced `sg_*` entry point (sokol's `SOKOL_TRACE_HOOKS`) and aborts with a stack on
  a call off sokol's owner thread; the render thread takes ownership with `dev::sgOwnerTake()` when it starts.
  `--sg-owner-probe` proves it fires. Not seen: `sg_query_*`, `sg_isvalid`, setup/shutdown (no hooks), and the abort
  comes after the offending call has run.
- **GPU timer.** `GpuTimer` (`gputimer.hpp:11-35`) issues `GL_TIME_ELAPSED` queries; begin/end/collect must run on the
  render thread (it already triple-buffers and reads two frames late). Publish the per-section ms to the logic thread for
  the HUD, the `KCLIENT_GPULOG` stages line and the scene JSON. `KCLIENT_PROFILE=0` builds are unaffected.
- **Draw stats.** `drawstats` (`drawstats.hpp:14-72`) counts at the submit wrappers, i.e. on the render thread; publish
  the per-pass counters with the packet's return slot. The `sgl` counters (`DRAWSTATS_QUADS/TRIANGLES/LINES`) follow the
  UI move (below).
- **Scene test.** `scene_test_pre/post` (`main.cpp:5507/5538`) run on the logic thread and drive determinism; the PNG
  write comes from `save_screenshot` (`main.cpp:2471`), whose `glReadPixels` (`main.cpp:2479`) must become a render-thread
  request with the pixels returned to the logic thread. Scene ordering, `world_loading()` and the baseline comparison are
  logic-side and keep working.
- **Dev tools.** `save_screenshot` (screenshots, `--screenshot`, F12, `--loading-shot` — the last calls it then
  `_exit(0)`, `main.cpp:5017`), the probe readbacks (`main.cpp:2532-2533`) and the cursor-id pass (`main.cpp:6636-6646`,
  a hand-written GL FBO/PBO ring, `idbuffer.cpp:60`) are all logic-thread calls into GL today. Each becomes a request +
  a one-frame-later answer; the ID buffer is already two frames late, so its visible behaviour does not change. The
  debug overlays (navmesh, probes, trails, markers, HUD) are `sgl` draws and must build CPU lists (below).
- **UI (kui) draw.** `ui::draw()` (`ui/ui.cpp:336`) is immediate mode over `sokol_gl` (`ui/gfx.cpp`): widgets emit
  vertices as they walk the tree, and the tree is mutated by event handling. The packet needs the UI as data, so kui's
  gfx backend should emit a CPU vertex/command list on the logic thread which the render thread replays through `sgl`
  (a bounded change: `gfx::begin/end` and the `sgl_*` call sites in `ui/gfx.cpp`; the library is the UI session's, so
  coordinate the change). The HUD, minimap, name labels, cursors and debug overlays use the same default `sgl` context
  and the trails use their own (`main.cpp:6789`) — same treatment.
- **Input events.** `event()` (`main.cpp:10521`) mutates app state directly on the sokol_app thread; under (a) that is
  the render thread. Events become a queue drained by the logic thread at frame start (the event struct is a POD).
  `ui::event` belongs to the logic thread with the UI state.
- **Window size and vsync.** `sapp_width/height/frame_duration` are sokol_app calls (`main.cpp:6055,6219,825,1352`),
  legal only on the sapp thread. The logic thread reads published values: the camera already uses `windowgate::width/
  height/aspect()` (`main.cpp:2225-2229`), and the window gate (k16.2.222) already debounces changes; publish the applied
  size, the frame duration and the fps/HUD fields with the packet. Target rebuilds on resize (`Hdr::makeTargets`,
  `hdr.cpp:113`) and the idbuf/reflection targets happen render-side from the packet's size.
- **Every frame path draws.** `frame_blank` (`main.cpp:4665`), `frame_await_target`, `frame_loading` (`main.cpp:4990`),
  `frame_view`, `frame_title` all end in `sg_begin_pass`/`sgl`/`sg_commit`; the logic thread must choose the path and
  fill the packet even for a black or loading frame. The load steps' uploads (textures, atlases, buildings) run on the
  logic thread today — startup stays single-threaded until `app.loading()` first ends, and every path after that is a packet path
  (the exact boundary: §7.1 rule 7).
- **Logging.** sokol's logger must be reentrant (`sokol_gfx.h:2043`); stderr writes from both threads are fine.
- **windowgeom.** `windowgeom::restore()` (`main.cpp:5976`) reads/sets window state and is sapp-side (render thread);
  its inputs (config, script flags) are logic-side and already decided before the split.

## 7. Recommendation and phases

Each phase is shippable on its own and ends at a measurable acceptance. All numbers are at the report's cameras, AMD
1080p (the user's card) unless named.

- **P0 — shadow static-caster cache (§3).** Independent of the thread, the largest GPU win, and it shrinks (a)'s render
  frame. *Accept:* `tools/scene_test.sh` 0 changed (or one intended bless), `KCLIENT_STAGES=0` + `KCLIENT_GPULOG=1` Heft
  AMD `shadow` GPU ≤0.8 ms and `shadow.pass` CPU ≤1.0; Heng AMD `shadow` GPU ≤1.0; a moving-camera A/B frame-by-frame
  identical between cache on/off.
- **P1 — the packet split on one thread.** Introduce the packet struct (cameras, parameters, per-pass CPU data, upload
  bytes, UI list) and the double-buffered slots; the same thread builds and consumes it. No thread yet, so every
  behaviour is testable in isolation. *Accept:* scene_test 0 changed; frame `total` within ±0.2 ms of base; 0 `[sg][error]`;
  `dev_off_check.sh` PASS.
- **P2 — the thread split (§2.1).** sokol_app's loop thread is the render thread; a logic thread runs the sim/pose/UI/
  picking and fills packets; input queue; uploads and readbacks move behind the packet; startup stays single-threaded
  until the first world frame. *Accept:* scene_test identical; Heft AMD main mean ≤7.0 ms and render ≤5.0; ≥144 fps at
  1080p; Heng AMD main ≤8.0, render ≤7.5; a scripted snapshot burst (the REPORT §4 1.2 s decode stall) keeps the window
  drawing (no stall-freeze; the existing `stall` event still logs the logic-thread frame); 0 `[sg][error]`; `dev_off_check`
  PASS.
- **P3 — compute pool (b), only if the main side still binds** after 205.11 (pose cache, dirty props/buildings, reflection
  skip) lands. *Accept:* scene_test identical on a replay twice (determinism); main ≤6.5 ms at both places.
  First piece in (kenshi-k16.2.205.12.102, review .85 T22): `Characters::update`'s pose pass is a parallel-for over the
  characters on a persistent `WorkPool` (`client/src/app/workpool.hpp`; `anim.pose_threads`, default 4 with the logic thread,
  1 = serial). Each pose writes only its own character; a carried body's `carryPlace` keeps the serial order's read of its
  carrier (posed this frame if earlier in `chars_`, the state before the loop if later), so the output is the serial
  loop's bit for bit. Heft overlap, Intel card: `chars.update` 2.6-3.2 ms serial -> 1.2-1.4 parallel at like load.
- **P4 — Vulkan (k16.2.246).** After 170/37 and the Heft Vulkan-vs-GL measurement: re-run P0-P2's numbers on Vulkan; add
  multi-record (c) only if the Vulkan render thread's submit still binds.
- **P5 — OpenXR (VR.md path).** SBS/anaglyph output first (no SDK, testable with `--screenshot`), then the XR quad layer;
  head-tracked stereo only after the scope decision in section 5.

### 7.1 The packet protocol (kenshi-k16.2.205.12.20)

This is the contract between the logic thread and the render thread for `gPackets[2]` (`client/src/main.cpp`,
`struct FramePacket`). It settles the design review kenshi-k16.2.205.12.15 (render1, fable; the lead adopted it in
kenshi-k16.2.205.12.17). The review's file is `~/git/kenshi/kenshi-inspect/findings/kenshi-k16.2.205.12.15/REVIEW.md`.
A tag such as *[R1 flip]* names the review point that a rule settles: R1 is the review's section 1, R2.3 is section 2
change 3, and so on. Where a rule needs code that has not landed yet, the item that lands it is named. Until then the
rule constrains which modes may run (rule 9).

**Terms.** The *render thread* is sokol_app's loop thread (Lead decisions, "(a) as written"). It runs `frame()`, the
event callback and `cleanup()`. The *logic thread* runs `logic_frame`. A *packet* is one slot of `gPackets` together
with its `serial`, a counter that logic increments for each packet it fills.

1. **Slot states: free, filled, drawing.** *[R1 proposed protocol]* One mutex and one condition variable guard every
   slot's state and serial. Neither guards the slot's contents.
   - *free*: owned by logic. Only logic may write a free slot, and it writes without holding the mutex.
   - *filled*: logic has finished writing the slot (state set under the mutex). Nobody writes it.
   - *drawing*: owned by the render thread. It reads the slot; nobody writes it.
   Only these transitions are allowed. Free→filled (logic). Filled→drawing (render, at the flip). Drawing→free (render,
   at the flip when another slot is taken, and at quit). Filled→free (logic, only when its bounded wait in rule 5 runs
   out). At most one slot is drawing and at most one is filled. So logic is at most one frame ahead, and it never
   writes a slot that the render thread can see.
2. **Every field of a slot is written every frame.** *[R1 "a slot it did not refill keeps its last list"]* With two
   slots, a field that logic did not write holds the value from two frames back. Logic writes every field on every
   path, including an empty list when grass, particles or any other renderer is off. `Grass::FrameData`'s "a slot it
   did not refill keeps its last list" rule goes away (kenshi-k16.2.205.12.16 or the queue item below). A packet is a
   **pure snapshot**: it carries no one-shot work. Creates, frees, uploads that must happen exactly once, and readback
   requests all travel in ordered queues that are never dropped. Those queues are the resource job queue
   (kenshi-k16.2.205.12.22, *[R2.2]*) and hook 4's readback queue. Grass's `uploads`/`frees` and the particle texture
   creates move into the resource job queue *[R1 "one-shot deltas"]*. After that move, a packet can be drawn twice or
   skipped without any effect beyond what is shown on screen.
3. **The flip is on the render thread.** *[R1 flip]* At the start of each `frame()` and under the mutex, the render
   thread looks for a filled slot. If it finds one, the filled slot becomes drawing, the slot it drew before becomes
   free, and it notifies logic. Logic never flips. It only marks its own slot filled.
4. **The render thread never waits without a bound, and redraws its current packet.** *[R1 "render thread never
   waits"]* The render thread is sokol_app's loop thread. If it blocks, X events stop and a quit is never processed.
   Its wait for a filled slot is bounded at about one refresh: the display's refresh period (X11 RandR on the sapp thread,
   published with `view`'s window snapshot; 16.7 ms when unknown), or `1000 / gfx.fps_cap` under a cap with vsync off,
   clamped to [4, 20] ms. Never a measured interval: with vsync off sapp's frame duration is the callback interval, which
   the no-packet callbacks shorten themselves (kenshi-k16.2.205.12.87, review .85 section 1.8). When
   no packet arrives in time, it draws its current drawing slot again. This is P2's acceptance ("a 1.2 s decode stall
   keeps the window drawing"). Before the first packet, the render thread draws the blank path (black and the load
   screen, as `frame_blank` does today).
5. **`render_frame` is legal twice on one packet.** *[R1, consequence of rule 4]* Drawing the same serial again changes
   nothing but the pixels:
   - the per-frame uploads (`chars.upload`, `buildings.upload`, `particles.upload`, `arrows.update`) are NOT skipped on a second draw of a
     serial: they rewrite the same data from the packet, which costs CPU and is harmless (overlap review kenshi-k16.2.205.12.101 A; `Grass`
     alone keys on the serial);
   - readback requests and resource jobs are served on the first draw of a serial only (rule 2: they are not in the
     packet);
   - every counter that a draw increments today (`app.frames++`, the `cpuMs` family, `app.fogDrawn`,
     `app.adaptedOnce`, `app.sglShown`) moves to the FrameStats slot, stamped with the serial
     (kenshi-k16.2.205.12.24 and kenshi-k16.2.205.12.27).
   A redraw reads only the packet and render-owned state. So it is legal only once `render_frame` no longer reads
   `app.` (kenshi-k16.2.205.12.27); see rule 9 for lockstep.
6. **Logic's wait is bounded too.** *[R1 "logic's wait is bounded"]* After it marks a slot filled, logic waits until
   the render thread has taken it. A hidden or occluded window can hold the swap for a long time, and the sim link must
   keep being pumped. So in overlap mode the wait is bounded at a fixed 100 ms (`kLogicBound`, logicthread.cpp; the lead's
   call 2026-11-18, kenshi-k16.2.205.12.33: the code stands unless the hidden-window run shows the ~9 Hz pacing hurts). When the bound runs out, logic sets its untaken slot filled→free under the mutex, but only if the slot is
   still filled; if the render thread took it meanwhile, it proceeds normally. Then logic runs its next frame and
   overwrites the slot. That skips a packet, which rule 2 makes harmless. **Overlap mode is not enabled before the
   one-shot deltas have left the packet.**
7. **The logic thread starts once, when loading first ends, and never stops before quit.** *[R2.3]* `frame()` runs
   `logic_frame` and `render_frame` on the render thread, as today, until `app.loading()` first returns false. It then
   starts the logic thread (only when `render.thread` is not off) and never runs `logic_frame` itself again. This
   replaces §6's "until the first world frame". Session load, the creator scene, the title path and the viewer are
   packet paths after that point. Their uploads go through the resource job queue (kenshi-k16.2.205.12.22; the
   untraced sites are kenshi-k16.2.205.12.21). `gFrameRan` is set on the render thread before the logic thread starts
   (kenshi-k16.2.205.12.23, *[R1 gFrameRan]*). `frame_loading`'s load steps stay on the render thread, which is the one
   thread there is at that point.
8. **Quit: stop flag, notify, join, then destroy.** *[R1 quit, R2.4]* `sapp_request_quit` is applied on the render
   thread (hook 3's `sappcmd` queue for a quit requested by logic), and `cleanup()` runs there. The order is:
   1. set the atomic stop flag;
   2. notify the condition variable, so that a logic thread asleep in rule 6's wait wakes up (the frame limiter's sleep
      is at most one cap step);
   3. join the logic thread;
   4. only then run any `sg_destroy_*`, `sgl_shutdown` or `sg_shutdown`.
   Logic checks the flag at the top of each frame and after each wait. A done callback that calls `_exit(0)`
   (`--loading-shot`) or `request_quit()` (the resize burst) runs on the logic thread and is fine: nothing joins from
   inside logic, and `_exit` skips `cleanup()`.
9. **`render.thread` = off / lockstep / overlap.** *[R6, R1 "scripted and scene_test runs need lockstep"]* It is a
   config option, read once at startup, and the default is off.
   - *off*: one thread. `frame()` is `logic_frame(gPackets[0]); render_frame(gPackets[0]); sappcmd::flush()`, exactly
     as on `client` today. Nothing in this protocol runs.
   - *lockstep*: the logic thread exists (rule 7), and the two threads strictly alternate. Logic frame N fills a slot
     and waits (rule 6's wait, no skip) until the render thread has drawn serial N, committed, and finished N's
     readbacks. Only then does logic run N+1, with N's done callbacks first (hook 4). The render thread runs only
     while logic waits, so there is no data race by construction, and `render_frame`'s `app.` reads stay legal. In
     lockstep, rule 4's bounded wait does not redraw while logic runs, because a redraw would read `app.` during
     logic's writes. When the bound runs out, the frame callback returns without a pass and repeats the kept frame; a run
     of such frames over 50 ms logs one line (`logicthread::kMissLogMs`), a shorter run only goes into the `pace:` line
     (kenshi-k16.2.437.1; `client/README.md` "Vsync and present pacing"). Rule 6's skip is never taken in lockstep. When the bound runs out, logic polls the sim link without
     stepping the world and waits again: determinism comes before liveness.
   - *overlap*: **full overlap** since kenshi-k16.2.205.12.118 (stage 2: no gate in the world frame; rules 4, 5, 11 and 12 below). Logic fills
     packet N, waits only until the render thread has *taken* it (rule 6's wait, with its skip), then runs the whole of
     frame N+1 (the event drain, the script, the camera, `sim.update`, the UI, `chars.update`, the weather, `shadow.update`,
     the prepares, the packet) while N is drawn or redrawn. The frame period is max(A + B, R + P) instead of lockstep's
     A + B + R (R = the draw's CPU). It rests on the deltas out of the packet (rule 2, proved by kenshi-k16.2.205.12.93's
     digest and poison), an `app`-free `render_frame` (kenshi-k16.2.205.12.27), the shadow's and the buildings' draw state in the
     packet (T17-T20) and the stray writes gated or moved (kenshi-k16.2.205.12.95). A gate remains only for frames that are not plain world
     frames and for rare writes (rule 12). It is still opt-in: its default waits on the overlap review's go
     (kenshi-k16.2.205.12.101) and the user's play test (kenshi-k16.2.205.12.106).
   **Scripted runs (`--script`, `--screenshot`, the headless tools) and `tools/scene_test.sh` run lockstep** whenever
   `render.thread` is not off: overlap prints `render.thread=overlap: a scripted run runs lockstep` and falls back to
   lockstep for them. The exception is `debug.overlap_scripted=1` (dev; review A3): the scripted run then really
   overlaps, with rule 6's skip, so a ThreadSanitizer run can test overlap; frame-exact shots are not expected from it. Frame counts, `kScriptDt` steps and shots are
   then per logic frame, and `scene_test` stays identical. Lockstep lands before the rest of hook 7 and before hook 8
   (kenshi-k16.2.205.12.25).
   *Landed (kenshi-k16.2.205.12.25):* `client/src/logicthread.{hpp,cpp}` holds the slot states, the mutex and condition
   variable and the thread; `render.thread` is a `cfg::Text` (off / lockstep / overlap, read once by
   `logicthread::configure()`; overlap prints a loud "IS NOT IMPLEMENTED" line and runs lockstep). `frame()` runs as before
   until the first frame that is not loading, then `start_logic_thread()` (rule 7) and, from the next callback on,
   `frame_lockstep()`: the window state as before, a wait of `clamp(sapp frame duration, 1, 20)` ms for a filled slot, the flip
   (rule 3), `render_frame` + `sappcmd::flush`, then logic is let go. Logic's wait is bounded at 100 ms; each expiry runs the
   idle hook, `SimView::pumpLink()` (socket bytes into the decoder, HELLO and the 5 s ping; `SimView::update` alone steps the
   render tick), and only while the slot is still *filled*, under the hand-off mutex, so the render thread cannot flip and start
   reading `app.` meanwhile. A frame callback whose wait runs out draws no pass and blits the last composed frame back
   (`client/src/lastframe.{hpp,cpp}`: each drawn frame's back buffer is copied into a render-owned renderbuffer after its
   commit; GL only). `cleanup()` starts with the join (rule 8). Xlib calls from logic (`windowgeom::resizeWindow`: the `resize`
   op and the resize burst) go through `sappcmd::post`, which runs them on the sapp thread at the flush. The sokol thread-owner
   assert stays as installed: the render thread owns sokol, any `sg_*` from the logic thread aborts; `--sg-owner-logic`'s
   scopes act only on the sapp thread. The HUD's cpu figure shows `logic` and `render` ms (rule 11) while the thread runs.
   Dev ops: `stall MS` (logic sleeps), `renderstall MS` (the render thread sleeps at its next frame start);
   `KCLIENT_LOCKSTEP_STALL_SHOTS=DIR` writes the back buffer of the first frames without a pass, `KCLIENT_LOCKSTEP_NOKEEP=1`
   skips the blit.
10. **Resize: the packet carries its size.** *[R1 resize]* `windowgate::frame()`, `view::snapshot()` and
    `windowgeom::restore()` run on the render thread at its frame start, before the flip, and publish the applied size
    and the frame duration (kenshi-k16.2.205.12.23). Logic reads only that published copy, at its own frame start. Each
    packet carries the applied width and height that it was built for. The render thread rebuilds its targets
    (`Hdr::makeTargets`, idbuf, reflection) from the packet's size, never from the live window. Readbacks use the size
    of the packet they were drawn from, not `view::` *[R3.7]*. A packet built for the old size is drawn scaled, which is
    what the gate's hold already does.
    *Landed (kenshi-k16.2.205.12.23):* `frame()` runs the three at its start and they publish under a mutex;
    `logic_frame` copies them with `view::take()` (where the snapshot was) and `windowgate::take()` (after the event
    drain, where the gate's poll was), and every `view::` / `windowgate::` accessor reads that copy. `FramePacket` has
    `width`/`height` (the applied size, written on every path); the scene, mask and id passes, the post chain and the
    stats slot take them from the packet. A screenshot request records the window size at the request. `gFrameRan` is
    set in `frame()`. The logic that ran between the draw's last pass and `sg_commit` (the script's post commands, F12,
    the shot captures, the session load's progress, the resize burst, the scene test, `--loading-shot`) is
    `logic_tail`, run at the end of `logic_frame`. What it needs from the drawn frame is a readback request
    (`readback::screenshot`, `probe`, and `after` for the draw's counters and for work that must follow the draw,
    e.g. a script `quit` behind its shot), served by `readback::serve()` after the last pass. Each request carries the
    serial of the packet logic was filling (`readback::beginPacket`); serve reads only the packets drawn so far
    (`readback::drawing`) and keeps each result by value. The dones run on the logic thread at the top of the next
    `logic_frame` (`readback::runDone`), in request order; a request a done makes belongs to the packet being filled. A
    done captures by value (a frame number it prints is captured at the request); a GPU read a done needs that is not a
    pixel read of the frame (`Hdr::read`'s luminance) is a `readback::sample` request made before it, whose read runs at
    the serve point and whose values reach the done by value. The scene test does not count
    frames while its floor step or its shot is out (`SceneTest::waiting`). The frame stats work the same way: the render
    side fills its own slot, `framestats::publish(serial)` copies it under a mutex after `fillDraw`, stamped with the
    render frame number and the packet serial, and `logic_frame` takes the copy once (`framestats::take`) before the
    dones run; logic reads only `framestats::stats()`, the HUD and GPULOG read the render side's `drawing()`. On one
    thread logic N+1 starts right after draw N, so every done and every stats read sees what it saw before
    (kenshi-k16.2.205.12.24).
11. **Frame pacing.** *[R6 "frame pacing"]* In off and lockstep modes, nothing moves: `gfx.fps_cap`'s limiter sleeps in
    `logic_frame` and vsync blocks in the swap. In lockstep the two alternate, so the limiter paces both threads. In
    **overlap the render thread paces**. Vsync is in its swap, and the `gfx.fps_cap` limiter moves to the render
    thread's frame start, before the flip. Its sleep is at most one cap step, so it obeys rule 4. Logic is paced only by
    back-pressure, its wait for its slot to be taken (rule 6). It measures its own `dt` and its `stall` evlog gap on its
    own clock. The HUD shows both rates: logic ms and render ms are split, because `pk.cpu0` cannot span two threads
    *[R3 "pk.cpu0"]*.
    *Stage 2 (full overlap, kenshi-k16.2.205.12.118):* **the limiter is the render thread's** (`overlap_limiter`, main.cpp, at the frame start before the
    flip; one cap step at most), logic's `logic_paths` skips it, and a frame whose wait for a packet runs out **redraws the drawing packet** (rule 4;
    `logicthread::renderFrame` returns true, the frame is kept as any drawn one) unless the gate is closed (rule 12). The pace log counts
    those as before; the 50 ms line reads "frames redrew the last packet (overlap)".
    *Stage 1 (gated overlap, kenshi-k16.2.205.12.96):* the render thread draws each packet once and never redraws, so
    logic still paces: the limiter stays in logic's part A, the render thread's no-packet wait is rule 4's bound
    (`lockstep_bound_ms`: with vsync on and `gfx.fps_cap` under the refresh, the longer of the refresh period and the
    cap's step, in both modes), and a frame without a packet blits the kept frame as in lockstep. The limiter's move to
    the render thread comes with stage 2's redraw.
12. **The gate.** *Stage 2 (kenshi-k16.2.205.12.118): the world frame has no gate: `logicthread::renderIdle()` is gone from `logic_paths`' part B, and the whole
    frame runs while the render thread draws (or redraws) the previous packet. The invariant that replaced the stage-1 one: logic never writes the drawing slot, and
    whatever logic writes outside the packet is not read by a draw, except behind a closed gate.* `renderIdle()` **closes** the gate (`gClosed`, under the mutex):
    until logic's next fill the render thread draws nothing, not even a redraw (it shows the kept frame, as in lockstep), so a load, the title, the creator, the
    lab and a dev op write the renderers under no draw. Logic's fill opens it. Rule 6's skip keeps the invariant: a packet taken back was never drawn, and
    the one-shot work it carried (portrait jobs, a shadow cascade's redraw) is handed back to its owners by `Hooks::skipped`
    (`PortraitTable::giveBack`, `Shadow::lostPacket`; kenshi-k16.2.205.12.z79f). What logic writes must be its own or reach the draw through the
    packet (review section 1.3's list; kenshi-k16.2.205.12.95 moved the strays; the letterbox bar is computed on logic from the view size, the grass reads
    the wind copied into its `FrameData`).
    - `renderIdle()` waits under the hand-off mutex until the render thread is outside its draw hook and no slot is
      filled; the wait checks the stop flag every 100 ms (rule 8's notify wakes it) and never gives up otherwise. Its
      time is `lt.gate` on the stages line (`threadsplit::gateWait`). It is free in off and lockstep and on any thread
      but logic's.
    - It is also the escape hatch for a rare logic-side write to what the draw reads, and every such write calls it
      first: `exec_cmd` (a script or console op), `sim_opened`, `begin_session_load`, the buildings' `setData`, the
      creator's frames (`update_ui`), the lab's open (`animationsCommand`). A frame that is not a plain world frame (a
      session load, the title, a session waiting for its camera target, the creator, the open animation lab) gates at
      its start (`logic_paths`), so those run as lockstep does.
    - Logic's take-wait (rule 6): bounded at 100 ms; when it runs out with the slot still filled, logic sets it free and
      runs its next frame into it (the skip), so the sim keeps stepping at ~10 Hz behind a render thread that does not
      draw (`renderstall`). Lockstep never skips.
    - Dev checks: `stall MS gate` / `stall MS b` sleep just before / after the gate (with `stall MS` in part A, a quit
      can be timed in each phase), `renderstall MS` holds the render thread.

**Additions to the hook order** *[R6 "missing from the list", R3.2]*. HOOKS.md is the lead's inventory
(kenshi-k16.2.205.12.4). It is not reachable from the ds account, so the order is given here for the lead to merge
into it. Items already filed carry their id:
- before the rest of 7: the sokol thread-owner assert (kenshi-k16.2.205.12.19); logic out of `render_frame`'s tail and
  the window snapshot to the render side (kenshi-k16.2.205.12.23); the readback and framestats fixes
  (kenshi-k16.2.205.12.24); **then the thread in lockstep (hook 9, moved forward; kenshi-k16.2.205.12.25)**;
- 7 continues: the resource job queue (kenshi-k16.2.205.12.22), the untraced upload sites
  (kenshi-k16.2.205.12.21), 7b foliage (kenshi-k16.2.205.12.18), `chars.prepare` to the logic half
  (kenshi-k16.2.205.12.26), then the renderers the order lacked: shadow (cascade maths and the cache signature inputs
  in the packet; its slot comes when the `shadow.cpp` freeze lifts), blood decals, weather fog map / sky parameter
  block, portraits, arrows, world map, light volumes, labels and overlays (the last join hook 8's list);
- alongside 7: `render_frame` in an `app`-free TU (kenshi-k16.2.205.12.27): `client/src/frame/renderframe.cpp`, which cannot
  see `App`; the blocks that still read `app.` sat in main.cpp behind `RenderState::Legacy` and left it one renderer
  per sitting (what has moved and what is left: the top of `renderframe.hpp`; sitting 2, kenshi-k16.2.205.12.45: the
  sky pass and the fog volumes, from the packet's sky block `FramePacket::sky`, a `SkyFrame` that `Sky::frame()` takes
  at the end of `logic_frame`; sitting 3, kenshi-k16.2.205.12.47: the exposure and the post chain up to the swapchain
  pass, with the luminance range, heat haze and clock in the sky block and the options in `FramePacket::post`; sitting 4,
  kenshi-k16.2.205.12.48: the buildings' and the terrain's opaque draw and the frame's lit `LightParams` (the legacy scene
  gets it as an argument), from `FramePacket::scene` (bldSet, pos, terrain load budget) and `RenderState`'s buildings,
  terrain and creator-scene references; sitting 5, kenshi-k16.2.205.12.49: the foliage's and the grass's opaque draw (the
  fade band stays legacy, after the water; foliageOn, grassOn and the grass clock join `FramePacket::scene`), and the
  building lights packed by `buildings.update` into `WorldBuildings::FrameData` (count + 16 x 4 vec4, 1028 bytes a frame;
  the draw side no longer reads `lights_`; sitting 6, kenshi-k16.2.205.12.50: the water and the foliage fade band, with
  `waterOn`, `scripted` and `rain` in `FramePacket::scene` and the `Water` reference in `RenderState`; the legacy scene is
  split in two, `worldCharacters` before them and `worldScene` after, because the water's depth copy must include the
  characters, items, squads and arrows; sitting 7, kenshi-k16.2.205.12.51: the characters', items', squad pillars' and
  detection arrows' draw, between the grass and the water, from `RenderState`'s chars, squads and arrows references:
  the arrows' marks (`stealth_arrow_marks`, from the sim view) are logic's, in `FramePacket::arrows` at the end of
  `logic_frame`, and worldSetup's `Arrows::update` writes them into the instance buffer (the render's upload); the squads'
  dots and overlay switch are logic's (`setLive`, the toggle), so the draw takes `FramePacket::scene.squads`
  (`Squads::drawCount()`) and reads neither; the characters' poses are logic's (`chars.update`), and so are their
  palettes, records and instance lists since kenshi-k16.2.205.12.26: `chars.prepare` runs last in `logic_frame` on the
  world path into `FramePacket::chars` (`Characters::FrameData`: values only, each instanced draw's mesh buffers and
  views resolved at build time, no pointer into a character; cleared on the other paths), worldSetup's `chars.upload`
  writes it into the frame's GPU buffers, and the draw reads only those (a character whose build is not drawable, or
  whose body is still not ready, has no entry: never drawn half-built). The portraits too since kenshi-k16.2.205.12.67:
  `Portraits::pose` (logic, beside `prepare_chars`) poses each packet portrait job's copy into a local and gathers it
  (`Characters::portraitFrame`: the camera and a `FrameData`) into `FramePacket::portraitJobs`, with the frame's set
  budget decided there; `Portraits::update` only uploads and draws. `headHeight` poses into a local too (no member the
  render reads), and the animation lab's `portraitCopy` gathers in `Lab::frame` into `LabFrame::frame`;
  sitting 8, kenshi-k16.2.205.12.53: the glass, the cursor id pass, the AO, the sun shafts and the dust (`worldScene` is
  gone from Legacy), from `RenderState`'s idbuf, shadow, sunShafts, dust and lightVolumes references and
  `FramePacket::scene` (mouse, sunShafts, dustMotes, sunShaftsDebug; the clock is `animTime`). The id pass's pixel is
  `scene.mouse`, logic's copy of the pointer at the end of `logic_frame` (same value as the old live read, which no event
  could change between the two halves on one thread; no race on two). Its readback goes through `IdBuffer`'s pixel buffer
  ring to `framestats::publishCursor`, and logic takes it with the stats copy at the next frame's start (`cursorHit`):
  one frame late, as before; sitting 9, kenshi-k16.2.205.12.54: worldSetup (the shadow pass, the portraits, the reflection, the
  light volumes, the interior mask, the scene pass opened) is `world_setup` in renderframe.cpp, same place in the order;
  its inputs are `FramePacket::scene` (the existing bldSet, pos, foliageOn, grassOn, animTime, waterOn, scripted,
  terrainLoads, sunShafts, dustMotes; new: simActive = `app.sim.active()`, interiorLight, sunBounce, interiorMask, ground
  = the colour map under the camera) and `FramePacket::worldMap`; `RenderState` gets `refl`, `portraits`, `lightVols` (the
  vector App keeps for the dev ops' context) and the render-owned `reflSkyOnly`; `app.shadowMs` is `framestats` `shadowMs`
  (the HUD reads the logic copy). The id pass scales the pointer by `scene.windowW/H`, the window of the same frame as
  `scene.mouse`; sitting 10, kenshi-k16.2.205.12.55: `world_overlay` and the GPULOG line are in renderframe.cpp (the UI replay, load
  screen, cursor at `scene.mouse`, sgl checks, `framestats::fillDraw`), its conditions in `FramePacket::overlay`; `RenderState` gets
  `loadScreen`, `cursors`, `trails` and the render-owned `sglShown`; sitting 11, kenshi-k16.2.205.12.57: the debug views (nav overlay,
  light probes, trails), the sim markers (with the labels when there is no UI) and the minimap / full map / HUD run in logic at the end of
  `logic_frame` (`record_overlay`, under the old conditions) and record through `ovl::` (`client/src/render/overlaylist.hpp`: the sgl / debugtext
  call stream, verbatim, bound per thread; unbound calls go to sokol at once, so the early paths' `draw_text` / `draw_panel` are unchanged)
  into `FramePacket::overlayLists`; `world_overlay` plays them at the same place (`ovl::play`; the trails' list into their own context,
  `Trails::submit`). The 3D marks are projected while recording with the packet's camera (`views[0]` at near 1, far 1000, main.cpp
  `overlay_camera`, as `build_mvp(1, 1000)` gave the old draws): the lists hold pixels and carry no camera; a stereo frame records one set per
  view. The map's texture is a `dl::TexId` bound at `WorldMap::init`; the drawstats counts are ops read at play. The HUD reads logic's stats
  copy (`framestats::stats()`), so its GPU ms are one frame older than the old render-side read; sitting 12, kenshi-k16.2.205.12.60: the early
  paths (blank / await, loading, title) are in renderframe.cpp, reading `FramePacket::early` (the mouse, the session load flag, the load
  progress and step name, filled at the end of `logic_frame` by `record_early`); the mesh viewer's (kView) is `dev::viewDraw` in
  `dev/viewmesh.cpp`, compiled only with `KCLIENT_DEV_TOOLS` (render hook 9, kenshi-k16.2.205.12.65): `dev::viewFrame` records the camera
  position and the report panel and text (an `ovl` list) into the early block's dev-only `viewPos` / `viewText`, and the draw reads the packet
  and the viewer's own `MeshView`, never `dev::Context`. `RenderState::Legacy` is gone, not left empty: nothing in the draw half reads `app.` now);
  `Post` and the adaptation flag are `RenderState` members). The frame count, the CPU ms (the
  packet's logic ms + the draw's ms) and the fog count are FrameStats fields; and packet views as `Camera views[N]` +
  count (kenshi-k16.2.205.12.30);
- 8: the UI command list, with glyph pages, icons and textures as list entries (kenshi-k16.2.205.12.28). The host half
  is `client/src/render/drawlist.hpp` (the logic-side recorder: per sgl group the vertices, scissor, pipeline id, layer and a
  UI-side `dl::TexId`; uploads create / dirty rect / destroy ahead of the draws, a destroy taking effect at the next
  packet) and `drawlist_gpu.hpp` (render side: the id table, the uploads, the replay into sgl). The cursor's shape is
  in the packet (`FramePacket::cursor`) and it is drawn render side at the newest mouse position. The sgl error flags and
  the replay counters are in `FrameStats::sgl`. The ui/ half (gfx records, font pages and icons as ids) is the UI
  worker's;
- before overlap is enabled: the ThreadSanitizer build and run (kenshi-k16.2.205.12.29) and this protocol's
  implementation in overlap (rules 2, 4-6 and 11).

**Open points (for the lead to file):**
- *Settled (kenshi-k16.2.205.12.25):* lockstep's render-side timeout. On the Intel card under gamescope, a frame callback
  with no pass swaps a back buffer nobody drew: the window alternates between two stale buffers (the frame before last and
  an older one, RMSE 0.10 apart, 0.18 from the last frame in a moving camera), so it flickers. Each drawn frame is now
  copied render side and blitted back on a frame without a packet (`lastframe`); with it, every no-pass frame equals the
  last frame (RMSE 0). Captures: `findings/kenshi-k16.2.205.12.25/stall/` (the `stall 1200` op, with and without the blit).
- *Settled (kenshi-k16.2.205.12.25):* logic's poll of the link without a world step is `SimView::pumpLink()` (`pump()`:
  the socket into the decoder, HELLO, the 5 s ping; the render tick moves only in `SimView::update`). In isolation
  (`renderstall 6500` in a live bar_lure session): 64 polls, the render tick stayed 83156.01, the newest decoded snapshot
  went 83160 -> 83286.
- The applied width and height in `FramePacket` (rule 10) landed with kenshi-k16.2.205.12.23; the logic ms / render ms
  split of `pk.cpu0` (rule 11) with kenshi-k16.2.205.12.25 (`app.logicMs`, `app.renderMs`, the HUD's cpu line).
- *Timed (kenshi-k16.2.205.12.25):* quit during a session load in lockstep. Scripted quits at frames 405, 410, 420 of
  `tests/creatorload` (inside the load) and the 20 resize-burst runs: the join took 0-3 ms (logic was between steps). A
  quit with a 1.7 s logic step right behind it (the longest session-load step measured; `405 quit`, `407 stall 1700`,
  a script quit takes effect a frame late): the join waited 1410 ms for the step and the quit was done 2.19 s after the
  request. The window is gone only after the step; nothing hangs. A step that must end sooner checks
  `logicthread::stopping()` (none does yet).
- The exact bounds (render ~1 refresh capped at 20 ms; logic a fixed 100 ms) are starting values. Verify
  them in overlap with a hidden or occluded window on the user's compositor.

### 7.2 The resource job queue (kenshi-k16.2.205.12.22)

Review 2.2 *[R2.2]* and §7.1 rule 2: a packet is a pure snapshot, so every create, free and one-shot GPU job travels
beside it, in order, and is never dropped. `client/src/render/renderqueue.hpp` (namespace `rq`) is that queue. It is also the
base of the streaming design (kenshi-k16.2.258: per-frame budget, cheapest first, never wait).

1. **Logic never calls `sg_make_*`, `sg_alloc_*`, `sg_init_*` or `sg_destroy_*`.** It owns the CPU data (decoded
   pixels, vertices, a decal list, glyph pages) and its own pending state (a "requested" set, a "queued" flag). It calls
   `rq::push(tag, job, lane, bytes)`. The job is a closure that holds the data by value and the renderer by pointer
   (renderers live as long as the app). The render thread runs the job, makes the resource and publishes it. It
   publishes into render-side members that the draw reads (most users), or into an `rq::Handle<T>` that logic polls with
   `ready()`.
2. **Two lanes, each in submit order.** A resource's create and its destroy go in the same lane. The lanes are not
   ordered against each other.
   - *frame*: every job queued while packet N was filled runs before packet N is drawn, all of them, whatever they
     cost. The packet may name what they make. Use it for creates the same frame draws, destroys, and one-shot paints.
   - *stream*: under a per-frame byte budget (`rq::setStreamBudget`, 8 MiB) and a time budget (`rq::setStreamTimeBudget`,
     `render.stream_ms`, 4 ms; kenshi-k16.2.205.12.74): a job runs only if the lane's time so far plus its predicted cost
     (its bytes at the measured create rate, a running mean) fits; at least one job per frame even if it is larger.
     A streamed DDS texture over twice `render.stream_slice_mb` (4 MiB) is several jobs (`dds_queue_image`): the first
     makes an unsealed immutable image, each writes a strip of block rows or a few whole small levels
     (`sg_write_image_unsealed`), the last seals it and publishes. So no single create holds a frame for long: measured,
     a create costs about its bytes (a 4096² BC3 texture, 22 MB, took 3-10 ms in one job). What it makes reaches logic
     through a `Handle`, and logic names it in a packet only once `ready()`. Characters and buildings use it (below), and
     258's priorities sort it. The frame lane is not split (it runs everything before its packet's draw anyway).
   Both lanes run a job only on the draw of the packet it was queued for, or a later one (its serial; the stream lane is gated
   like the frame lane, kenshi-k16.2.205.12.35). So a redraw of packet N (§7.1 rule 4) never runs a destroy that logic queued
   while filling N+1, and a resource that N names is never freed before N's last draw.
3. **Serials.** `logic_frame` starts with `pk.serial = rq::beginPacket()`. Every job queued until the next call carries
   that serial, and a job queued before the first call carries 0. `render_frame` starts with `rq::drain(pk.serial)`,
   before the path switch, so the blank, loading, viewer and title paths drain too. It runs the frame lane up to that
   serial, then the stream lane. A redraw of the same serial (rule 4) finds no frame job left (rule 5). In overlap, the
   jobs logic queues for N+1 while N is drawn wait for N+1's draw. So a free that logic queues for N+1 never pulls a
   resource out from under packet N.
4. **Shutdown.** `cleanup()` runs `rq::flushAll()` while every renderer is still up, and again after the shutdowns
   (their queued destroys), before `sg_shutdown`. Under the thread this comes after the join (rule 8). Shutdown
   functions that only run in `cleanup()` still destroy directly: that is the render thread, after the join.
5. **Behaviour on one thread.** With `render.thread` off, `frame()` is logic, then render, in one callback. The jobs
   queued by `logic_frame` run at the top of the same frame's `render_frame`, before any pass. That is the same frame
   and the same order of GPU work as the calls they replace, so scene_test is unchanged (46/46, rmse 0, on 8086:e223).
6. **The forced assert.** `--sg-owner-logic` (dev builds; `dev/sgowner.cpp`) runs the thread-owner assert on one
   thread. Inside `logic_frame` (`dev::SgLogicScope`) sokol has no owner, so any traced `sg_*` call aborts, as it will
   on the logic thread. A `dev::SgAllowScope("why")` hands ownership back for a caller that is legal there: the load
   steps (rule 7) and the callers not converted yet, each scope naming its follow-up. `--sg-owner-logic=log` prints
   each distinct stack once and does not abort, a survey of the logic-side callers. The list of allow scopes in
   `main.cpp` is the to-do list. When it is empty, the logic half is clean.

**Converted (the first users):**
- Blood decals: `BloodDecals::update` chooses the slots on the logic side and copies the decals that touch them.
  `paint()` makes the atlas (on first use) and repaints, then publishes the terrain's patch uniforms (`patches()` is
  render side).
- The load screen: `init`, `relayout` (`Font::bake` is CPU only) and `shutdown` read the files and bake the glyphs on
  the logic side. Jobs make, swap and free the images, views and samplers. `draw()` reads only `gpu_`. One change at
  the edge: init now takes the first painting that decodes. The old code also asked sokol whether it accepted the
  image and tried the next one if not.
- Grass (hook 7a's deltas): the pipeline (first ready frame), each tile's buffer and the frees are jobs.
  `Grass::FrameData` lost `uploads`, `frees` and `serial`, and `Grass::upload` is gone.
- Particles (hook 7a's deltas): the first `prepare` that sees a technique texture decodes it (`loadTexture`) and
  queues its create (`makeTexture`). `upload()` only looks the names up.
- The weather fog map: `setAtlas` / `setLive` → `buildFogMap` blurs on the logic side and queues `makeFogMap`
  (the image swap and the shader binding). This covers the trace's new `setAtlas` caller (kenshi-k16.2.205.12.21).

- Characters (kenshi-k16.2.205.12.34): `add`, `replace`, `addWeapon`, `body`, `morphed` and the stump / armour / hair meshes
  build their vertices on the logic side and decode their textures there (or take a prefetch worker's decode). `upload` →
  `queueBuffers` and `texture` → `makeView` queue stream-lane jobs (bytes = the vertex + index data, or the image data)
  that make the buffers or the image + view and publish them into an `rq::Handle` per mesh (`GpuMesh::gpu`) and per
  texture (`Characters::Tex`, cached per file in `views_`). The morph cache's eviction is a stream-lane destroy behind the
  create. The 1×1 fallbacks (`nothing`, `flat`, `white`) are made in `init`, a load step, and published at once. The draw
  reads `get()`. 205.13's character upload budget is not on client yet, so the lane keeps its defaults (8 MiB and 4 ms a frame, 4 MiB texture slices);
  `sim.char_add_budget_ms` (the CPU side of `add`) is unchanged.

  **What is drawn before a body is ready.** A character is put in the draw list (`buildSet`, which
  kenshi-k16.2.205.12.26 moves to the logic half) only when `drawable()`: its body mesh and every mesh and texture it
  binds are `ready()`. There are no placeholder bodies and no fallback textures: a character never shows at a lower
  fidelity, it appears whole, so pop-in comes before lost fidelity. A *rebuild* (`replace`, `fall`, `riseLive`: the
  creator's sliders, a weapon drawn, a KO) whose new GPU objects are still queued keeps drawing the build it replaced
  (`Char::was`, frozen in its last pose) until the new one is drawable; `update()` then drops it. So a rebuild never
  blinks out. A portrait waits for a drawable character (`portraitPose` is false, Portraits asks again). On one thread
  (and in lockstep) the jobs run at the top of the same frame's `render_frame`, before `prepare`, so within the budget a
  new character is drawn in the frame that added it, as before.

- Buildings (kenshi-k16.2.205.12.35): `WorldBuildings::update` → `ensurePart` / `drain` / `evict`, and Foliage's `prepare`, which
  reaches the same cache. `drain` takes a finished decode on the logic side, sets the mesh's CPU fields there (subs, bounds, the
  interior hull) and queues the create: `uploadMesh` (the vertex and index buffers) and `uploadTexture` (`dds_make_image` + its view)
  publish into an `rq::Handle` per mesh (`MeshEntry::gpu`) and per texture (`TexEntry::gpu`), bytes = the data size. A streamed entry
  is `Uploading` until `promote()` (at `drain`'s start) sees its Handle ready, and only then `Ready`, the state every draw list and the
  pack test: logic names a mesh or texture in a draw only once it is ready. A **blocking** update (scripted runs, the scene test)
  queues the same jobs in the frame lane instead and marks the entry `Ready` at once: the frame lane runs before the packet being
  filled is drawn, so what the blocking update waited for is drawn in the same frame, as before (an image sokol refuses is drawn
  grey, `texView`, and `promote()` marks it `Failed` on the next drain). `debug.stream_scripted` makes a scripted run stream as a live
  one does (the walk check of kenshi-k16.2.205.12.35). An **evict** queues the destroy in the lane of the entry's create
  (`MeshEntry::lane`), capturing the old Handle, and gives the entry a fresh one; the lanes' serial gate (rule 2) keeps the destroy
  behind every draw of the packets that named the entry, and the create runs first (same lane, in submit order).
  The **instance write** is a packet upload: `update` fills `FramePacket::buildings` (`WorldBuildings::FrameData::inst`, cleared when
  update does not run), and `upload()` at the top of `render_frame` writes it into the write-transient buffer on every draw of the
  packet (a transient buffer must be written in each frame that draws it). Still read from `app` on the render side: the groups and
  the mesh and texture states (`render`, `renderShadow`, `renderMask`, `renderGlass`, `renderIds`), legal in lockstep; they move to
  the packet with `render_frame`'s split (kenshi-k16.2.205.12.27).
- kui (render hook 8; the ui/ half kenshi-k16.2.205.12.38, the host half kenshi-k16.2.205.12.39): `record_ui`, at the end of
  `logic_frame`, records `FramePacket::ui` (`ui::record`) and `FramePacket::labels` (the world's name and item tags, the
  selection box and the debug portrait strip, from the packet's camera) when the draw that replays them would have drawn
  them. The uploads of both (font pages, icons) are one frame-lane job per packet (`dl::gpu::apply`); the draw replays
  the lists where `ui::draw` and the label batch ran. Host textures carry ids reserved at creation and bound there (the
  portrait atlas at `Portraits::init`, the title art in `init_menus`, the animation lab's target); the prospecting map
  is made and bound by a frame-lane job of the packet that painted it. `ui::draw` and `gfx::begin(w, h)` are gone.

**Not converted yet.** Each has an `SgAllowScope` in `main.cpp`. Two surveys found these
and no others: the 46 scenes, and `tests/creatorload` (title → new game → session load screen → creator) under
`--sg-owner-logic=log`.
- `sim_opened()`: the session's GPU setup when the sim arrives (`Cursors::init` and the rest of that function). It runs
  on the logic side when the title's local server comes up after loading.

## 8. Cost: starting now vs after the playable loop

Plainly, for the user:

- **Do P0 now either way.** The shadow cache needs no architecture change, is the biggest GPU saving the report found,
  and is the difference between "60 is safe" and "60 is safe with room" on both cards. It is independent of the timing
  question below.
- **The render thread is not needed for 60 fps.** On the AMD card Heft is already inside 16.7 ms and Heng is close; P0
  plus the in-flight 205.11 tunables get Heng there (REPORT §5: 12.0 ms with all four removals, 83 fps). The render
  thread buys 144, and it is the foundation for multi-record Vulkan and for XR's context ownership — not a 60 fps fix.
- **What starting now costs.** P1+P2 are a large, mostly mechanical rework of `main.cpp:6015-6857` (the frame), the
  upload sites, every `sgl` consumer (kui's `gfx.cpp` — the UI session's library), the dev tools' readbacks, and kui's
  event ownership. Estimate **3-6 weeks of one worker** (P1 ~1-2, P2 ~2-4), during which:
  - `frame()`'s tail is churn: every merge that touches a draw pass, a uniform, an overlay draw or an upload conflicts
    and must be rebased; the client lead's merge cadence slows in the render path.
  - feature work on the render path should pause or land first: new passes (interiors' masks/light volumes), new shader
    uniforms, new HUD/overlay draws, the UI's window work, the window gate's resize handling. Logic-side work (sim,
    AI, UI state, doors' data) is unaffected.
  - the playable-loop features in flight — doors, interiors, UI, mine-and-sell — are mostly logic/UI and keep moving;
    the ones that would conflict are the UI's draw code and any new render pass.
- **What starting later costs.** The same 3-6 weeks, later, plus rebasing whatever render-path feature work landed
  meanwhile, and it re-touches the same code a second time if the features were written against today's "call `sg_*`
  anywhere in `frame()`" model. 144 fps, the Vulkan multi-record path and XR wait for it.
- **What would have to be redone on the new path.** Anything that draws or uploads from the logic thread: the UI gfx
  backend, the dev-tool readbacks, the load-step uploads, and every new pass written in the old shape. Shaders, world
  data, sim, camera logic and the scene test's scenes do not change.
- **The decision to put to the user.** (1) Start P1/P2 now, pausing render-path features for 3-6 weeks, or (2) land
  P0 (and 205.11) now, ship the playable loop, and start P1/P2 after it — accepting that 144 fps waits and the rework
  then lands on a larger `frame()`. My default if it must be taken without an answer: **P0 now, P1/P2 after the
  playable loop** (the 60 fps target and the user's playtesting do not need the thread; the design above does not expire).

## Lead decisions (2026-11-14, client lead, on review)

- **XR scope: `docs/VR.md` governs** (stereoscopic play, quad layers, no head tracking, no VR frame rate). The "90 fps
  stereo, pose prediction" in this item's brief was the lead's wording, not an agreed goal; head-tracked VR stays out of
  scope unless the user decides otherwise. Section 5's late-pose latch is kept only as the path that (a) leaves open.
- **(a) as written: sokol_app's loop thread is the render thread**, the game moves to a logic thread; no sokol patch for
  the split. A manual-present API is not pursued.
- **P0 (the shadow cache) goes ahead now**, independent of the timing decision (kenshi-k16.2.205.12.2). Its baseline run is
  the current split at the user's cameras (the last open question below).
- The timing of P1/P2 is the user's (ask kenshi-k16.2.205.12.3). The UI split's shape is agreed with the UI worker before P1.
- **Timing, user 2026-11-15 (ask kenshi-k16.2.205.12.3): (b) now, on its own pool.** "How about we create a new agent
  type/pool? render1 (render2, but not used). It must work on it's own branch, and keep rebasing until it reaches feature
  parity. For every rebase conflict, it must find a generic hook point that the main code can share to stop rebase churn."
  So: the render worker (pool `render`, ROLES "Worker pools") keeps the long-lived `ds/render` branch rebased on
  `client`; each rebase conflict becomes a small behaviour-neutral hook point (`ds/render-hook-<n>`) that the lead merges
  into `client` first; the thread lands early behind `render.thread` (off), and the default flips at parity on the lead's
  and the user's word. Render-path features on `client` are not paused. Jobs: children of kenshi-k16.2.205.12.

## Open questions

- **XR scope.** This item's brief (90 fps stereo, pose prediction) contradicts `docs/VR.md` ("stereoscopic play, not a
  VR game; no VR frame rate"; quad layers, no head tracking). Which is the target? The design keeps both possible, but
  only the head-tracked one needs the late-pose latch or reprojection. Verify: read VR.md with the coordinator; if
  head-tracked is wanted, VR.md's scope needs the user's decision first.
- **sapp loop inversion vs a sokol patch.** (a) as written needs no sokol change because sokol_app's loop thread *is* the
  render thread. If the client lead prefers keeping the game on the sapp thread and rendering elsewhere, a
  manual-present/external-context API is an `up/` patch instead — more sokol surface for the same result. Verify: ask
  the lead which shape it wants before P2.
- **The UI split's ownership.** Moving kui's `gfx` backend to a CPU command list (or gating UI state behind the packet)
  is a change to the UI session's library; it is the biggest single coordination cost in P2. Verify: agree the shape with
  the UI session/lead before P1.
- **Shadow cache keys.** Whether `camLz` must be texel-snapped for the cache (and its sub-texel visual delta), and the
  full set of invalidations for animated building parts (doors, wind-aimed parts, limbs) and streaming. Verify: measure
  with a cache-on/off moving-camera script.
- **Current split at the user's cameras.** REPORT §2 predates 205.8; 205.8's own numbers are on a different camera. The
  estimates here are arithmetic on the report's stages. Verify: one `KCLIENT_STAGES=0 KCLIENT_GPULOG=1` run on the
  current base at Heft (cage_prisoner, the user's camera) and Heng, fresh server per Heng run (k16.5.12's fault).
- **Vulkan blockers' effort** (k16.2.170, k16.2.37) and whether sokol's Vulkan per-draw cost (uniform buffer per draw)
  actually beats GL after 205.8's instancing. Verify: k16.2.246's Heft measurement.
