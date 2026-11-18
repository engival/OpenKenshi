# Supported mods

The client works with the plain game files. A few Steam Workshop mods for
Kenshi are also officially supported. If one is installed, the client finds it
automatically and turns it on. You can turn any of them off.

| Mod | Key | Workshop ID | What it does in this client |
|---|---|---|---|
| [Nice Map [Zones + Zone names + Roads]](https://steamcommunity.com/sharedfiles/filedetails/?id=1613829988) | `nicemap` | 1613829988 | Replaces the world map and minimap image with a labelled one: region names, zone borders, roads. |
| [Dark UI](https://steamcommunity.com/sharedfiles/filedetails/?id=1200632417) | `darkui` | 1200632417 | Planned: dark interface theme. Not used yet; the client's interface is its own. |
| [Slopeless](https://steamcommunity.com/sharedfiles/filedetails/?id=1740462542) | `slopeless` | 1740462542 | Lets you build on slopes: removes the slope limit for buildings, keeps farms level. Applied by the game simulation; loads after all other data. |
| [Less Buggy Bughouse](https://steamcommunity.com/sharedfiles/filedetails/?id=2600663487) | `bughouse` | 2600663487 | Fixes the collision of the "bughouse" storm house, whose walls and dome confuse character movement in the original. Applied by the game simulation. |

"Planned" means the client recognises the mod but doesn't use it yet.

## Where the client looks

- `mods/` in the Kenshi install folder (local mods)
- Steam Workshop downloads (`steamapps/workshop/content/233860/<id>/`)

A mod is recognised by the Workshop ID in its `_<name>.info` file, not by its
folder name, so renamed folders still work.

## Turning mods on and off

```
kclient --list-mods              # show supported mods: found / enabled / used
kclient --mod nicemap=off        # disable one for this run
kclient --mod nicemap=off --save-mods   # ...and remember it
```

`--mod` only applies to the run it is given on. Add `--save-mods` to write your
choices to `~/.config/kclient/kclient.cfg` (or `$XDG_CONFIG_HOME/kclient/kclient.cfg`);
they stay in effect until you change them. They are the `mod.KEY=on|off` lines of
that file, which also holds the client's other options; editing them by hand works too.
`--map FILE` overrides the map image
whatever the mods say. If the same mod is in both places, the copy in the
install's `mods/` is used.

## Credits

These mods belong to their authors. The client reads them from your own
installation. It never copies or redistributes their files.
