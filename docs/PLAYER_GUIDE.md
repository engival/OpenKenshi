# Player's guide (kclient)

This is the player's side of the Kenshi runtime rewrite: what you see and press in the game today, where to find it,
and what it does. It is a work in progress against the original game; an entry says **not built yet** where the
original has something we do not. Keys are the defaults (rebind them in Options, section 2.5). Names in `code` are keys.

Developer detail (flags, scripts, scenes, rendering) is in [`client/README.md`](../client/README.md). The launcher
`client/tools/play.sh` is the only way the guide asks you to start the client.

## 1. Launching

`client/tools/play.sh` is the launcher. It takes one mode:

| Mode | What you get |
|---|---|
| `play.sh` | The list of modes, one line each. `play.sh --help` is the same. |
| `play.sh fly [--at PLACE\|x,z]` | The free camera over a place, no title screen; the Hub by default. `--at` takes a town name or `x,z`. |
| `play.sh title` | The title screen. New Game starts a game; the client starts its own game server behind the title. |
| `play.sh game` | A new game at the Hub with a game server behind it (the Wanderer start). |
| `play.sh scenario [NAME]` | A scenario fixture from `scenarios/`; no NAME lists them. |
| `play.sh spectate [NAME]` | A player-less spectator fixture (the `spectate_*` files); no NAME lists them. |

Arguments after `--` go to the client (`play.sh fly --at Hub -- --alt 200`). Builds are not run by the launcher: if a
program is missing it prints the command that builds it. `title` runs the plain client (no `--debug`, no direct server
join); `fly`, `game`, `scenario` and `spectate` pass `--debug`, so the development windows and keys in section 6 work
there.

