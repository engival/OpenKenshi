# Client renderer: the "enhanced" profile

Design notes for taking the client past the original's look. Not scheduled; a parking lot with the steps worked out
far enough that any one can be picked up as a slice. Owner: client coordinator. Companion docs: `CLIENT.md`
(renderer structure, "Open work"), `reference/LIGHTING.md` (the original's pipeline, which the faithful path ports).

Status (2026-11-07): 3.1 built; 3.4 is the next slice. 3.9-3.14 are gaps found after the first revision.

Direction (2026-11-07, retired 2026-11-10): move to a linear working space, world shaders output linear radiance and
exposure moves to the composite — rejected: the scene holds the game's display-referred HDR (k16.2.82). 2026-11-10
(k16.2.82): the display at the end is the original's hard clip (`EXPOSURE_KEY` 0.55, no output encode) and it is the one
base; `--vanilla`, the AgX / Khronos Neutral curves, the fitted contrast and the preset rows are retired
(`client/retired/`). One base; a preset is layers on top of it, none today. The faithful target is the in-game reference
screenshots within tolerance.

## 1. Where the renderer stands

The world path is a faithful port of the game's 2013 deferred lighting (`client/shaders/lighting.inc`): Lambert sun
with the GGX sun specular, the Lazarov environment BRDF, a four-cascade PCF shadow atlas, SkyX-style atmosphere, HDR
auto exposure, SMAA 1x, bloom, clarity, CAS, vignette. The equations are fine. What dates the image: most *inputs* to
the ambient half are static data Lo-Fi baked once (3.1 makes the irradiance dynamic when on):

- irradiance: six face colours of `mp_irradiance.dds`, the same at noon, dusk and in a dust storm;
- environment specular: the static `mp_specularity.dds` cube;
- `ambientmap.png` sampled at the camera;
- gloss: the diffuse alpha, near zero on most assets, so stone, iron, cloth and wood shade identically;
- metalness: never loaded for world objects (armour reads `cmap.b`; the game has no maps for the rest).

Observed in the Hub at 10:08 (2026-11-05; world particles, weather instances, heat haze, AO/indirect and the sky
ambient have landed since): shadow sides flat grey-brown (the dynamic ambient is a level/tint shift, 3.1); props
floating (the AO pass is too small to see); terrain tiling and repeated debris sprites; hard seam where buildings
meet the ground; the sky rolling off to a flat orange. Dust is now drawn in the air; its placement is partial.

Budget is not the constraint: 2.8 ms GPU for that frame at 1080p, so over 10 ms free at 60 fps. Policy is: the
standing direction is "match the game's look", and the steps below change it.

## 2. The switch

The post track (`CLIENT_GOALS.md` "Post effects") has the mechanism: `post.*` keys, each its own option, all off by
default over the original's display. The preset rows (original / subtle / enhanced / cinematic / stylized) and
`--vanilla` are retired (k16.2.82); a future preset is a set of these keys layered on the base, none today. The steps
below are new `post.*` keys (or `render.*` where the
effect is in the world pass, not the chain). No profile switch. Each key can be A/B'd alone against the original
with the headless capture (`kenshi-client-agent/docs/TESTING.md`). `post.sky_ambient` and `post.moonlight` are
world-lighting inputs, not chain stages; moonlight is a key only (it does nothing while sky ambient is off).

Each step is still a key, and off means off, but the byte-identical rule is retired with the linear pipeline: the
display transform is part of the pipeline, and the faithful comparison is the reference screenshots. Where a step
changes a function, keep one implementation with the off branch, not a copy.

No pop-in rule: an enhanced effect that needs a draw-range cut is rejected.

## 3. Steps, in payoff order

**Faithful debt first.** Some gaps are unfinished ports of the original, not enhancements: the water reflection and
rain ripples, per-pixel `ambientmap.png`, the night stars, weather effect types 7/10. Fix those under the faithful
rules (no key, match the game); the side-by-side demo runs the display base (no post effects), so they are the only steps that change it.

Each: what, where it plugs in, rough GPU cost at 1080p, how to verify. Costs are estimates to be measured with
`gputimer`.

### 3.1 Dynamic ambient (sky SH + ground bounce)

Done as `post.sky_ambient` (0..1 blend over the cube; `Sky::ambientSH`, `lighting.inc light_irradiance`): 256-sample
Rayleigh sky (no Mie) x the weather's sky mult, plus the colour map's mip-4 albedo under the camera lit by sun + sky;
white-balanced so noon's mean equals the cube's; off is pixel-identical (the display base). A/B (2026-11-07): it mostly
moves level and tint (Hub +8/+6/+5 RGB, dusk −5/−11/−10), and shade stays warm, not blue — the directional effect
needs 3.2's bent normal. Open: per-pixel `ambientmap.png` (the game samples it per pixel, ours at the camera), a
bounce-level key instead of today's renormalise, and the red pre-dawn dust hue.

### 3.2 Ambient occlusion and contact shadows

- The current `post.ao` (post slice 4: scalable ambient obscurance, 12 spiral taps, half res, reprojected one
  frame) and its `post.indirect` bounce (slice 10) are the right hooks but too small to see at the Hub's scale.
  Replace the estimator with GTAO: horizon-based, 4 slices x 8 steps, radius 1.5 m world, bent normal as a third
  output, multi-bounce from the albedo; keep the blur, the reprojection and the indirect output as they are.
  Proper temporal accumulation comes with 3.7.
- Screen-space contact shadows for the sun: 16-step depth ray towards `sun_dir`, 0.5 m, multiplied into `shadow`
  only (the game shadows the sun term only; keep that).
- Bent normal feeds 3.1's SH lookup, which is how the ambient becomes "shadowed" under overhangs without a second
  shadow pass.
- Cost: 0.6 to 0.9 ms. Verify: tents, benches and barrels sit on the ground; the ladder's rungs shadow the wall.

### 3.3 Volumetric dust and fog

- Froxel fog: 160x90x64 volume, camera aligned, exponential depth slices. Per froxel: height fog density + weather
  density (`fog_rng.w`, `surf.y` dust) + wind-scrolled 3D noise; in-scatter from the sun (Henyey-Greenstein, g 0.6
  for dust) with the cascade shadow sampled per froxel (light shafts through the gate arches); ambient in-scatter from
  3.1's SH. Integrate front to back, apply in the world shaders in place of the current distance fog when enhanced.
- The current fog colour and ranges remain the far-field term so the horizon matches the original.
- Cost: 0.8 to 1.2 ms. Verify: sand stream 100 % is visible as haze with a sun disc dimmed through it; clear weather
  looks like the faithful path at distance.

### 3.4 Tone mapping and grading

**Retired 2026-11-10 (k16.2.82).** The user made the original's display the one base (INTENT 2026-11-10): the
`post.tone` curves (AgX, Khronos PBR Neutral + sRGB encode), `post.contrast` and the preset rows are in
`client/retired/`, not built. What remains of this step: `post.shoulder`, an individual option off by default (the
brightest channel rolls toward 1.0, hue kept) layered over the clip. A look curve is now an added layer on the base,
not a switch; the exposure calibration and the LUT stay open.
- Linear-pipeline follow-ups (master `50e87b62`, `94505be8`, `155e0a92`): the weather particles decoded their
  display-authored texture and ParticleUniverse colours to linear, and the character tints (hair, vest, blood, armour
  color1/2, hair mesh) decoded at the uniform upload. **Reversed 2026-11-10 (k16.2.80)**: the scene target holds the
  game's display-referred values again, so those decodes were colour errors (Fable 5.2 review 1+2); particle records,
  materials and textures and the character tints are used raw as the game does, and `srgb.hpp` is retired. The audit
  of the rest: fog volumes, fog/light/sky colours and the water sun spec carry no decode; minimap and floaters never
  enter the scene buffer.
- **Decided 2026-11-10 (k16.2.82): no albedo decode — rejected.** The game never decodes, and the original's
  display-referred output is the one base (user/lead 2026-11-10). Everything the world path multiplies into albedo was
  authored for that pipeline: the legacy DXT textures, the mesh COLOURING vertex colour, the grass colour maps, the
  terrain slope-rock stand-ins. Decoding them would move the image away from its only reference, and decoding only part
  of it (a linear tint over a display-authored texture) is the worst case. The pieces stay archived as history: three
  unmerged commits, `c6fbbfd8` (albedo textures, tag `archive/render-linear-2`), `649a650b` (mesh vertex colour) and
  `f7631e72` (grass colour map), the last two on tag `archive/render-tints` (branches deleted 2026-11-09). The evidence
  of what they would have done, kept as the record: the Hub with each on and off against the original's reference shot,
  `kenshi-inspect/findings/render_albedo_choice_hub_overview.png` — the original's buildings and bar roof are pale; the
  vertex-colour decode darkens them, the full decode darkens them further. Grass has no reference shot, but its colour
  maps are saturated: the pale straw turns deep green (`findings/render_tints_grass_canyon.png`). Exposure: the decoded
  set drops the Hub's scene luminance 0.80 -> 0.18; the adaptation floors at MIN_LUMINANCE (`exposureMin` 0.8), so auto
  exposure sticks at 0.69 and the image reads ~3.4x dark. Nothing here merges.
- A 32^3 grade LUT per time-of-day band (night, dawn, day, dusk) and per weather, blended; stored as our own PNG
  strips (not the game's). Authoring: start from identity, adjust by eye against captures of the original.
- Cost: negligible. Verify: the sky keeps hue at the sun instead of going flat orange; shadows gain contrast without
  crushing.

### 3.5 Terrain

- Stochastic (hex) tiling in `terrain.glsl` for the detail layers: 3 taps with triangle-grid blending, kills the
  visible repeat at the cost of 3x the detail fetches. Distant macro variation: a low-frequency noise tint from the
  colour map so the far ground is not a flat wash.
- Building base blending: in `mesh.glsl`, for fragments within 0.5 m above the terrain height (sample the heightmap),
  blend the surface albedo and normal towards the terrain's splat. Buildings grow out of the sand instead of sitting
  on it.
- Decal layer: a deferred decal pass (projected boxes) for wear paths, rubble aprons, blood is already one
  (`blooddecals.cpp`; generalise). Path decals come from the sim's traffic, not hand placement: ask the sim for a
  per-nav-tile traffic count when the time comes (don't approximate client-side).
- Cost: 0.3 to 0.5 ms. Verify: screenshot at the Hub's gate from 50 m: no repeating debris, no hard base seam.

### 3.6 Specular and shadows

- Environment specular from a per-frame prefiltered cube of our sky (64 px, 6 mips, GGX convolution over ~4 frames,
  one face per frame) in place of `mp_specularity.dds` (whose static cube feeds `light_spec` today). Same
  `light_spec` BRDF.
- PCSS in `shadow.inc`: blocker search (8 taps) then penumbra-scaled PCF, so the tower's shadow is sharp at the base
  and soft at the tip. Cost: shadow 1.6 -> ~2.4 ms. Only on cascades 0 and 1.
- Point lights get shadows only where it matters (bar interiors, one cube per light, static, baked on building load);
  flagged, not default.

### 3.7 Temporal AA and upscaling

- FSR 2 (MIT) as the TAA and render-scale path; SMAA 1x stays the faithful AA. Needs motion vectors (characters:
  previous-frame bone matrices; foliage: wind offset), a jittered projection and reactive masks for particles and
  the occluder fade cone.
- Also what 3.2 and 3.3 reuse for temporal denoise. Do this before shipping those two at full quality.
- Cost: 0.8 ms at native 1080p. Verify: the headless capture diff against the faithful path shows no ghosting on a
  running squad.

### 3.8 Motion and density

- Vertex wind on tarps, shrubs, grass and cloth tents (a per-material "sways" flag from 4.1 below; amplitude from
  the weather wind). Dust puffs kicked by walkers (`particles.cpp`, a sim event we already receive: footsteps).
- Grass density where the game has none: our own distribution from the biome map, drawn with the existing `grass`
  pass; fades, never pops.
- Character close-ups: 3.12.

### 3.9 Water and reflections

The planar reflection is in (kenshi-k16.2.53, `reflection.hpp`): the game's 512² pass about y = 100 (oblique near
plane), holding the unlit albedo luma of terrain, buildings, foliage, grass and characters and the sky's own colour;
`water.glsl` multiplies its environment specular by the red channel, and samples the water's own cube
`mp_specuwatery.dds` (the old `kReflection = 0.2` had stood in for both). ~1.2 ms GPU (`gpu_reflection_ms`). Gap: it
reuses the main view's culling, so things only the reflection would see are missing. Still to do: rain ripples
(`rainMap`); no screen-space reflection anywhere in the client.

- Done: planar reflection of the near field (above).
- Rain ripples: the weather's `rain` drives three scrolling layers on the water normal (`water.hlsl`), and
  depth-buffer splashes on land (`CLIENT_GOALS` W5).
- Cost: planar pass 0.4-0.8 ms at 512²; ripples < 0.05 ms. Verify: greenlands_bridge (the bridge reflected), a
  Heavy_Rain scene at 100 %.
- Faithful first: the planar reflection is the original's own feature (no key); SSR is the enhanced extra.

### 3.10 Sky, clouds and night

The visible sky is the atmosphere dome plus a scrolling 2D cloud layer with a guessed tile scale (`sky.cpp` `0.16`);
clouds cast no shadows. Stars are about 7x short (`LIGHTING.md` §15). `post.moonlight` (0.5) exists but does nothing
while `post.sky_ambient` is off (the default).

- Cloud shadows: one low-res cloud-density sample along the sun ray, multiplied into the sun term; moving cloud
  shade is the cheapest way to make a clear-weather scene alive. Cost < 0.1 ms.
- A parallax or volumetric cloud deck in place of the flat layer (0.5-1.0 ms); sunset clouds keep their colour.
- Night: stars at the reference density, a moon disc/glow, and moonlight as a layer in a future preset.
- Verify: hub_overview at 12:41, cloud density 0.7 (shadows move); night_moons (star count).
- Cloud shadows and the volumetric deck depart from the original; both are opt-in.

### 3.11 Weather surface response

Rain is particles + wetness; nothing responds: no ripples on water, no splashes or ground marks, no lightning
(effect type 7) or globalpoint (type 10), both TODO in `weatherinstances.cpp`.

- Water ripples (3.9), depth-buffer splashes, a lightning flash (a sky/ambient pulse with the thunder cue), and the
  dust coat already coded but disabled in `mesh.glsl`.
- Cross-reference `CLIENT_GOALS.md` "World effects plan" W1-W5; do not duplicate it here.
- Verify: a Heavy_Rain run with ripples, splashes and a flash at night; a dust storm with the coat on.

### 3.12 Characters

3.8 defers close-ups, but characters are the subject of the first use. The character BRDF is the world one: no skin
SSS, hair anisotropy or cloth translucency (armour already reads `cmap.b` metalness).

- Skin: a cheap wrap-diffuse + red bleed (or Burley) for hands and faces; hair: anisotropic specular; cloth: a sheen
  term. Per-squad cost 0.1-0.3 ms.
- Verify: characters_hub at 10:00, close-up of skin/hair/armour; the same shot with post effects off.
- The demo runs the display base; this is for the user's play unless a preset later turns it on.

### 3.13 Interiors and local light

Point lights have no bounce, and the ambient is the global sky SH (the interior mask only clips particles), so a bar
at night reads as outdoor shade plus lamps.

- An irradiance volume or DDGI over building interiors and towns, baked on building load; a cheap per-interior
  ambient probe is the fallback.
- Verify: the 4.3 bar-at-night capture: walls near a lamp lit, corners not black.
- Cost: 0.1-0.3 ms runtime, build time on load.

### 3.14 Foliage and grass shading

Leaves and grass use the up normal and gloss 0, as the game does; 3.8 only gives them wind. Real normals, backlit
translucency and a thin waxy sheen are cheap and visible on every tree and grass scene.

- Cost < 0.1 ms. Verify: grass_far / petal_hills: leaves shade by their own normal, backlit edges glow.

## 4. AI-assisted asset enhancement

The game's textures and meshes are Lo-Fi's and read-only. Nothing derived from them (an upscaled texture, a
roughness map decomposed from a diffuse) may enter a repo or a release (`LICENSING.md`: assets stay on the player's
install). The split that keeps this clean: **ship the tool and the labels, never the pixels.** Derived maps are
generated on the player's machine from the player's own install, into the client cache, on first run of the
enhanced profile (same shape as the existing DDS and mesh caches). Three loops, cheapest first.

### 4.1 Material classification (labels manifest)

The gap: no per-material roughness, height or sway flag for world materials (armour already reads `cmap.b`
metalness). One label per material, not per pixel.

- An agent loop (kinspect or a client sub-agent, off-peak, `memory: deepseek-peak-hours`) walks every `.material`
  and its diffuse, views the texture (a 256 px contact sheet, 16 per image) and assigns one class from a fixed set:
  `stone, plaster, sand, metal_iron, metal_bright, wood, cloth, leather, skin, hair, glass, foliage, bone, water`,
  plus flags `sways`, `translucent`. The output is `client/data/material_classes.json`: material name -> class,
  keyed also by the diffuse texture's content hash so renamed mods still match. Labels are facts about the texture,
  our own data, shippable.
- Per class, a parameter row (roughness range, F0, height scale, sway amplitude) in the same file; the shader reads
  the class row and modulates with the existing gloss alpha, so the game's own variation inside a class survives.
- Verification is sampled, not trusted: every class gets a contact sheet of 64 random members for the user to eyeball;
  a misfiled class is a bug like any other. Unlabelled material = faithful shading (no silent default to a class).
- Cost to build: an evening of model time; the labels are stable across game versions because of the content hash.

### 4.2 Derived maps on first run (local tooling)

Generated into the cache from the player's own textures, never shipped:

- Height from the normal map by Poisson integration (deterministic, no model) for parallax occlusion on stone and
  wood classes. Covers the "low-poly silhouette" complaint on walls partially; nothing else does without new art.
- Roughness from diffuse where the class says it varies (stone, wood, leather): a small open-licensed
  intrinsic-decomposition model (candidates to evaluate: DeepBump for normals from diffuse, GPL-3; StableNormal and
  the Material Palette line, check each license before use), run through ONNX Runtime (MIT) as an optional
  dependency. If the dependency is absent the class-constant roughness from 4.1 applies; the enhanced profile never
  needs the model to work.
- Upscaling (Real-ESRGAN, BSD-3) at 2x for the diffuse and normal of classes the camera gets close to (characters,
  walls). Texture packs do this today; the difference is that ours is generated locally. VRAM doubles for those
  classes; keep it a separate key.
- Meshes: AI mesh upres is not mature enough to touch; silhouettes stay as shipped. Tessellation with the derived
  height is the realistic ceiling.

### 4.3 Parameter tuning loop (renderer side)

The headless capture already exists; the loop is: pick a camera and time (the Hub gate at 10:08, dusk at Squin,
night in a bar), capture the original (kinspect's game-side screenshots in `findings/`) and ours, let a vision model
score the pair on a fixed rubric (exposure match, horizon hue, shadow depth, haze) and propose one parameter change
(grade LUT control point, fog density, SH gain). Human-accepted changes only; the model proposes, the capture diff
decides, the user picks. Applies to 3.1, 3.3 and 3.4, which are the taste-driven steps. Not a training loop: a
search over our own config values, each run logged with the capture pair.

## 5. What this does not do

- Match the original byte-for-byte. The target is the reference screenshots within tolerance; the base (the original's
  display, no post effects) is the one path, and presets layer on it.
- Introduce proprietary SDKs: FSR 2 (MIT), ONNX Runtime (MIT), Real-ESRGAN (BSD) are the only candidates named;
  anything else goes through `LICENSING.md` first.
- Ship derived art. Labels and parameters yes; pixels no.
- Approximate sim state for effects (traffic paths, footsteps, weather): the wire has it or we ask.

## 6. Order

Done: 3.1. Next, cheap and every frame: 3.4. Faithful debt for the demo: 3.9 water reflection + rain ripples,
per-pixel `ambientmap.png`, 3.10 stars, 3.11 effect types 7/10. Enhanced after that: 3.2 + 3.7 together (temporal
reuse; 3.2 also makes 3.1 visible), 3.3, 3.10 cloud shadows, 3.9 SSR. 4.1 before 3.5 / 3.6 / 3.8 / 3.12 / 3.14
(all read the labels). 4.2 and 4.3 opportunistic.

## 7. Deadlands reimagining (2026-11-14): what the images map to

Two Deadlands Heavy_Rain night shots were run through the local image models (Qwen-Image-2.1, FLUX.3 Klein, Fibo 2.0) as a "2026 game" restyle. They are idea boards, not targets (user: "mostly for ideas, not something that's realistic to achieve 1:1"). Images: kenshi-inspect/findings/deadlands_reimagining/. Most of what the models added is already in this doc:

| What the image models added | Step |
|---|---|
| crevices dark, domes and tanks sit on the ground | 3.2 GTAO + contact shadows + bent normal into 3.1; 3.5 base blending |
| ruined tower dissolving into layered haze (the biggest single change in qwen_domes) | 3.3 froxel fog |
| wet rock reflecting the green-brown storm sky | 3.6 per-frame env cube. makeWet (lighting.inc light_wet / wet_gloss) already raises gloss, but the specular still samples the static mp_specularity.dds, so wet surfaces reflect a sky that isn't the storm |
| puddle sheen / reflections | 3.9 SSR |
| green-yellow storm grade | 3.4 LUT, a Deadlands / Heavy_Rain band |
| Fibo's crisp rust and cracked basalt | 4.1 classes (rust roughness) + 4.2 local ESRGAN; 3.5 stochastic tiling |
| a glowing break in the clouds | 3.10 cloud deck |
| ground splashes | 3.11 / CLIENT_GOALS W5 |

Not covered above, and these are what date the Deadlands most:

**A. Lit rain.** The game's particle shader gives rain a constant 0.32-grey additive streak with no sun colour, no sky light and no fog (CLIENT_GOALS "Particles", basic.hlsl). So rain looks the same in front of the tower as in front of a fire, and the same at 5 m as at 100 m. Enhanced key: light the streaks from the sky SH (3.1) and from `light_points`, so rain glows around fires; apply fog and a depth fade; add a far-rain layer (scrolling curtains beyond the 120-unit emitter box) that also feeds 3.3's density. Needs 3.7 to keep the thin streaks stable. (kenshi-jj3.21)

**B. 3.3 is written for dust in daylight, not rain at night.** Add (1) in-scatter from point lights into the froxels, which is what makes doorway fires glow through rain in qwen_domes, and (2) rain intensity as a density source. Heavy_Rain deadlands has fog off, so today the only haze is the stormDark extension. (kenshi-jj3.22)

**C. A weathering coat, built from the dust coat.** The most visible thing Qwen did to the domes: acid and grime streaks running down vertical faces, and dirt collecting in crevices. `dust_coat()` in mesh.glsl already has the parts (Turbulent.dds noise, the per-instance biome via v_oxz, the slope response). A rain variant stretches the noise along −y on vertical faces, darkens by AO / cavity, and takes a per-biome tint (yellow-green in the Deadlands). No pixels shipped, so it is LICENSING-clean. (kenshi-jj3.23)

**D. Outdoor fire shadows.** 3.6 limits point-light shadows to interiors. Fires lighting wet rock with shadows are much of the night look. A small nearest-N shadowed-light budget near the camera would be enough. (kenshi-jj3.24)

**E. Smooth silhouettes on curved meshes.** Section 4.2 says silhouettes stay as shipped, but the domes, tanks and the black tower are meant to be round. A load-time Phong / PN subdivision of meshes that 4.1 labels "curved", into the mesh cache, would smooth them without new art. sokol has no tessellation stage (the vendored `sokol_gfx.h` has vertex, fragment and compute only), so it is a load-time step, not GPU tessellation. This also changes the faithful silhouette, so it is its own key, off by default. (kenshi-jj3.25)

Deadlands payoff order: 3.3 with B -> 3.2 -> A -> 3.6 cube -> C -> 3.4 Deadlands LUT -> 4.2.
