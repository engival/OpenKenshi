# Layout files

A screen that opts in describes its widgets here, in `<name>.layout`. The code builds them (`ui::Layout`, `client/src/ui/layout.*`),
finds them by `id` and gives them their actions. With `ui.layout_reload` on (the default), saving a file rebuilds its screen
while the client runs.

This is our own small format, not the game's layout XML and not HTML.

## Lines

```
# a comment (a whole line)
kind "text" key=value key="a value with spaces"
	child
		grandchild
```

- One widget per line: its kind first, then its text in quotes if it has any, then its settings as `key=value`.
- Indentation makes children: a line indented deeper than the line above belongs to it. Tabs count as 4 spaces.
- Every mistake is an error: an unknown kind or setting, a bad value, an id used twice, children under a label.

## When something is wrong

- The error is printed as `file:line: what` on stderr, and a red box shows it in the screen's place.
- A bad edit while the screen is running keeps the last good layout under the red box. Fix the file and save again.
- There is no built-in copy to fall back on.
- An id the code needs but the file lacks is an error too. The screen keeps working, but that piece is missing.

## Kinds

| kind | what it is | children |
|---|---|---|
| `row`, `col` | children side by side / one under another | yes |
| `stack` | children on top of each other, each placed by its own `halign` / `valign` | yes |
| `panel` | a box drawn with `look=`: `none`, `window`, `panel`, `inset`, `field`, `button` or `hud`; `pad=` | yes |
| `scroll` | a column that scrolls; `gap=` between children | yes |
| `slot` | a place the code fills with its own widgets; they stay across a reload | from the code |
| `spacer` | empty space | no |
| `label` | text: `"text"`, `colour=` (a palette name or `#rrggbb`), `alpha=`, `wrap=on` (with `size=auto` in a `col`: as high as the wrapped text), `text_align=`, `text_valign=` (start / center / end) | no |
| `button` | a button: `"TEXT"`; the code says what it does | no |
| `tabs` | a tab bar: `side=top` or `side=left` | no |
| `edit` | a one-line text box: `"hint"`, drawn dim while it is empty | no |
| `check` | a tick box: `"its label"` | no |
| `dropdown` | a closed list: `"placeholder"`; the code gives its items | no |
| `slider` | a horizontal slider: `min=`, `max=`, `step=` (0: smooth) are its values here, not the row's limits | no |

## Settings

- On any widget:
  - `id=name`: how the code finds it.
  - `w=`, `h=`, `x=`, `y=`: in units (1 unit is 1 pixel at 1080 lines).
  - `tip="..."`: a tooltip.
  - `visible=off`: hidden.
  - `font=`: a game font name. Labels and tabs default to `Kenshi_StandardFont_Medium`, buttons to `Kenshi_PaintedTextFont_Small`.
- On `row` and `col`: `gap=` between children, `pad=` around them, and `justify=start|center|end` for where they sit when none fills.
- On a child of a `row` / `col`:
  - `size=` is its length along the row (its width) or the column (its height):
    - a number: that many units;
    - `auto`: what it needs;
    - `fill`: a share of what is left; `fill:2` takes twice the share.
  - `min=` and `max=` limit that length.
  - `align=start|center|end|stretch` places it across the row or column. The default is `stretch`.
- On a child of anything else: `halign=` and `valign=` (start / center / end / stretch). The default is `stretch`.

## Files

- `options.layout`: the Options window's body (`ui::Options`).
- `debug.layout`: the debug window's body (`ui::DebugPanel`, F8): toggles and scenes; commands, the reply, time and weather; the keys.
- `debugtree.layout`: the DEBUG VIEW tree window's body (`ui::DebugTree`).
- `trailspanel.layout`: the TRAILS panel's body (`ui::TrailsPanel`).
- `lightspanel.layout`: the LIGHTS window's body (`ui::LightsPanel`): ALL ON, the off count, the term rows.
- `scenarios.layout`: the playtest SCENARIOS window's body (`ui::ScenariosWindow`).
- `notes.layout`: the playtest NOTES window's body (`ui::NotesWindow`).
- `cast.layout`: the spectator's CAST window body (`ui::CastWindow`).
- `animbrowser.layout`: the animation browser's body (`ui::AnimBrowser`): clips, the render, the transport, camera, plant, overlays, the readout.
