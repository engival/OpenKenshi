# Stats panel replacement (PARKED)

Parked 2026-11-03 by the user: major panel rewrites wait until much later in the project. Nothing here is wired into
kclient. This doc is the whole state, so the work can restart from a fresh context.

## Goal

The original HUD's status panel (bottom left) has a stats list by name (MARTIAL ARTS / STRENGTH / ...) and a ^ button
that slides up the extended rows (XP, KO point, run speed, hunger rate). The user always hated the ^ button. The
replacement is one panel: the stats as a table of cells (an icon or a short label over the value, the modifier under it,
an optional XP strip). This leaves room for extra stats such as run speed, with no expand button.

## Decisions (user, 2026-11-03)

- Three layouts, one key: `ui.stats_panel = icons | text | original`.
  - `original`: today's stat list and ^ button, unchanged ("if people want original kenshi, they get original kenshi").
  - `text`: short labels instead of icons ("Str", "Dex", "Tgh"), for players who don't want generated art.
  - Default: `original` until the icons land (the client coordinator's call), then reconsider.
- Icons: the user's generated PNGs (Qwen Image; the user cleared them for the FOSS repo). They are still being generated,
  so don't wire any until the user names the final picks. WIP: `~/git/kenshi/kenshi-inspect/findings/stat_icons/picks/`
  (RGBA, about 190-390 px, soft baked shadow). `_`-prefixed files are not cut out yet (opaque backgrounds).
- The weapon row follows the equipped weapon class, as the original does (`CharStats.weaponClass`):
  - unarmed (0): MARTIAL ARTS, STR, DEX, TGH, DODGE;
  - melee (1): ATTACK, DEFENSE, STR, DEX, TGH, DODGE;
  - ranged (2): CROSSBOWS, PRECISION, PERCEPTION, STR, DEX, TGH (no DODGE).
- Hover text goes to the HUD's fixed tip box (done: 7bdaf13, `ui.hud_tips`). That is what makes icons usable: each cell's
  `tip` names the stat and explains it.
- The run speed cell takes the host's unit label (`ui.speed_unit`, kph default | mph), never a hard-coded kph.

## What exists (merged on master)

- `client/src/ui/statgrid.*`: `ui::StatGrid` (rows of `StatCell{icon, value, note, noteColour, xp, tip, label}`, each row
  split evenly; `labels` draws `label` in the icon's place). `drawStatIcon` draws our own stand-in pictograms.
- kui_demo `--show statsmock` (d221ef1, 1f56fd1, 06916d0, b39e4e4, 17d05cd): three class panels with icons, the same three
  with text labels, and after about 4 s the unarmed one in the HUD's place. The script plays in a window too.
- docs/UI.md `statgrid.*` entry.

## Open look issues (from the mockup shot)

1. Our drawn icons don't read (the arm looks like a blob, the fist like a building, the crossbow like an umbrella). The
   user's PNGs replace them, with ours kept as the fallback.
2. Icons too small and faint: go to about 34 px at 1080p. At 26 px the user's crossbow, nunchaku and knife-hand
   icons turn to mush; bold single silhouettes survive (fist, arm, eye, weight, heart, shield, plate, crosshair, swords).
3. The notes (+4, "xp x3", kph, KO, rate) are tiny; the red "-5" is the worst, especially on the original style's parchment.
4. The bottom row's meaning is unclear ("-13 KO", "100% rate"): the text labels "KO" / "Hunger" read better.
5. XP strips are nearly invisible.
6. Cramped: 5-6 cells in 243 units. Widen the panel or cap a row at 4-5 cells.
7. The icons' baked dark shadow may look muddy on the parchment (`ui.style=original`).

## Wiring plan (to send to the client coordinator when this restarts)

UI side: the Hud builds the StatGrid beside the existing rows and shows one by `ui.stats_panel`; the original rows and ^
button stay untouched. Host side: a new `HudInfo` list of cells (or the Hud maps the existing lines). Fields each cell
reads (`simlink::CharStats`, built today in main.cpp `stats_info()`):

| Cell | Field | Note |
|---|---|---|
| Martial Arts / Attack / Defense | `skills[kMartialArts / kAttack / kDefence]` value + bonus | by `weaponClass` |
| Strength, Dexterity, Dodge | `skills[kStrength / kDexterity / kDodge]` value + bonus | Dodge not for ranged |
| Toughness | `skills[kToughness]` value | never shows a bonus |
| Crossbows, Precision, Perception | none yet | the sim has no such skills in `CharStats::Skill`: ask the sim |
| XP strips | `athleticsXp`, `strengthXp`, `toughnessXp` (a multiplier, "3x") | the rest have none on the wire |
| Run speed | `runSpeed`, `runPenalty` x `speedScale()` + `ui.speed_unit` | any modifier red |
| KO point | `koPoint` | UNSURE: the original's second figure |
| Hunger rate | `hungerRate` | percent |

kui_demo checks per mode (cells, tooltips into the tip box, `original` unchanged), docs/UI.md entry, baselines in CLAUDE.md.
