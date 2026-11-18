# Town routine data: where the sites live (TQ23)

Verified 2026-11-01 on the merged `gamedata.base` + `Newwworld.mod` + `Dialogue.mod` + `rebirth.mod` and the four `newland/leveldata/**/interiors.level` files. Code: `cpp/include/ksim/routines.hpp` (`classifyPiece`, `LayoutLibrary`), test `ksim_town_goals`. Design: `docs/reference/TOWN_LIFE_SLICE.md` section 5.4.

## BUILDING_FUNCTIONALITY `function` (86 records)

| function | meaning | examples |
|--:|---|---|
| 6 | bed | `sleeping`, `sleeping on floor` |
| 9 | shop counter | `shop counter` (BUILDING `Shop Counter Old`, `Shop Counter Large old`) |
| 16 | table | `table` (BUILDING `Round table`, `barrelTable`, `barrelTable2`) |
| 17 | chair / stool (not a site) | `chair`, `chair-fixed`, `sit on floor` |
| 8 cage, 2 storage, 13 general storage, 10 crafting bench, 11 cooking pot / corpse disposal, 12 gun turret | other users | |

## Nodes

A node is a BUILDING with `is node` true; its `node type` is: 0 Shopkeeper, 1 General (also the default of any record), 2 **Guardpost**, 3 Defensive position / Turret, 4 Use, 5 Light mount. Guard nodes (type 2) and the Shopkeeper / General nodes are **not** child instances of any BUILDING record (those hold only Use, Light mount and Turret nodes: 772 / 676 / 119). They are placed by interior layouts.

The Shopkeeper node is where the shopkeeper stands (STAND_AT_SHOPKEEPER_NODE, task 20): behind the counter, e.g. the Hub bar's markers lie east of its Shop Counter (x -50549 against the counter's -50558.7) while customers walk to the counter itself. `SiteKind::ShopkeeperNode` (real pieces only) carries them; the Shopkeeper goal takes those slots when the building's layout has any, else the Counter slots (kenshi-k16.6.106; test `ksim_barman_node`).

## Interior layouts

`interiors.level` (a normal kdata file): one `INSTANCE_COLLECTION` (type 30) per layout, sid `<building sid>-<layout name>`, string `associated building`. Each instance names a furniture / node BUILDING with a building-local transform. Example `611-gamedata.base-bar2` (the Saloon): 7 Node Guardpost, 3 Node Shopkeeper, 15 Node Generaal, 14 Old Stool, Round table, barrelTable, barrelTable2, Shop Counter Large old. 410 layouts across the four files.

## Not found

The town outside nodes of STAND_AT_GUARD_NODE_HOMETOWN_OUTSIDE (157): no marker in the BUILDING data or the interior layouts. Which layout a placement uses is in the zone placement's GAMESTATE_BUILDING (`interior layout name`), not yet read by `kworld`.