**The Kenshi folder.** The client finds your Kenshi install by itself (Steam and GOG default places). The first time it cannot, it asks:
a folder picker titled "Where is Kenshi installed?" opens before the game window (on Linux it needs `zenity`, `kdialog` or `qarma`
installed; without one, or with Cancel, it prints where it looked and exits). Pick the Kenshi install folder or its `data` folder; a
folder without `gamedata.base` is refused and the picker reopens. Your choice is saved as `data=PATH` in `~/.config/kenshi-client/data.conf`
(`%APPDATA%\OpenKenshi\kenshi-client\` on Windows), so the next start does not ask. `--data DIR`, `KENSHI_DATA` and editing that file still work and
win over the saved choice; delete the line to be asked again.

**Windows: log and console.** No console window opens. The log is `%LOCALAPPDATA%\OpenKenshi\data\logs\openkenshi.log` (the run before it: `openkenshi.log.1`); attach it to a bug report. To watch it live, start with `--console` (or set `KCLIENT_CONSOLE=1`): from cmd it prints there, otherwise a console window opens.

**Window position.** The client remembers where its window was, its size and whether it was fullscreen, and puts it
back at the next start. Each desktop layout (the set of monitors, their names, sizes and places) has its own entry in
`~/.config/kclient/kclient.cfg` (`window.<layout>.rect`, `.monitor`, `.fullscreen`), so a laptop on its own screen and
docked remember separately. It is saved when the client quits, and used only if the window's centre is still on a
connected monitor. Switch it off with `window.remember=off` in that file (or `--set window.remember=off` for one
run). Nothing is read or saved when `--size`, `--script`, `--screenshot` or a scene test is given. X11 only: under a
native Wayland session the position is best effort (the compositor may ignore the move), the size still restores.
Every start's restore path and every exit's save are also appended to `windowgeom.log` beside `kclient.cfg` (last 200 lines kept), to read after a
window came up small.

**Options and the config file.** Every client setting is a `key=value` line in the same `kclient.cfg` (the options window writes it too).
A setting left at its built-in default is not saved, so after an update it follows whatever the new default is; set a setting away from
its default to pin it. `kclient --print-defaults` lists every setting with its built-in default and help line as config-file text (nothing is
started and no game data is read), a template or reference for the file. Only the built-in defaults appear: detected values such as the
monitor or the remembered window layout live in their own `window.<layout>.*` lines and are never treated as defaults.

## 2. The title screen and menus

### 2.1 Title screen

The title art fills the screen. Bottom right: `OpenKenshi <version> - not affiliated with or endorsed by Lo-Fi Games. Requires a purchased copy of Kenshi (Steam or GOG).` (`dev` when the build has no version). Down the left third:

- `CONTINUE` — loads the newest save. Greyed when there is no save yet.
- `NEW GAME` — opens Choose Your Beginning (2.2).
- `LOAD GAME` — opens the load panel (2.4).
- `IMPORT GAME` — **not built yet** (says so).
- `OPTIONS` — the options window (2.5).
- `CREDITS` — shows the credits box: this project, the libraries and their licences, the reverse-engineering references.
- `SCENARIOS` — the playtest scenarios (2.6). Only in development builds, and only with `--debug` or `--playtest` (`play.sh title --playtest`, for a journey playtest).
- `EXIT` — quits.

Before the game server is up, a panel beside the buttons says GAME SERVER STARTING with what it is doing (with the seconds so far),
a bar of its steps and three dots that keep moving while it works; if the server failed, the panel says GAME SERVER ERROR in red with
why and what to run. CONTINUE, NEW GAME, LOAD GAME and IMPORT GAME show the same text as their tooltip. They still take a click: the one
you pressed stays held down, its caption lit, the panel says e.g. "NEW GAME will start when the server is ready", and it goes ahead once
the server answers. Pressing another changes your mind.

### 2.2 New game

`CHOOSE YOUR BEGINNING` is a panel in the middle of the screen:

- On the left, a list of the starts, Wanderer first. Click one, or use Up / Down, to choose it. The list is the
  game's own starts (its `NEW_GAME_STARTOFF` records, sent by the game server).
- On the right, the chosen start's title, its description, then its squad, money and starting place.
- `ADVANCED OPTIONS` — opens the advanced options window below the panel (see below).
- `BEGIN` — starts that start (the creator opens first when it has editable squad members, 2.3). Enter does the same.
- `CANCEL` (or Escape) closes the panel.

**Advanced options** lists nine new-game settings, each a slider (arrows step, the box takes a typed value) or a tick,
with a tip at the bottom describing the focused row: Hunger time, Chance of death, Global damage multiplier,
Production speed, Research speed, Building speed, Number of nests multiplier, Bandits loot the player, Easy prospecting.
The values are kept for the session — **not built yet**: they are not yet carried into the running game.

### 2.3 Character creator

A new game that has editable squad members opens the creator first. The loading screen stands in until the creator's scene (platform and character) is ready to draw; the panels appear with it, never over a black screen. It has three parts:

- **Left column**: name; race, subrace and gender (each cycled with its arrows); `IMPORT` / `EXPORT` / `CLOTHES`;
  `CHARACTER n/N` steps through the squad's editable members; the race's description and stats; `CONFIRM`, and
  `CANCEL` (the new game's editor has no CANCEL: CONFIRM is the way out).
- **Middle**: the character standing on the editor's platform, rendered live; the two are one assembly and move together.
  - **Drag** it with the left button: side to side turns it around (yaw), up and down tilts it (short of tipping over).
    The turn is about the point the view is framed on (see the wheel below), so the part you zoomed onto stays put.
  - **Mouse wheel**: zooms in on the point of the model under the pointer — the hand under the pointer stays under the
    pointer as you scroll — and that point becomes the point the drag turns about. With the pointer off the model the
    zoom goes straight towards (or away from) that point.
  - Zooming all the way back out drifts the view smoothly back to the default framing of the whole character — the
    turn and the tilt you gave it ease back with it, the turn the shorter way round, so the character comes round to
    face you again instead of snapping. Each wheel step glides there over about a quarter of a second; scrolling
    on during a glide carries on from where it is going, and a drag takes over from what is on screen.
- **Right column**: `BODY` / `FACE` / `HAIR` tabs over sliding pages of shape sliders and colour/hair/beard pickers
  (hair and beard have a "none" first entry; a race or sex with none shows the picker disabled). `RANDOMISE ALL`,
  `RANDOMISE` (this page) and `RESET` under the pages.

`CONFIRM` keeps the look and moves on (an empty name is refused). `CANCEL` (when present) leaves the creator.

### 2.4 In-game menu and saves

Escape in the game opens the menu over a dimmed screen: `NEW GAME`, `SAVE GAME`, `LOAD GAME`, `OPTIONS`, `EXIT GAME`,
`RESUME` (plus `SCENARIOS` in development builds, with `--debug` or `--playtest`). The game is paused while the menu is up. NEW GAME and EXIT GAME ask first
(unsaved progress); EXIT GAME then quits the program (it does not return to the title); RESUME or Escape closes it.

The **save/load panel** is the same window in two modes: a `NAME | DATE` list of saves on the left, the selected
save's details on the right, `LOAD` (or `SAVE`) and `CANCEL`. Saving names the save in a box under the list; a picked
row fills it. Saving over a save and deleting one ask first. Loading has a **Reset squad positions** tick: every squad
goes back to its home or start position as the game loads.

### 2.5 Options

`OPTIONS` (title and menu) or `F10` (in game). A resizable window with a side list of tabs, one per section:
**Graphics**, **Post-processing**, **Animation**, **Camera**, **Interface**, **Keys**, **Game**, **Audio**, **Mods**, and **Debug**
(only with `--debug`, under CLIENT). Inside a tab the settings are grouped under headers (Graphics: Detail, Draw
distance, Indoor lighting (experimental), Shadows, ...), so related settings sit together. A setting that only works
with another one says so in a small line under it, by that setting's name ("needs Interior light below 1", with its tab
in brackets when it is in another tab); the line dims while the setting it needs is off. Find still matches the keys. Changes are saved as you make them; a change marked "next start" (mods, some colours) applies
when the client next starts.

- **Interface / Windows** has **Remember** (`window.remember`, on by default): the window's position, size and
  fullscreen state are kept per monitor layout and restored at the next start; untick it to always start at the default size.
- **Keys** (its own tab, after Interface) — every action with its keys. Click a box, then press the key (hold Ctrl, Alt or Shift with it).
  Escape cancels, a right click clears it; red means another action already has that key.
- **Interface / Colours** — the flat style's colours, each with a swatch.
- **Debug / Debug keys** (only with `--debug`) — the client's development keys.

Each row has its value at the right and a reset-to-default button; `DEFAULT` resets the tab's rows, `CLOSE` closes.

**`--set KEY=VALUE`** on the command line gives an option its starting value for this run only (it is not saved, and its
row's tooltip says so); changing it in Options keeps your new value and saves it.

**Find**: the bar across the top finds a setting in any tab without hiding anything. Press `Ctrl+F` while Options is open
(the window or the world has the keyboard, not one of its boxes) and type: it finds every row, across every tab (Keys included), whose name, key
(`render.shadow_soft`) or help has the text, any case, and goes to the best match first: the setting whose name is the
text, then by where it is found, the name before the key before the help, and in each a whole word before the start of
a word before the inside of a word (`sun` finds Sun bounce before a "Sunny ..." name, and a help saying "the sun's"
before the Inset colour's "Sunken"). The tab switches, the row scrolls into view and
is highlighted, and the count reads `1 of 7` (`0 matches` in red when nothing has it). `Enter` or `NEXT` goes to the
next best match, which may be on another tab, `Shift+Enter` or `PREVIOUS` to the one before; both wrap around. Every tab and row
stays where it is, so you always see which section you are in; the highlighted row works as usual, and clicking
another tab keeps the find. `Escape` in the field clears the text and the highlight; a second `Escape` closes the window.

**Sections and groups.** Every option belongs to a section (one tab: Graphics, Post-processing, Animation, Camera, Interface, Game, Audio,
Mods) and a group inside it, so related settings sit together. The same grouping is used by the console, the saved file and
`kclient --print-defaults`. A group's options that depend on another are noted `needs ...`:

| Section | Group | Keys |
|---|---|---|
| Graphics | Detail | `render.normal_maps`, `render.leaf_sway` (on: trees bend and their leaves sway with the wind; off: static trees, as in the original) |
| Graphics | Draw distance | `render.building_far` |
| Graphics | Indoor lighting (experimental) | `render.interior_light`, `render.shadow_soft`, `render.sun_smooth`, `render.sun_bounce`, `render.sun_shafts`, `render.dust_motes`, `render.lamp_beams` |
| Graphics | Interiors | `render.interior_mask`, `render.signs_indoors` |
| Graphics | Performance | `render.thread`, `render.char_instancing`, `gfx.vsync`, `gfx.fps_cap` |
| Graphics | Shadows | `render.shadow_crossfade`, `render.shadow_cache` |
| Post-processing | Ambient light | `post.indirect`, `post.sky_ambient`, `post.ao`, `post.moonlight` |
| Post-processing | Colour | `post.vibrance`, `post.warmth`, `post.posterize`, `post.dither` |
| Post-processing | Effects | `post.bloom`, `post.clarity`, `post.vignette`, `post.letterbox`, `post.dof`, `post.outline` |
| Post-processing | Exposure | `post.shoulder`, `post.black_point`, `post.eye_adapt` |
| Post-processing | Sharpness | `post.aa`, `post.sharpen` |
| Animation | Foot placement (experimental) | `anim.foot_lock`, `anim.foot_terrain` |
| Animation | KO and ragdolls | `anim.fall_blend`, `anim.rise_blend`, `physics.ragdolls`, `physics.ragdoll_cap` |
| Animation | Locomotion | `anim.vel_blend`, `anim.speed_filter`, `anim.stride_warp`, `anim.stop_contact`, `anim.idle_draw`, `anim.inertialize`, `anim.action_root`, `anim.slop_tau` |
| Camera | Building see-through | `camera.occluder_fade`, `camera.peek`, `camera.occluder_ghost` |
| Interface | Colours | `ui.colour.*` |
| Interface | HUD and labels | `ui.floaters`, `ui.progress_bars`, `ui.show_names`, `ui.labels.characters`, `ui.speed_icons`, `ui.speed_unit`, `ui.map_icons_stepped` |
| Interface | Inventory windows | `ui.gear_top`, `ui.cursor_badges`, `ui.place_one`, `ui.auto_split` |
| Interface | Keys | `key.*` |
| Interface | Look | `ui.scale`, `ui.hud_tips`, `ui.style` |
| Interface | Markers | `markers.selection_ring`, `markers.route` |
| Interface | Screens | `ui.title_at_start` |
| Interface | Screenshots | `screenshot.dir` |
| Interface | Sounds | `ui.sounds`, `ui.hover_sounds` |
| Interface | Windows | `ui.windows_movable`, `window.remember` |
| Game | Character loading | `sim.char_defer`, `sim.char_add_budget_ms`, `sim.char_warm`, `render.loading_standin` |
| Game | Combat | `combat.fix_extra_attack_slots`, `combat.retarget_hold_s` |
| Game | Off-screen world | `world.offscreen_medical`, `world.offscreen_fights`, `world.campaign_victory_ai` |
| Game | Orders | `orders.foreign_walk_to` |
| Game | Paths | `client.data_dir` |
| Game | Single player | `session.server`, `session.server_args`, `session.loading_screen` |
| Game | Skills | `xp.no_runaway_skills` |
| Game | Squads and AI | `squad.follow_heading_fix`, `ai.token_speeds` |
| Audio | Volume | `audio.master`, `audio.music`, `audio.environment`, `audio.effects` |
| Mods | Installed mods | `mod.nicemap`, `mod.darkui`, `mod.slopeless`, `mod.bughouse` |
| Keys | Debug keys | `key.debug_*` (with --debug) |
| Debug (F8 > Settings) | Light debug | `render.sun_shafts_debug`, `debug.light_probes` |
| Debug (F8 > Settings) | Logging | `debug.audio_trace`, `debug.simjump`, `debug.nonfinite` |
| Debug (F8 > Settings) | Navmesh | `debug.navmesh`, `debug.navmesh_stair`, `debug.navmesh_pruned`, `debug.navmesh_band`, `debug.collision` |
| Debug (F8 > Settings) | Pick mask | `debug.pick_mask` |
| Debug (F8 > Settings) | Route preview | `debug.route_preview` |
| Debug (F8 > Settings) | Run files | `anim.wire_still_ticks`, `anim.draw_y_off` |
| Debug (F8 > Settings) | Scripted runs | `debug.stream_scripted`, `debug.pick_wait` |
| Debug (F8 > Settings) | Simulation | `sim.drawn_force` |
| Debug (F8 > Settings) | Trails | `debug.trails`, `debug.trails_seconds`, `debug.trails_dump` |
| Debug (F8 > Settings) | UI development | `ui.layout_reload` |
| Debug (F8 > Settings) | kui_demo | `demo.volume`, `demo.gamma`, `demo.subtitles`, `demo.name` |

**Route preview** (`--debug`, `debug.route_preview`, default off; kenshi-k16.2.331.2). With exactly one character selected, the client plans the route a
right click at the cursor's ground point would get, with the sim's own planner (`knav::planRoute`, the call the server makes) on the navmesh tiles the
client holds, and draws it in magenta (the route marker line is white). Nothing is planned or drawn with the key off. Two lines go to the events file
and stderr, loudly, as `WARN`:

- `route_jump`: the cursor's ground point moved under 3 u since the last plan but the route length changed by x2 or more and by more than 20 u (not between two approach-point routes: the nearest reachable point slides with the cursor). It names
  the tick, both cursor points, both routes' snapped start, goal and waypoints, and whether each was found / an approach point / reseated.
- `route_mismatch`: on a click, the preview for that goal is compared with the server's route for the order when it arrives; a length differing by more than 25 %
  and 20 u, or ends more than 5 u apart, logs both routes, the start positions (wire at the click vs the character at arrival), the goal and whether
  both ends' tiles were held. Silent otherwise. The usual cause is the client's mesh (only streamed tiles, one layer per tile, door paint only from
  tile patches) or a start / body input, since the planner is the same.

`render.loading_standin` sets what a character still being built draws as: `silhouette` (the default) a flat black body of its race, `double` a plain neutral
body of its race, `off` nothing until it is ready. When the real look is ready the stand-in's shape morphs into it over 0.25 s, then the real look fades in over 0.2 s.

Dependencies: `render.sun_bounce` needs `render.interior_light` below 1 (it does nothing while it is 1); `post.moonlight` needs `post.sky_ambient`;
`camera.occluder_ghost` needs `camera.occluder_fade`; `post.eye_adapt` only shows indoors with `render.interior_light` below 1. `render.sun_shafts` and
`render.dust_motes` are independent of each other and of `render.interior_light` and `render.sun_bounce`; both show in the cut-away view.
All of Graphics / "Indoor lighting (experimental)" is off by default except
`render.sun_smooth` and `render.interior_light` (0.25, the effect at the strength 0.75 gave before): to try the rest of the look, set
`render.sun_bounce`, `render.sun_shafts` and `render.dust_motes` (for example 1) and `post.eye_adapt` on.

**Console.** `set NAME` with a section, a group or a key prefix lists its options with their current values (and the `needs` note), for
example `set Graphics`, `set indoor` (a group, the first letters of its name are enough) or `set render.`. `set KEY VALUE` sets one.

**The saved file** (`kclient.cfg`) is written grouped: each group under a `# Section / Group` comment, only the settings that differ from the
default, then other builds' keys and window positions at the end. `kclient --print-defaults` prints every option under the same headers.

The floors and roof that the cut-away hides while you are inside a building still cast their sun shadows (always; the old `render.cut_shadows` setting is gone), so the room is as dark as under the roof and sun only comes through the door, hatches and windows. When you orbit the camera in the cut-away view, a wall that turns its back to you does not vanish at once: for a quarter of a second it fades out as see-through glass (clicks pass through it to the room).

`render.signs_indoors` ("Display building signs while indoors", on by default, as the original; options popup) keeps a building's outside signs (shop and bar signs) drawing while the building is cut to a floor; off hides them until the roof view.

`render.interior_mask` (on by default) is the original's "carve the interior out of the landscape": a building's interior is cut out of the terrain and the grass, so a building
sunk in a dune (the slavers' Longhouse) shows its whole floor and rooms instead of sand filling part of them. The weather particles (rain, ash, dust) stay out of
buildings by the same mask. Turn it off to compare.

`render.sun_smooth` (on by default; settings file / `set`) moves the sun's shadows with the sun. The shadow map's light direction used to be held until the sun
had moved 0.25 degrees (a game minute, so a jump just under every two seconds at normal speed, and faster at x2 and x4): watching the sun set behind a
mountain, the shadows jerk in steps, as the original's do. With this on, the map's direction chases the sun and covers such a step over about a second, so the
shadow edges slide instead of jumping. Off restores the original's stepped motion, for comparison.

Experimental, on by default (0.25): `render.interior_light` (0..1, settings file / `set`) is the amount of the original Kenshi lighting that comes through indoors. The ambient light indoors follows
the openings: the sky light that the baked probes of the nearest buildings let reach each spot multiplies the ambient term, so a room is brighter beside its door, windows and hatches and dark in the
corners, and a character standing in it is lit like the wall next to it. 0 keeps none of the original lighting (the effect at full), 1 keeps all of it (the same frame as with the effect off).
Outside every building it changes nothing. The exposure range is still the original's, so the rooms read dark with the effect strong. The first time a building type is near, its light bakes on a worker
thread (a few seconds; cached on disk after).

Experimental, off by default: `render.lamp_occlusion` (settings file / `set`) stops a building's lamps at its walls and floors: the lamp no longer lights the floor above or the room next door through the slab or wall. Each lamp's visibility is baked per probe of the building (one ray per probe, on a worker thread the first time a lamp is near) and multiplies the lamp's light, so the edge is soft (the probes sit 6 units apart); it does not draw the shadow of a chair. A room lit by its own lamp looks the same; the doorway still glows from outside. Needs no other switch; with it off nothing changes.

Experimental, off by default: `render.shadow_soft` (sun half angle in degrees, 0 = off; settings file / `set`) makes the sun shadows near the camera soft the way real ones are: crisp where an object touches the surface it shadows, wider the farther the shadow falls from it (stair shadows on a wall read as a soft shape, not hard bars). The real sun is 0.27; 1.5 to 2 is an artistic, clearly soft look.

Experimental, off by default: `render.sun_shafts` (density scale, 0 = off, 1 = the default look; settings file / `set`) draws sunbeams in the air of the nearest baked buildings (the same bake as `render.interior_light`, which it does not need below 1): a ray march against the sun's shadow map, so a beam runs from each door, window or roof hatch to the patch of floor it lights. Nothing outdoors, in rooms the sun does not enter, or at night.

Experimental, off by default: `post.eye_adapt` lets the auto exposure reach far lower (floor 0.02 instead of the original's 0.8, a dark room compensated only 85%), so with `render.interior_light` below 1 a room lit through its door reads bright inside and the doorway blows out; outdoors by day nothing changes. Eyes adapt at the game's rate, so walking in or out fades over a second or two.

Experimental, off by default: `render.sun_bounce` (0..1, settings file / `set`; does nothing unless `render.interior_light` is below 1) adds the light the sun bounces off the floor and walls to the indoor ambient: the sun patch at the door lights the walls beside it warm, and a wall the sun never reaches stays as `render.interior_light` left it. It follows the sun: the nearest buildings that hold the camera or a player character re-bake as the sun moves (about 5 degrees, in the background on a worker thread), and the new bake fades in over a second. Nothing outdoors, in rooms the sun does not enter, or at night.

Experimental, off by default: `render.dust_motes` (mote count scale, 0 = off, 1 = the default look; settings file / `set`) draws small dust motes drifting in the air of the nearest baked building that show only inside a sunbeam (the same sun shadow lookup as `render.sun_shafts`, which need not be on; no ambient, so a mote outside a beam is invisible). None outdoors or at night.

Experimental, off by default, our own effect (the original has none): `render.lamp_beams` (0..1, 0 = off; settings file / `set`) draws a faint cone of light under each shaded hanging lamp (a spot light pointing down) in the air of the nearest baked buildings, in the lamp's colour, fading with distance and stopped at the lamp's own walls and floors (it switches on the baked lamp visibility of `render.lamp_occlusion` while above 0). At 1 it is clearly fainter than `render.sun_shafts` 1. Works at night and without `render.sun_shafts`.

Also experimental, off by default: `render.open_windows` leaves out the small dark quads a building mesh uses to close its window openings
(the Storm House's four window barriers, the Watchtower's and Shack's too, and the Outpost's dark window panes), so from outside the slits are see-through and the light bake sees the
sky through them (with `debug.light_probes` or `render.interior_light`); the black cavity wall behind the windows is left out with them.

Interface sounds are the original's: a click on any button, the pointer coming onto a title menu button (once
each time it comes on; like the original, no other button clicks on mouse-over), a tab, a slider, windows opening and
closing (map, research, stats, inventory), item moves (picking up, putting in a container, selling, dropping
on the ground), dialogue replies, the floor arrows, prospecting and the build window's buttons. They play in both
styles. Interface, the Sounds row, turns them off; the Hover sounds row picks which buttons click on mouse-over:
original (the default, the title menu's), all, or none (a playtest switch). Orders have the original's sounds too: one click per right-click
order (a move picked from a menu row is silent), the sneak sound when sneaking turns on and a click when it turns off
(the SNEAK button or X), and the level sound when the floor keys change floor. Characters make footsteps (heavier armour sounds different) within about 200
units of the camera, and weapon swings are heard within about 1000 units, with the sound of the weapon they hold; the steps are always the sand sound for now (the ground
under the character is not looked at yet), and animals, mining, smithing and farming are silent (not built yet). Fights are heard where they happen: a landed blow thuds on the victim, a
blocked one rings as a parry, a dodge is silent, a crossbow shot is heard at the shooter, a bolt that hits an armoured part clangs off it, and a
hit character sometimes grunts (an animal snarls, and cries out when it dies). Doors creak open and thud shut where they are, lockpicking clicks, a
body lifted or put down is heard, and stepping into a building muffles the outside.

**Server settings** (Options rows): the sim carries a few settings that correct or extend the original, chosen when the server starts, so a change
takes effect when the next game starts (`docs/COMBAT_OPTIONS.md`; hover a row for its explanation). The rows are extra attack slots, runaway skills,
squad turn-around, target switching delay, fast off-screen travel, living off-screen, off-screen fights and raiders staying after winning; a flag
named in `session.server_args` wins over its row, and a remote server keeps its owner's flags. One affects raids: `world.campaign_victory_ai` (the
server flag `--campaign-victory-ai on|off`), default off. In original Kenshi a bug stops big faction raids
(mostly Holy Nation assaults, also Band of Bones, Southern Hive kidnappers, the Empire and Kral's Chosen) from using the "victory: occupy the
town" orders their data gives them, so they always go home after winning; on, the winners stay and occupy the town until you kill them all (the
town stays yours). Bandit-style raiders (Dust Bandits, Crab Raiders, Rebel Farmers, Black Dragon Ninjas) occupy either way.

### 2.6 Playtest: scenarios and notes

These two windows are the project's playtest record. Both need `--debug`.

A run starts in one of two ways: `PLAY` in Scenarios, or straight from the launcher: `play.sh scenario NAME` and
`play.sh spectate NAME` start NAME's recorded run by themselves (kclient `--play NAME`; the notes dock opens, NOTES is on
the HUD, and the run file is written when you leave). A fixture with no `playtests/NAME/versions.md` plays, but the HUD
says "Not a recorded run" and why.

**Scenarios** (`SCENARIOS` on the title or in the pause menu). Left: the scenarios with their setup, last run and
verdict. Right, for the selected scenario:

- Its title, and under it the feature's bead id (`Bead: kenshi-...`) when it has one.
- `RECIPE`, for a recipe playtest only: the steps to run it by hand (a recipe has no fixture file; its Setup column in
  the list reads `Recipe`, and hovering its row shows the steps).
- `LOOK FOR`: what the scenario is meant to show.
- The line that one run is evidence, not proof: runs are not deterministic, so compare several.
- `RESULTS`, one block per version (newest first): `vN date`, Changed (from the version before) and Answers, then a row
  per run: date, sim build, client build (with `!` when it differs from its version's), seed, one column per metric, the
  verdict and the first note.
- `PLAY` at the bottom starts the selected scenario; the status line says what is happening ("loading ..."). The world
  is swapped in, the menu closes and the run plays (a player-less `spectate_*` scenario is watched in spectator mode,
  3.9); the Scenarios window closes once the run has started (a world scenario: when the server says it started). To
  read the results while playing, open it again from the game menu's `SCENARIOS`. A start that fails (no `versions.md`,
  the server refuses, or it times out) leaves the window open with the reason on its status line.
- A **recipe** (`Recipe` in the list's Setup column, e.g. the journey's New Game / Save / Reload / Load steps) is a game
  flow, not a placed world: there is no scenario file. Selecting one shows its steps in the `RECIPE` block above
  `LOOK FOR`, and `PLAY` starts recording against the game as it runs, loading nothing. Play the recipe, write the
  notes and the verdict, and leave the run (another Play, EXIT GAME or quitting) to store its file. It has no metrics,
  and a NEW_GAME does not end it: the new game is the flow it records.
- A run records the scenario's bead id as `bead:` in its file (none: the line is absent).

**Pinned runs (`--bead ID`).** `play.sh title|scenario|spectate ... --bead ID` pins the run to one playtest: with `scenario NAME` / `spectate NAME`
it is NAME; with `title`, play.sh walks ID and its parents until one is the `bead:` of a playtest. Either way there is no `SCENARIOS` button, and its Scenarios window (title,
`LOOK FOR`) and Notes dock are up from the start. A recipe journey starts at the title screen. For `title`, if no item up the chain is
a playtest's bead, play.sh says so and starts nothing. Notes and F12 shots are posted to ID as comments starting
`[playtest NAME vN run STAMP]` (they wake nobody); the verdict is `verdict (NAME vN run STAMP): good|off|broken` and wakes
the coordinator once.

A scenario comes from the game server's list when one is running, merged with the playtest store; with no server the
store alone is shown and Play starts the run without one. A recipe never loads a world: it always uses the session as
it is.

**Notes** (during a run: `Ctrl+N`, or the `NOTES` button at the screen's foot, left of the shortcuts panel). A small
window docked at the right edge:

- `GOOD` / `OFF` / `BROKEN` marks the run's verdict (the same one in the results table).
- `MARK` stamps the time, the place and the selected character in front of your text, as a note is written.
- The numbered notes, newest kept in view; each shows its follow-up (triage) under it, dimmed. A note that could not be
  added to the run's beads item (`--bead`) says so under it in red, with the reason; a red line above the box says when
  the whole run's notes could not be posted (they stay in the run file).
- The one-line box and `ADD`: Enter adds when it is not blank. While you type, the game pauses.
- Escape in the box keeps the draft and gives the keyboard back to the game; Escape on the window (or its X) hides it.

**Posting the run to its item (`--bead ID`).** A run started with `--bead ID` (kclient; `play.sh scenario NAME --bead ID`,
`play.sh spectate NAME --bead ID`, `play.sh title --bead ID`) also writes itself into that beads item, usually the playtest ask:

- Each note is one comment as it is added: `[<scenario> <vN> run <run file>] note <n> @<MARK stamp>: <text>`, so a crash
  loses nothing from the item and triage can start mid-run. The verdict is its own comment when it is set:
  `[...] verdict: good|off|broken`. Both end with the build line below.
- An F12 shot during the run is copied (the png and its `.txt` / `.trees.txt` / `.events.txt`) to
  `~/git/kenshi/kenshi-inspect/findings/asks/ID/` (never into the repo; `--findings DIR` moves the root), a line
  `<file>: <scenario> <vN>, <game time>, <place>, <selected>` is appended to that folder's `CAPTIONS.txt`, and a comment
  goes on the item with the path, the game time, the place, the selected character and — when one was just added or is
  being typed — the note's number.
- Every `.txt` sidecar of a shot and the first line of every events file carry
  `build: client <git hash> sim <sim hash or ?>`, with or without `--bead`: it says which binaries made a run.
- The run file under `playtests/NAME/vN/` is written exactly as before (the item copy is in addition). It keeps the
  scenario's `bead:` and records the `--bead` item as `playtest: ID`.
- A **warning** (a client message line) means a post did not land: bd is missing or failed, or a shot could not be
  copied. The run file still has the note or verdict; the item does not. The comment is not retried.

### 2.7 Tutorials and the controls list

`F1` (or the `HLP` button) opens the **TUTORIALS** browser: every tutorial on the left, the selected one's pages,
`<<` / `>>` between pages, and `ACTIVATE`. We ship one tutorial, **Controls**, generated from your current key
bindings: a page of the game's keys and a page of the build-mode keys. Its text is ours, never the original's.
ACTIVATE opens the tutorial window (page text; `PREVIOUS` / `CONTINUE`; `DISMISS` to drop it) and leaves a hint row at
the left edge under the window; the row's title reopens it, its red X completes it.

### 2.8 Loading the world

Starting a game or a load shows the loading screen while the world round the camera streams in; the game runs paused
under it and starts when nothing is still loading.

## 3. In the game: the HUD and the world's UI

The HUD is the fixed screen furniture. `F7` hides the whole interface and shows the world alone (the same key brings it
back).

### 3.1 Time and speed

Bottom right, above the orders panel, from left to right:

- The **speed buttons**, four pictures: pause, normal speed, x2, x5 (their tooltips say which). Pause toggles; a speed
  button while paused also unpauses.
- The **money and time** box: `Money:` and your cats on the top line, `Day: N` and the time of day under them.
- The **build** button (the same as `B`, 4.7).

A large `PAUSED` banner shows at the top centre while the game is paused.

### 3.2 Squads

In the world, a left click selects the character under the cursor and a left **drag** (both sides past 4 pixels; below
that the release is the ordinary click) draws a selection box: on release every one of your characters whose position —
their feet — falls inside it is selected and the last one caught keeps the inspect focus, replacing the selection
(Shift adds to it); dead characters are left out, knocked-out ones are boxed, NPCs are never picked, and a character more
than 7500 from the camera is out of reach. A box that catches none of yours leaves the selection and the focus as they
were. No box starts over a window, while dragging an item, or with the camera-drag buttons held; a right press during the
box is ignored (no order, no cursor change) and the box keeps going; it works while paused and while following the
selection. `ui.style=original` draws the game's own box: a thin near-black outline with no fill.

The portrait bar along the bottom centre holds the active squad's characters. A tab over it picks another of your
squads. The bar and its tabs fit between the panels in the bottom corners at any window shape: on a narrow (4:3, 5:4) or
crowded screen the tabs narrow, and portraits that don't fit page behind the < and > arrows at the bar's ends (or roll
the mouse wheel over the bar). A portrait:

- click — selects the character; Shift adds to the selection;
- double-click — focuses the camera on the character (the camera then follows it: see "Following" below);
- the portrait's bar is health (dimmed when KO or dead);
- a mark over the face: bars when the character is caged, a shackle when they wear shackles (open or locked; the bars win over the shackle), teeth when being eaten (not shown yet: the sim sends no such state);
- behind the face: red stripes in combat, yellow when hungry; a red grid pulsing in and out when hurt (engaged in a fight or being eaten);
- a faded face: the character is knocked out, dead, carried, or put in a bed (asleep there by their own routine or put in it); the same marks show in the squads window.
- hovering it opens no tooltip; the name under it turns red while the character is down, and the `F6` control hints name the portrait.

**Following.** The camera follows one of your characters after `Z` (focus), a double click on the character or its portrait, or a double
number key. There is always exactly one follow target, and the floor view and the camera follow it:

- A plain click on another character's portrait moves the follow to that character, and selects only them.
- Shift+click on a portrait adds to the selection, and the follow goes to **whoever of the selection comes first in squad order**: the order in the
  squads window (drag a portrait there to change it; first place is the leader). The same holds wherever the selection grows or changes while
  following (a box select, select all (`` ` ``), the number keys, `Tab`). To keep one character always the focus, drag them to first place. The portrait
  bar itself is not reorderable for this.
- A click on a character in the world selects them (Shift adds) but does not move the follow (a default; the lead may change it).
- The following ends with a camera move: the pan keys, a jump on the minimap or the world map, a go-to. It does not end with the wheel zoom, the rotation
  keys (`Q` / `E`), the tilt keys, or an orbit / look drag.
- Direct drive (`V`, or a double right click on your character) has its own rules and is not changed by any of this.

### 3.3 Orders

The orders panel is the bottom-right corner. Down its left side, the selection's standing orders as ticks (each one's
tooltip says the same):

- `BLOCK`: hold a defensive stance.
- `HOLD`: hold position: the selection does not run to help allies, and answers only an enemy that comes into close range (the sim's range is our own choice, not yet measured against the original). Order 10 `which` 7; the tick shows the sim's state for the shown character. Not the same as a stop order, which makes a character stand still.
- `PASSIVE`: never start a fight, only defend: the selection does not join an ally's fight but still answers its own attacker. Order 10 `which` 6; the tick shows the sim's state.
- `JOBS`: work through the job list; off, only the jobs that always run.
- `RANGED`: fight with a ranged weapon.
- `TAUNT`: draw the enemy's attacks.
- `SNEAK`: move in stealth mode. A sneaking character's name label gets the stealth icon, tinted by how hidden it is,
  and a light-level icon (3.8). A sneaking character that stands still crouches, with a sword in hand too (it stood upright
  there before 2026-11-13). `X` does the same. While a character of yours sneaks, **one arrow floats on the line
  between it and each person whose attention it is drawing** (the original's "red arrows"): **red** points at someone
  who has noticed you, **yellow** at someone who is noticing you (the arrow turns red when the noticing completes),
  **green** at a friendly observer that has been told about you. One arrow per observer, aimed at them, floating
  above head height along the line; strangers and townsfolk draw them too — they are usually not enemies, only
  people whose attention your sneaking caught. The arrows go when the sneak is switched off (or a fight starts).

Enabled today: Block, Hold, Passive, Jobs, Sneak (Ranged and Taunt are shown disabled: **not built yet**).

Right of the ticks, the selected character's **job list**: numbered in priority order, the X removes one, a row dragged
onto another takes its place (onto empty space it goes last). Under it: `RESCUE` (**not built yet**), `MEDIC` (the
Medic job for the selection) and `PROSPECT` (sends a prospecting task; the results window opens when it finishes).

Above the squad bar at the bottom centre: `RUN` (the run tick: the selection runs instead of walking) and `STOP`
(stops the selection).

### 3.4 Status and medical panels

Bottom left, for the selected character (or the one you last clicked to inspect):

- A tag line when there is one, the most important only (the original's first match):
  - **War Leader** / **Leader** — the head of an army / the leader of the character's squad.
  - **Kidnapped** — carried off against their will; law enforcers will try to help.
  - **Diplomatic Status** — an envoy; attacking them angers their whole faction.
  - **`[SLAVE]`**, **`[ESCAPED SLAVE]`**, **`[EX-SLAVE]`** — owned by a faction, escaped and spotted, or
    freed but still recognised. Hover for whose slave it is, the situation (Seen! / Undiscovered), how much
    they look like a slave and how long the status lasts.
  - **`Committing Crime! (N)`** — a crime in progress, N its seconds; hover for the crime's name and whether
    it was only witnessed or the character was caught.
  - **`[ESCAPED PRISONER]`** — broke out of jail; leave town to clear it. Hover for the expiry.
  - **`Bounty: c.N`** — wanted by a faction. The local faction's bounty shown, in the local ink; another
    faction's only when there is no local one, in the faint ink. Hover for one line per faction, the crimes
    (`-Wanted For:`), the expiry (or "Notorious"), and for a caged prisoner the prison sentence last.
  Then the name plate with the faction on its right. The game reports the leader, kidnapped, slave, crime and
  bounty states today; War Leader, Diplomatic Status and `[ESCAPED PRISONER]` stay hidden until the server
  models them.
- `State:`, `Goal:` and `Encumbrance:`, the skill row in use.
- The `^` button slides the extended rows up: ATHLETICS XP, STRENGTH XP, TOUGHNESS XP, KO POINT, RUN SPEED, HUNGER RATE.
- The medical panel: up to nine body parts with their bars and values, the Blood and Hunger rows, the status box
  ("Normal" and the other health states; its tooltip is a health sentence) and the `>` / `<` markers for food being
  digested, bleeding and resting.

Hovering a row shows its explanation in the HUD's tip box when the HUD is set to fixed tips.

### 3.5 Selected building

Selecting one of your buildings replaces the status and medical panels with the building's: name and owner, its state
("No workers", "Output full"), rows (Town, Workers, Efficiency, ...), buttons (`[UPGRADE]`, Dismantle — a button that
asks first says so), ticks (Battery, Power) and its bars (Condition, Power, construction materials).

### 3.6 Camera diamonds

- The **floor diamond** (over the speed buttons' left end) is always live, as in the original: up and down arrows, the
  floor number in the middle (`Floor 0` is the ground, the roof deck is a building's top floor). `R` / `F` do the same.
  With one of your characters in or on a building (a roof deck counts), it cuts that building at the floor you pick:
  the top floor is the roof view with nothing cut, a lower one the inside cut-away that hides the roof and anything
  on it. A building you stand outside of, or another building, is peeked at the cursor (no fade while you are
  in or on the building under the camera). Once you have stepped the level, the peek cuts at that level like the inside
  view: level 0 shows the ground floor, the building's top level or above the roof deck uncut, a level between that
  floor. Before you step it, the peek follows the floor under the cursor. Step the level before a move order: a right click on a building lands on
  the highest floor at or below the level (the roof deck at the roof's level, inside on the ground floor at 0); open
  ground ignores the level. The level starts at the top (roofs are clickable until you lower it).
- With the camera following one of your characters (`V`, or a double click on it), the floor follows the character:
  the view always shows the floor the character is on: through a building's doorway it becomes that building's
  floor (the ground floor at the front door), on a roof deck the top floor, outdoors floor 0. On a staircase it
  stays on the floor the character came from until a switch line, then shows the floor it is heading to: walking up the
  view switches two steps below the top, walking down one step below the top, and turning round on the stairs
  switches it back. Stopping on the stairs shows whichever side of the line the feet are on. Where you click never
  changes the level, only the character's own position does. The character you follow is always drawn.
  `R` / `F` step the level and it stays there until the character's next door, staircase or roof crossing; a double
  click on the character, or `V` / `Z`, re-follows and puts the view back on its floor. With the camera free
  the level is yours alone.
- The **move speed diamond** picks the selection's pace: Walk, Jog, Run, Group (everyone keeps to the slowest member's
  pace, in formation). Its tooltips name the modes.

### 3.7 Shortcuts and messages

The `ShortcutsPanel` at the screen's foot: `INV`, `STA`, `MAP`, `TCH`, `SQD`, `HLP` — the same as I, C, M, T, U, F1
(`STA` is the character stats window, 4.12). The build button sits with them.

Messages the game sends you appear in the message box over the shortcut line (refusals such as "You can't afford
that.", "Discovered <place>", and the hint "Hold down the ALT key to pick the lock." when you right-click a locked
container that is not yours without ALT; one message at a time, about four seconds each); a toast appears at the top centre in a dark rounded bubble for short news (a screenshot saved); toasts go after about four seconds, even while the game is paused, the same news again counts up in its bubble ("(x3)") instead of stacking, and at most three show at once. Entering a new zone shows its name as a
large banner across the upper middle, fading out.

**Control hints** (`F6`, off by default; Options setting `ui.control_hints`): a small dark see-through panel just
above the bottom-right HUD (the speed buttons and money box), lined up with its right edge, lists what the mouse does on the thing under the pointer: one line per input ("Shift+Click   Add to the
selection"), its name on top; it grows upward with more lines. A greyed line is not possible right now and says why after it.
Separate from the tooltips. With the HUD hidden it sits against the right edge, halfway up.

**Text boxes** (names, save names, notes, options):

| Keys | What they do |
|---|---|
| Left / Right, Home / End | move the cursor; Ctrl + Left / Right jumps a word |
| Shift + those keys, or drag with the mouse, or Shift + click | select text |
| Ctrl+A, or a double click | select everything |
| Typing, Backspace, Delete | replace or delete the selection (Ctrl + Backspace: the word before) |
| Ctrl+C / Ctrl+X / Ctrl+V | copy / cut / paste (one line) |
| The keypad | types digits with NumLock on; with NumLock off its arrows, Home / End and Del move and delete |
| Enter (either one) / Escape | keep / undo the edit |

### 3.8 Labels and text in the world

- Player characters show their names over their heads (selected ones in `[brackets]`). The Interface option
  **Show character names** turns the names off; with names off only the icons show. `Sneak` adds the stealth icon,
  tinted by how hidden you are, and a light-level icon.
- Item labels sit over items in the world: holding `Left Alt` shows every label near the cursor (an option can make
  Alt show character names too), otherwise only the one under the pointer shows. A label of an item that is not yours
  carries its tag (`Steal`, grade, ...).
- A limb cut off in a fight (or by a skin peeler) lies where it fell as an item — `Severed Human Left Arm` and the
  like, drawn as the limb itself. It labels under `Left Alt` and a right click picks it up like any other item.
- Speech bubbles appear over speakers' heads with what they say, wrapped and centred, above the world but under every
  window.
- Progress bars float over characters doing a timed thing: lockpicking, unlocking, prospecting, adding materials,
  training, restraining, cutting shackles. A floating text beside them shows damage and short results ("Pick
  success!", "Shackles broken!"), fading as it ages. A stealth KO shows its outcome over the character that tried
  it, in the original's words: `Failed!` when the victim noticed you first (the attempt ends where it stands) or
  the roll failed, `*Thunk*` when the victim goes down. All interface text is drawn at the original's size for your
  screen height. The bar's fill is two-toned (a lighter top band) and runs
  close to the panel's edge, as in the original; its colour in the flat style is `ui.colour.ProgressFill` (Options).
  The bar hangs under the middle of the character's drawn body (a crouch or a lean does not pull it off to one side as you turn the camera).
- How many bars you see is the setting `ui.progress_bars` (Options, Interface tab, the Progress bars row; its help line
  lists the four values): `off` shows none; `original` (the default) shows exactly
  the bars the original game does, for your own characters' tasks (lockpicking, cutting shackles, ...); `squad` also
  shows the work the original never drew a bar for, mining, producing, building, repairing and dismantling,
  processing and first aid; `everyone` adds the NPCs in view.

### 3.9 Spectator mode

`play.sh spectate` joins a player-less fixture and starts its recorded run (2.6). The squad bar, its tabs, the orders panel, money, build and the move
speed diamond are hidden. Instead:

- the **Watching** panel at the top centre names the character being watched and its faction, its goal and task one line
  each, with two buttons: `FOLLOW` puts the camera on the character (it reads `STOP` while following, and stops it) and
  `CAST` opens and closes the cast window; (`PAUSED` moves under this panel)
- the **CAST** window lists every character (Name / Faction / Id, sorted by name; click a header to sort) with a filter
  box. A click watches that character; a double click (or Enter) follows it with the camera. `CAST` opens and closes
  it; it opens on its own when spectating starts.
- The status and medical panels draw the watched character.

### 3.10 Nothing else in the view

A plain run shows the game's own interface, the panels and windows in this section and section 4, and the minimap
(3.11, an addition of ours). The block of developer text at the top left needs `--debug` and the Debug option
`debug.hud_text` (default off; section 6.10).

### 3.11 The minimap

A small square map at the top right, centred on the camera: the world map with your characters as dots, the places you
have discovered as icons (no names) and a wedge showing the way the camera faces. It carries no text. The original game
has no minimap; this is a deliberate addition.

- **Drag** it anywhere (hold the left button on it) to put it where you like. It remembers the place across sessions and
  stays on the screen when the window is resized.
- **The wheel** over it changes the zoom, five levels from about 0.5 km across to 7 km; the default is the second, about
  0.9 km. The level is remembered too.
- **Options → Interface → Minimap** (`ui.minimap`, default on) turns it off. It is hidden with the full map, the title
  screen, the character creator and loading, and `F7` hides it with the rest of the interface.

### 3.12 The status popup

A small panel at the top right (left of the minimap when that shows) that appears only when something is wrong with the
server, and goes when it is fixed. It lists real conditions, one line each; it never shows the lockstep frame repeats
(those are normal and not yours to correct).

- **Server not responding**: the game is running (not paused, speed above 0) but no new tick has come from the server
  for over a second. The line shows the seconds waited; past 10 s it turns red. It goes when ticks resume.
- **Connection to the server lost**: the link closed or failed. There is no reconnect yet: the game stays frozen and you
  leave to the title and start again (a message also names the cause).

## 4. Windows and pages

Any window comes to the front when you left-click anywhere on it (its title bar, body, border or a control), and the click
still does what it does there: one click both raises the window and presses the button, focuses the text box or scrolls
the list. A right click does not raise. A message box or the game menu stays over everything until it closes.

### 4.1 Overview (the map, and the faction/research/crafting/squads pages)

`M` opens the overview window on its MAP page; `G`, `T`, `Y` and `U` open it on FACTION, RESEARCH, CRAFTING and SQUADS.
A key whose page is already showing closes the window; another key opens its own page. The tabs across the top are
`MAP`, `FACTION`, `RESEARCH`, `CRAFTING`, `SQUADS`, `DIALOGUE` and `AI`; the last two show **not built yet** on their
page (the conversation screen in 4.9 is separate).

**MAP**: the game's world map with its markers (towns, your characters and squads). Drag to pan, the wheel to zoom, or
use `O` (centre), `-` and `+` (zoom); a touchpad or a fine-stepped wheel zooms by how far you scroll. The town icons grow
steadily as you zoom in, from half size at the whole-map view to full size near the closest zoom. Options `ui.map_icons_stepped`
brings back the original's way: icons keep their size on screen and grow in two steps (at about 5x and 9x the whole-map view).
A right click on the map sends the selection there as a move order. A new game opens the map on an area overview: the Hub in the middle and Stack
just inside the top edge (a deviation from the original, which starts on the whole world); the zoom and the place you were looking at are
remembered, across closing and reopening the map and through saving and loading a game.

### 4.2 Squads page

`ADD SQUAD` and the faction's used/maximum at the top right; a row per squad: its name (edit it in the box, it is
renamed as you type), members/maximum, `DELETE` and a strip of the members' portraits. Everything is drag and drop: a
portrait onto a strip moves the character there (onto a portrait: to that place; the first place is the leader; Shift
swaps the two); onto `DISMISS` asks to dismiss them; a squad row onto another row reorders the squads. The active squad
is picked on the HUD. Refusals are shown in the original's message boxes (a full squad, the last character, the only
squad, the maximum number of squads).

### 4.3 Faction page

`RELATIONS`: a row per faction, name left and value right (red below zero) with a bar from the middle (left for
negative, right for positive, the tooltip gives the exact value). `FACTION NAME` is your faction's, renamed by
committing the box (Enter or focus loss). `INFORMATION` (contracts and campaigns) is **not built yet**: the game does
not send it.

### 4.4 Research page

`TYPES` (the categories, the picked one gold), `BLUEPRINTS` (the available entries of the picked type, name and time,
kept progress as a bar; red when it cannot start, its tooltip saying why), `QUEUE` (the bench's queue with progress)
and `DESCRIPTION` (the hovered or last-picked entry: level, ETA, costs, requirements, unlocks and text). **Not built
yet**: the game has no research bench, so every blueprint is locked, the queue is empty and queueing says "Research
needs a bench."

### 4.5 Crafting page

`STATIONS`, `ITEMS`, `QUEUE`, `REPEAT QUEUE` and `DESCRIPTION`, with the bench's recipes, the queue and its progress.
**Not built yet**: the game has no crafting benches, so the page is empty.

### 4.6 Inventory and containers (`I`)

`I` opens the grid inventory of the selected character:

- `OPEN BAG` and `LIMBS` open the backpack's contents and the robotics panel (arm and leg slots round the figure).
- The worn slots: WEAPON I and II, BELT, shirt, pants, boots, head, armour; the worn backpack; the main inventory with
  `ARRANGE` (places every item again, biggest first).
- Items move by dragging (left button) between grids, or by clicking: a click lifts an item, the next click places it.
  A right click or Escape puts a lifted item back. A refused move says why ("Can't wear that item.", "No room for that
  item."). Hovering an item shows its tooltip: name, grade and stats, description.
- The mouse bindings of the inventory and trade screen:

  | Mouse | What it does |
  |---|---|
  | Left click, or drag | lifts one unit (the lifted stack shows its count); a second click or a drop places it |
  | Shift + left click, or Ctrl + left click | lifts the whole stack |
  | Right click, with a window open beside | moves one unit to the other window |
  | Shift + right click | moves the whole stack |
  | Ctrl + right click | moves every stack of that item in the grid (ours; the original has no such binding) |
  | Right click, no window open | equips a storable item, or takes a worn piece off |
  | Drag out over the world | drops the lifted units on the ground |
  | Right click, or Escape, on a lifted item | puts it back |
  | Right button held still on an item (0.2 s) | opens the item menu; release on a row to pick it |

- With several windows open (your inventory, a box, a trader, a body), the ringed-dot button in a window's caption,
  beside the pin, marks it as where right clicks send items; click it again to unmark. With none marked and more than
  one place an item could go, a right click opens the item menu instead.
- Windows stack: the newest opens on top, and clicking anywhere on a window brings it to the front. Escape closes the
  front window, then the next one down, passing over pinned windows; with none left (or only pinned ones) Escape opens
  the game menu.
- The item menu lists what can be done with the item, each with its shortcut dimmed on the right; greyed rows cannot be
  picked. Releasing the held button on a greyed row or off the menu leaves it open: click a row, or click elsewhere to
  close it.
- Over an item, a small badge by the pointer says what a right click will do: a coin with a plus (buy), a coin with a
  minus (sell), two arrows (move to the other window); two dots for Shift (the stack), three for Ctrl (all of them).
  Options: `ui.cursor_badges` (on) turns it off.
- `ui.place_one` (off by default, the original's setting): a lifted stack is put down one unit per click, and the rest
  stays lifted until placed or put back.
- The item menu's rows, for each window the item could go to (the window's name is added when there are several):
  - `Sell` (to a trader), `Buy` (from a trader), `Pick up` (from a box or a body) or `Move` (between your own windows):
    one unit (shortcut Right click), `... stack` (Shift + Right click, shown for a stack) and `... all <item>`
    (Ctrl + Right click, shown when the grid holds several stacks of it). `all` sends every stack of that item one
    order after the other and stops at the first refusal, with its message.
  - `Equip` / `Unequip`, and `Drop` (one unit) / `Drop stack`; dropping is also a drag out over the world.
  A shortcut is shown only when the plain click already does that: with several windows open and none marked, the
  right click opens this menu and no row has one.
- Auto-split (`ui.auto_split`, on): a stack held over a spot that holds fewer than it carries (a bag's stack of 6
  into the main grid, where 3 fit a cell) puts one unit there and places the rest one by one wherever there is room,
  all in the one click; if the room runs out the rest stays where it was and the refusal says so. With `ui.place_one`
  on, the drop is one unit per click and this stays out of the way.

- A click is sent as soon as the game server has answered the one before it, however fast you click: the item dims
  until the server takes it, cannot be clicked or dropped onto meanwhile, and a refusal says why.
- A grid taller than its view scrolls: the wheel, the arrows, the thumb, or a click on the track.

**Locked shackles.** Shackles worn locked (a slave's) cannot be taken off: right-click them in the equipment grid (or hold
the right button) for three rows. `Pick Lock N%` rolls against the wearer's lockpicking; `N` is the chance per try, and it
takes tries (the bar over the character reads "Lockpicking (Chance N%)", the roll shows "Pick success!" or "Pick
failed"); a picked pair stays worn but open. `Use Tools` cuts them with a carried tool (a hacksaw), slowly, the bar
reading "Cutting"; they are gone at the end. `Use Strength` breaks the lock outright, only when strength is above three
times the hardness left ("Shackles broken!", worn and open); otherwise the message box says so. The wearer does the work
itself, from its own inventory window.

A left click on a container opens its window alone; a downed character's `Loot` (right click or the action menu) docks
their body's window beside yours. Drag between the grids as usual. A locked box shows its lockpick state; the game
answers.

Windows stay open together: your inventory, any number of boxes, a looted body and a trader's window at once, each
placed beside the first or cascaded from it, each movable. A box or body window closes with its close button, when the
thing is gone, or when the character that opened it gets a new order of its own (a move, an attack); walking away does
not close it, and the pin keeps it through an order. Drag between any two windows: from a box onto the trader's grid
sells the item (it passes through your pack: taken, then sold). The ringed-dot button marks one window as where right
clicks send items; with none marked, a right click goes to the only other window, else from a box to your inventory
and from your inventory to the first box opened.

A trader's container is the **trade window**: the trader's name as its caption and the trader's money over the grid.
Buying and selling are ordinary moves: drag an item to or from the trader's grid, or right-click it (one unit; Shift:
the stack). Prices are in the item's tooltip. Selling a stolen item asks the fence question first ("You have a N%
chance of getting caught selling this stolen item. Fence it anyway?"). Refusals appear in the message box.
Clicks may come as fast as you like: each one is queued, and the item greys out at once until the trader takes it. A
greyed-out item cannot be picked up, clicked or dropped onto meanwhile; its tooltip still shows.

### 4.7 Build window (`B`)

`B` opens the build window: `CATEGORIES`, then `BUILDINGS` (the unlocked ones), the picked building's name between `<`
`>` when it has variants, its picture, cost and stats, and its description. Clicking a list row picks it; the arrows
step a group's variants. **Not built yet**: placing a building (the ghost, `CONFIRM`, `UNDO`, the build-mode keys), so
this window only browses what is unlocked.

### 4.8 A production building's window

Click a production building (mine, well, refinery, generator, battery, rain collector, farm, crafting bench, furnace)
to open its window at the left edge: its name with a close X, `INPUT(S) | OUTPUT` columns, each with its title, the
item's name, its status line (crafting benches), the slot's small grid and a thin progress bar under it. A crafting bench adds the item in progress and a `QUEUE...` button; a furnace adds
`Total Metals` and an `Active OFF/ON` toggle. Items drag in and out of the slots as in the inventory. An input takes
only its ingredient. A mine's output takes back its own ore while there is room; other outputs only give. A drop anywhere
over a slot lands where the item fits; a refused one says why above the squad bar ("Can't put that there.", "No room
for that item.").

### 4.9 Conversations

When someone talks to you, the conversation screen shows the NPC's panel (name, portrait, what they say) above yours
(name, portrait, money) with the numbered replies. `1`-`9` pick a reply; Escape leaves; a "(Leave)" row ends the talk.

### 4.10 Prospecting

`PROSPECT` (in the orders panel) sends a prospecting task for the selected character nearest the camera; when it ends,
the **PROSPECTING RESULTS** window opens: the overview across the top, the resources with their values down the left,
the selected resource's map on the right with the camera marker. Pick another resource to change the map. `PROSPECT`
again hides the window.

### 4.11 Saving and loading

`F5` quick-saves under the name "quicksave" (**Quick save.** in the message box); `Ctrl+F9` loads it. The menu's
`SAVE GAME` and `LOAD GAME` open the save panel (2.4).

### 4.12 Character stats (`C` / `STA`)

`C` — or `STA` in the shortcuts panel — opens the **CHARACTER STATS** window for the selected character, the one the
status panel (3.4) shows, with the character's name as its caption; `C` again closes it. While the window is open it
follows the selection; pin it (the caption's pin) and it stays on the character it was opened on.

- `ATTRIBUTES`: Strength, Toughness, Dexterity and Perception.
- `SKILLS`, in four columns of `-Group-` headed rows: Weapons (Katanas, Sabres, Hackers, Heavy weapons, Blunt,
  Polearms), Combat (Melee attack, Melee defence, Dodge, Martial arts), Ranged (Turrets, Crossbows, Precision
  shooting), Thievery (Stealth, Lockpicking, Thievery, Assassination), Athletics (Athletics, Swimming), Sciences
  (Field medic, Engineer, Robotics, Science), Trades (Weapon smith, Armour smith, Crossbow smith, Labouring, Farming,
  Cooking). The levels are the status panel's; a level moved by a modifier shows green or red.
- `DERIVED STATISTICS` down the right: Blood, Hunger, Carry weight in `kg` (`19 / 101`, the pack without the fixed
  30 kg carry-person weight), Run speed, KO point, Hunger rate and Damage resistance as a percent — the same numbers
  as the status and medical panels.
- Hover or click a row to describe it: its name (and the change) in `DESCRIPTION`. The `Next Level:` bar shows the
  skill's progress towards its next level (the game's figure; whether it is the original's own bar formula is unproven).

**Not built yet**: the per-skill explanations ("Stats Affected", "Ways To Train", the description text) are empty.

## 5. Orders, selection and the camera (the mouse)

- **Left click** a character selects it; Shift adds or removes it. Left clicking a character that is not yours
  inspects it in the status and medical panels instead. Left clicking an item label picks the item up; a container's
  label opens it; a production building's label opens its window.
- **Right click** acts on what is under the pointer: a character is attacked, talked to or used as the cursor says; an
  item is picked up; a door opens or closes; a bed is slept in; a container opens (someone else's shows the red magnifier; a right click on someone else's container, shop counter or locked safe walks you there and says to hold `Left Alt` to steal items or pick the lock; `Alt` + right click does it. Setting `orders.foreign_walk_to` off: the hint only, no walk); the ground is a move order. Shift
  makes attack, follow and first aid into jobs, and queues the rest.
- **While sneaking**, a right click on a hostile that has not seen your character is the knock-out (**red fist**); a
  hostile that has seen it is attacked as usual (the target menu keeps Stealth KO either way).
- **Hold right click for a moment on a target** opens its action menu where the button went down; releasing over a row
  picks it. The rows depend on the target: a character (Follow, Bodyguard, Talk / Attack, Loot, First aid, Pick up /
  Put down), an item (Pick up), a door (Open / Close, Unlock, Pick Lock, Bash), a container (Open, Take all, Pick
  Lock), a production building (Mine, Open).
- **A prisoner locked in a cage**: the cursor over it is the red **padlock** with the chance of picking the lock (green
  `N%`), and a right click orders nothing. The menu opens on the press as two columns side by side: the prisoner's own
  rows on the left and `Prisoner Cage` on the right with `Pick Lock N%`, `Use Tools` and `Use Strength` (only the ones
  the server says your selected character can try). Picking a row sends your selection to the cage.
- **Your own buildings** (anything your faction owns, built or still a site) are drawn where the sim puts them. A site starts as a pale dithered blue ghost and fills in from the ground up as the work progresses; a finished building is solid. The cursor over one is the **build** hammer while it is a site, the spanner when it is damaged, else the plain use cursor.
- **Camera**: right-drag looks around, middle-drag orbits, the wheel zooms (and sets the fly speed of
  the free camera). The keyboard camera keys are in section 7.
  The floating camera tilts up past the horizon, to about 80 degrees, so you can look at the sky: the original stops at 11.5. The
  ground (indoors the floor, walls and ceiling) comes between the focus and the camera as it tilts, so the camera moves in toward the
  focus, and eases back out to your zoom as you tilt down again. It stops short of the followed character rather than going into it:
  once the ground would push it any closer, the camera stays where it is and the view turns up around it, so the sky comes into view
  with the character below the frame. The distance also eases when hills or a wall swing in behind you, instead of jumping.
- **Direct drive**: `V`, or a double right click on one of your own characters, puts the camera behind that character
  at the default close distance, whatever the floating camera's zoom was (leaving and re-entering starts there again).
  `W` / `S` walk forward / back, `A` / `D` steer (stopped, they turn the character in place); with the right button held
  (mouse look) `A` / `D` strafe instead, sideways at half the walk speed with the body keeping its facing, and `W` / `S` + `A` / `D`
  walk the diagonals (the forward walk / run clips play). The mouse looks. Option `drive.facing` (Options, Game: Direct drive; default `camera`): `travel` turns the drawn body to the way it is moving and plays the plain forward walk / run, so strafe, backpedal and the mouse are all camera-relative; where you go on screen is unchanged (only the animation). Any wheel step, `Home` or `End` leaves the drive (the floating camera on the same character, `W` `A` `S` `D` move the camera again) and zooms it that step. The camera never hides the character: a wall or the ground behind it pulls the camera in at once and it eases back
  out slowly; thin props (signs, rails, poles) fade to a see-through dither instead. Leave it with `V`, `Escape`, or a double right click on the character being driven: the camera returns to the
  floating view centred on it. While driving, the floor and move speed diamonds wear a gold rim.
  A single right click keeps its usual order; on your own character alone in the selection that is no order at all, and
  with someone else selected too it is the *follow* order. A double right click never sends the second order.

A menu or a window takes the mouse and the keys while it is up; the game keys are off until it closes.

## 6. Debug and visualisation (`--debug`)

These are development views. They exist only when the client runs with `--debug` (the `fly`, `game`, `scenario` and
`spectate` modes pass it; `title` does not). Their keys are in the Debug keys tab of the Options window.

### 6.1 The F8 debug panel

`F8` opens a movable, resizable `DEBUG` window:

- **Toggles**, each with its key: Navmesh overlay (`Ctrl+F5`), Collision (the building collision shapes and the drawn terrain, `Ctrl+F7`; the navmesh
  overlay's flip partner: the collision shapes in white, doorways yellow, the thin props grey — the barricades that block movement and line of sight,
  which bodies do not stand on since kenshi-k16.2.382.2 — and the drawn terrain a body does stand on (`Heightmap::terrainHeight`, the drawn triangles) as a
  green grid on its 18 u lattice, all fading out 900 u from the focus; a line in the developer text counts them. What a body stands on at a point (a
  drawn floor, a stair, the terrain) is the `Ctrl+F10` height probe (6.5a) or `probe height X Z` from a script. Like the navmesh overlay it has no depth
  test: indoors the terrain grid shows through floors — read the white wires there), Navmesh: pruned polygons (no key; a settings key), Height
  probe (`Ctrl+F10`; the probe mode of 6.5a, the same switch as the key), Trails
  (`Ctrl+F6`), Squad overlay (`Ctrl+F4`), Shadows (`Ctrl+F3`), Walk on the ground (`Ctrl+G`), Follow camera
  (`Ctrl+C`, with a running game), Free camera (`Ctrl+F`; with Options open it goes to
  Options' Find instead), Autorun (`Ctrl+Q`, while following), Clock paused (`Ctrl+P`). Under them, the squads the client knows with their
  colours and counts (with the squad overlay).
  The panel also ticks the plain on/off settings (live, saved like the Options screen saves them): Developer text (`debug.hud_text`, the fps and timing lines of 6.10),
  Light probes (`debug.light_probes`), Pick mask (`debug.pick_mask`) and Route preview (`debug.route_preview`). The `Settings...` button opens DEBUG
  SETTINGS, the rest of the development options (the navmesh stair angle and level band, logging, and the others), a tab per group, with Find; it is shown
  only with `--debug`. The window and Options > Keys > Debug keys hold the rest.
  The setting `debug.light_probes` (experimental, off) draws the light probes of the nearest building with an interior as points, grey by how much
  sky reaches them and red inside walls; the first time it bakes that building on a background thread (seconds) and keeps the result in the cache.
- **Scenes** — the scene bookmarks, as buttons. Offline only (no running game).
- **Windows** — the buttons toggle their window (a second press closes it; `Keys` is Options at its Keys tab) and open: `Animations` (the animation lab, 6.2; enabled when the lab can load the game data),
  `Lights`, `Trails window`, `Full map`, `Keys` and `Decisions` (below). **Actions** — `Spawn bandit patrol` (with a running game and its towns loaded),
  `Clear height probe` (live only while a probe is set: it forgets the probe, 6.5a), and
  `Character...`, which shows or hides the CHARACTER toolbar with the cheats: `Heal`, `Knock out`, `Wake` and `Kill`, live with a running game,
  a debug view and a character in the status panel (otherwise dim). The toolbar is a small window of its own: it stays up after a click (heal several
  characters in a row), after a click elsewhere and after the debug panel closes, until its own close button (or `Escape` while it has the focus) or
  `Character...` again. Drag it by its title bar; the client remembers where it is and whether it is shown, so the next start puts it back
  (`debug.character_bar`, `debug.character_bar_x` / `_y`). Its buttons each act
  on the character the status panel shows — heal every part and stand
  up, drop it with a knock-out, clear the knock-out, or bleed it out. `Decisions` opens that character's DEBUG VIEW
  tree (6.4). `Lights` opens the LIGHTS window (`Shift+F7`), `Trails window` the trails window (`Shift+F6`), `Full map` the
  full-screen world map (`Ctrl+M`), and `Keys` opens Options at its Keys tab: the key list lives in Options > Keys, not in this panel. The window buttons print no reply. The reply line under Actions says what the server answered to an action (`ok`, or why it refused, e.g. `not down`).
- **Time** — `Dawn`, `Noon`, `Dusk`, `Midnight`, `+1h`, and beside them a round 24-hour dial: midnight at the bottom, noon
  at the top, the hours running clockwise (6 on the left, 18 on the right), daylight shaded gold, the hand at the game
  time. Click or drag on the dial to set that time (to the quarter hour, sent when you let go); its tooltip shows the
  time under the pointer. **Weather** — a scrollable list of the game's weathers,
  "auto" first. With a running game these go through the server's debug view.

### 6.2 The animation lab

`F8` → Commands → `Animations` opens `ANIMATIONS` (the command is enabled when the lab can load the game data):

- Left: race and sex, a name filter, the clip list (`PREV` / `NEXT` walk the filtered list, wrapping; a row's groups
  are named faintly at its right) and "N of M".
- Centre: the clip rendered offscreen. Drag to orbit, the wheel to zoom. The transport: step back / forward, `PLAY` /
  `PAUSE`, a frame scrub, the speed (0.1x to 2x, notched at 1x), the frame count and the reply line.
- Right: `CAMERA` presets (side, front, top, orbit); `PLANT` (the sole points that touch the ground: L/R heel and
  ball, none, or a bone from the dropdown); `OVERLAYS` (sole trails, root path, bone names, follow root, each with its
  tick); `MEASURE` and `MARK`; the readout; `COPY` (copies the readout).
- Keys while the window has the focus (but not the filter box): Space play / pause, `,` / `.` one frame back /
  forward, `M` mark, Escape closes.

### 6.3 Trails

`Ctrl+F6` turns on the trails: for every drawn character, its feet and its sim and drawn centres are
recorded and drawn as coloured tracks over the scene (the newest 30 s; older points go only to the trails dump), with a pop marked in the warning colour where
the simulation and the drawing disagree. `Ctrl+F6` again turns it off.

`Shift+F6` opens the `TRAILS` panel over the top left (`--debug` only, like the trails): `On` starts and stops the
recording; `CLEAR` forgets every point kept so far; `Window` is the last N seconds shown (`0` = the newest 30 s, the
cap; a larger N is clamped to it); `LAST 30 S` sets it to 0 (the number is the cap). The list has an `All` row and a row per fighting
character — its track colour, id, name, points and pops (red when there are any) — and a click on a row shows only
that character's tracks; `All`, or the same row again, shows everyone's. The line under the list is the trails'
status. `Shift+F6` again closes the panel.

### 6.4 The squad overlay and DEBUG VIEW

`Ctrl+F4` shows the live squads as coloured pillars, coloured by what each squad is doing (its goal kind). Clicking a
pillar opens its **DEBUG VIEW** tree window: the sim's rows as a tree with a tick, a filter, `Auto` (re-asks every
couple of seconds), `REFRESH` and `COPY`; Shift+click opens or closes a whole branch. The window's title carries the
squad's colour and its template.

`F8` → Commands → `Decisions` opens that same window for the character the status panel shows (the selected character,
or an NPC you inspected with a click), with the AI's goal selection at the end: one `pick` row per recent pick, newest
first, headed by its tick, and under it a `goal` row per candidate goal — its verdict (`Chosen`, else why it lost:
`Outscored`, `NoTarget`, `OutOfWindow`, `LowerTier` or `HoldCapped`) with its tier, target and the utility, weight, crowd
and score numbers the sim scored it by. A character that has not picked a goal says so. The server must run the debug
view (`demo.sh`, and so `play.sh game|scenario|spectate`, passes `--debug-view`; a title's own local server gets it
only when `session.server_args` names it): without it the window opens but no rows arrive. From a script,
`debug char ID|mine` opens the same window (`mine` is the status panel's character), and `dump debugtree` prints the
raw rows.

### 6.5 The navmesh overlay

`Ctrl+F5` draws the client's navmesh as glowing edges round the focus: walkable polygons, stairs and ramps in orange,
the wall band, the height band round the selected character's feet (below dimmed, above faint), the hovered polygon
white, and — from the F8 panel's "Navmesh: pruned polygons" — the polygons the bake pruned in dim red.

### 6.5a The vertical height probe

`Ctrl+F10` turns probe mode on (it needs `--debug` and a dev build): a left click on the world then sets the probe at that
x, z instead of acting. The same switch is the F8 panel's `Height probe (Ctrl+F10)` toggle (6.1). To drop a probe,
`Clear height probe` in the panel's Actions forgets it — no chart, no line in the scene, no `probe:` lines in an F12 shot —
and leaves the mode as it is (the button is live only while a probe is set). The probe lists, top to bottom in the developer text, every surface the vertical line through the
point crosses, each with its y and source: the drawn building / layout triangles (cyan), the building collision triangles
(orange; door leaves and thin props dimmer, `coll-skip`, listed only: the standing ray no longer reads collision, kenshi-k16.2.382.2), the navmesh polygons on every
storey (green, with the detour tile layer), the terrain (red `terr-tri`: `Heightmap::terrainHeight`, the drawn lattice triangle the standing
ray lands on, the ground a body stands on; yellow `terr-bilin`: the bilinear `Heightmap::height`, listed beside it because the two differ by up to ~0.5 u on rough ground).
Where a character stands within 1 u of the point it
adds the wire y, the standing ray's window (`wire - 4.5` .. `wire + 1.5`, the constants `SimView::kRayDown` / `kRayUp`; rows
inside it are marked `W`; a drawn floor is looked for down to `wire - 8`, `SimView::kDrawnDown`, its own row) and the ray's pick (`STAND`: its y and what decided it: drawn floor, terrain or open ground, or `MISS` with the reason), by the same rule the body is placed with (`SimView::standRayAt` calls the `standTerrain` helper `update()` uses).
The column runs from 40 u under the terrain line to 100 u over it; a character within 1 u widens it to its wire y `+/- 14` u, so a body
standing far above the ground (a high floor, a wire y floating over nothing) lists the surfaces where its ray looks (kenshi-k16.2.382.2).
In the scene it draws a vertical line with a tick per crossing in the same colours. An F12 shot's `.txt` carries the same
listing as `probe:` lines while a probe is set. From a script, `probe height X Z` (or `id=N`: at that character) sets the probe and prints the listing
to stdout (`probe: ...` lines; `--debug` only).

### 6.6 Camera and world toggles

- `Ctrl+F2` cycles the terrain view: the detail splat, the biome map, plain, and the colour map only (the last level
  only when the detail maps are loaded).
- `Ctrl+F3` shadows on/off; `Ctrl+G` walk mode (the camera stays at eye height on the ground) on/off.
- `Ctrl+C` follows the selection (the follow camera); `Ctrl+F` the free camera; `Ctrl+Q` autorun while following.
- `Ctrl+Tab` mouse look on/off (the pointer is locked); with the free camera, `PageUp` / `PageDown` move it up and down
  and hold `Left Shift` for the fast rate. The wheel changes the fly speed.
- `Ctrl+M` toggles the full-screen world map; a left click on it teleports the camera there, keeping the height above
  the ground.
- `Shift+F7` opens the `LIGHTS` window, the light debugger (a dev build; see-through, drag it by its title): one row per
  light term of the scene, grouped sun, ambient, lamps, emissive, fog, sky, air, post, with one row per lamp in view
  (nearest first; the rows change as you move). The tick turns that term off or on; `SOLO` leaves only that term on (and
  what it needs to show, e.g. the sun under its shadows); `ALL ON` puts everything back. The line at the top counts the
  terms switched off, in red. `Shift+F7` again, the X or `Escape` closes it; the switches stay as set. The script op
  `light <name> on|off|solo` and `light all on` do the same.

### 6.7 Clock and weather

`Ctrl+P` pauses the sky clock; `Ctrl+T` cycles its speed; `Ctrl+[` / `Ctrl+]` move it back / forward half an hour, with
Shift three hours. `Ctrl+,` / `Ctrl+.` step to the previous / next weather (auto, then every weather record).

Venge's daytime weather is the sky beams: columns of orange-white light strike the ground and wander for 10 to 30
seconds, burning anyone they pass over (gear with weather protection against burning helps). They only run between
05:00 and 23:00; at night the region's weather has no beams.

Some weathers (the deadlands' heavy rain, for one) put lightning in the sky: a bright, flickering column lands on
the ground up to two kilometres from a player character in each direction, one at a time, each flash lasting about a
second. It flickers as it falls, harms nobody, and comes with no thunder and no lit sky.

### 6.8 Screenshots

`F12` (or `PrintScreen`) saves a screenshot of the next drawn frame; `Alt+F12` waits three seconds first (so a held key
can show). A toast says where it went. The folder is the `screenshot.dir` setting, else `~/.local/share/kclient/screenshots`
(`%LOCALAPPDATA%\OpenKenshi\data\kclient\screenshots` on Windows).
Beside the picture the client writes a text sidecar recreating the view. A `clicks:` block in it lists your last 6 clicks (newest first) with the exact camera, screen point and what the pick hit or passed, so a click that went somewhere odd can be explained from the shot even a second later. Its `options:` line lists every setting changed
from its default (sorted `key=value`, window geometry left out, `(defaults)` when none), and the same settings follow
the `recreate:` line as `--set KEY=VALUE`, so a look reproduces even after the settings file has changed; while the whole-fight trails record, a trails
file (the whole span kept; a player character keeps 60 s, or since its last move order when that is longer, up to
120 s); with `--debug`, a route file (`<shot>.route.tsv`: each selected character's current route and goal, its last 8
move orders and the server's answer to each — the way to report a character stuck on its way somewhere); and with the
debug view, the on-screen characters' debug trees.

The text sidecar also says what the pointer was on: the `cursor:` and `pick:` lines (the pick's kind, id, point, walkable) are followed
by `owner:` (what the cursor belongs to: its kind, record name or display name, id and position; a thing with no record name says
so rather than leaving it blank), `primitive:` (which pick volume was hit and its bounds: a collision shape's box, a prop or layout
piece's mesh box and file, a character's hull, an item's pick sphere, the terrain; plus an `also under the ray:` line naming a bed
the ray crosses that did not take the pick) and `rule:` (the hover rule that chose the cursor shape, e.g. `bed -> use`). A script's
`screenshot` writes the same sidecar beside its picture.

### 6.9 Scenes

The F8 panel's scenes place the client at a named camera bookmark for a screenshot or a look. They are for offline
looks: with a running game the panel lists no scenes.

### 6.10 The developer text

With `--debug` the view gains a block of command-line text at the top left, while the interface is up and the option **Developer text:
  debug.hud_text** (Options → Debug, default off, saved in `kclient.cfg`) is on: camera position, ground, altitude,
  zone, heading, the nearest town, the buildings and foliage drawn, the game time, the weather, the scene, shadow state,
  frame and GPU timings, the sim link line and the sim profile. Off, the block is not drawn, but the `F12` screenshot's
  `.txt` still lists every line as `hud.<key>:` (pos, camera, nearest, buildings, time, foliage, squads, weather, gpu,
  building, sim, simprofile).

`F7` hides the block with the rest of the interface. (The minimap is the player's, 3.11.)

## 7. Keys

One table, defaults; rebind in Options → Keys (the development keys are under Debug keys, with `--debug`). A key
marked *inert* is bound today but the client does not act on it yet: **not built yet**.

| Group | Key | What it does |
|---|---|---|
| Camera | `W` `S` `A` `D`, `Up` `Down` `Left` `Right` | Move the camera |
| Camera | `Q` / `E` | Rotate the camera left / right |
| Camera | `,` / `.` | Tilt the camera up / down |
| Camera | `Home` / `End` | Zoom in / out |
| Camera | Right-drag / Middle-drag | Look around / orbit |
| Camera | `Left Ctrl` (held) + mouse | Rotate the camera with the mouse |
| Camera | `Z` | Focus the camera on the selection |
| Camera | `Semicolon` | First-person camera — *inert* |
| Camera | `V` | Hover / direct-drive camera on the selection (again: leave it), or a double right click on your character; a wheel step, `Home` or `End` also leaves it |
| Camera | `Shift+Semicolon` | Camera grid — *inert* |
| Camera | `R` / `F` | Floor up / down |
| Camera | Wheel | Zoom while following; fly speed on the free camera |
| Game | `Space` | Pause |
| Game | `F2` / `F3` / `F4` | Game speed 1 / 2 / 5 |
| Game | `F5` | Quick save |
| Game | `Ctrl+F9` | Quick load |
| Game | `F6` | Control hints panel |
| Game | `F7` | Hide the interface |
| Game | `F12`, `PrintScreen` | Screenshot (6.8) |
| Game | `Alt+F12` | Screenshot after 3 seconds |
| Game | `Left Alt` (held) | Highlight item labels (and character names, with the option) |
| Game | `F1` | Help (the TUTORIALS browser) |
| Selection | `1`-`9`, `0` | Select character 1-10 (the same number twice focuses) |
| Selection | `Tab` / `[` | Next / previous character |
| Selection | `` ` `` | Select all |
| Selection | (no key) | Next squad — the action exists, unbound (click a squad tab) |
| Selection | `Backspace` | Stop |
| Orders | `X` | Sneak |
| Orders | `Num1` | Hold position — *inert* (the HUD's HOLD tick works) |
| Orders | `Num2` | Passive — *inert* |
| Orders | `Num3` | Ranged — *inert* |
| Orders | `Num5` | Taunt — *inert* |
| Orders | `Num6` | Cycle run speed |
| Orders | `Num7` | Medic — *inert* (the HUD's MEDIC button works) |
| Orders | `Num8` | Rescue — *inert* |
| Orders | `Num0` | Block — *inert* (the HUD's BLOCK tick works) |
| Orders | (no key) | Jobs — the action exists, unbound (the HUD's JOBS tick) |
| Orders | (no key) | Prospect — the action exists, unbound (the HUD's PROSPECT button) |
| Windows | `I` | Inventory |
| Windows | `C` | Character stats (4.12) |
| Windows | `M` | Map |
| Windows | `T` | Research |
| Windows | `Y` | Crafting |
| Windows | `G` | Faction |
| Windows | `U` | Squads |
| Windows | `F10` | Options |
| Windows | `B` | Build |
| Windows | `Escape` | The in-game menu; while direct driving, leaves the drive instead |
| Windows | `Ctrl+N` | Playtest notes (`--debug`, during a run) |
| Build mode | `Space` | Place — *inert* (no placement yet) |
| Build mode | `Shift+Backspace` | Undo — *inert* |
| Build mode | `,` / `.` | Rotate left / right — *inert* |
| Build mode | `[` / `]` | Tilt — *inert* |
| Build mode | `=` / `-` | Move up / down — *inert* |
| Debug | `F8` | The debug panel |
| Debug | `Ctrl+F2` | Terrain view mode |
| Debug | `Ctrl+F3` | Shadows |
| Debug | `Ctrl+F4` | Squad overlay |
| Debug | `Ctrl+F5` | Navmesh overlay |
| Debug | `Ctrl+F6` | Trails (the last 30 s) |
| Debug | `Ctrl+F7` | Collision overlay: the building collision shapes and the terrain bodies stand on |
| Debug | `Ctrl+F10` | The vertical height probe mode (6.5a) |
| Debug | `Shift+F6` | The trails panel (6.3) |
| Debug | `Shift+F7` | The light debugger window (6.6) |
| Debug | `Ctrl+PageUp` / `Ctrl+PageDown` | Foliage range up / down |
| Debug | `Ctrl+G` | Walk on the ground |
| Debug | `Ctrl+C` | Follow the selection |
| Debug | `Ctrl+F` | Free camera |
| Debug | `Ctrl+Q` | Autorun while following |
| Debug | `Ctrl+Tab` | Mouse look |
| Debug | `PageUp` / `PageDown` (held) | Free camera up / down |
| Debug | `Left Shift` (held, free camera) | Fast fly |
| Debug | `Ctrl+P` | Pause the sky clock |
| Debug | `Ctrl+T` | Sky clock speed |
| Debug | `Ctrl+[` / `Ctrl+]` | Sky clock back / forward half an hour |
| Debug | `Ctrl+Shift+[` / `Ctrl+Shift+]` | Sky clock back / forward 3 hours |
| Debug | `Ctrl+,` / `Ctrl+.` | Previous / next weather |
| Debug | `Ctrl+M` | Full-screen world map (click teleports) |
| Debug | `Space`, `,` / `.`, `M`, `Escape` | In the animation lab's window only (6.2) |

## 8. Not built yet

The original has these; the client does not do them yet:

- **Import game** (title).
- The advanced options values do not reach the running game.
- The overview's DIALOGUE and AI pages ("Not available yet"; conversations do have their own screen, 4.9).
- Research and crafting beyond the data display: no research bench and no crafting bench in the game, so both pages
  are read-only and empty (4.4, 4.5).
- Building placement: the build window browses only; there is no ghost, `CONFIRM` or `UNDO`, and the build-mode keys
  are inert (4.7).
- The orders ticks Passive, Ranged and Taunt; the RESCUE button; the keyboard toggles Hold, Passive, Ranged, Taunt,
  Medic, Rescue and Block (the HUD ticks and buttons do work for Block, Hold, and Medic).
- First-person camera and camera grid.

If something here is wrong, or something you can see is missing, that is a bug in this guide: the rule in
[`client/README.md`](../client/README.md) is that a merge which adds or changes something the player can see or press
updates this file in the same merge.
