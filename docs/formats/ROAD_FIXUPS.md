# Road fixup table (format v1)

A user-made data fix over the game's ROAD markers: the original walks the broken data (the Hub spike of `424-Newwworld` marker 3, kenshi-rwh.300),
we patch it. The game's files are never edited. Agreed on kenshi-k16.6.713 (ksim + client); loader kenshi-k16.6.712; the client's in-world editor
(kenshi-k16.2.513) writes the rows. Code: `kworld/roadfixups.hpp` (`parseRoadFixups`, `applyRoadFixups`, `writeRoadFixups`): the sim and the editor
share it, so what the user sees is what the sim walks. Test `ksim_road_fixups`.

**File:** `data/fixups/roads.tsv` (source tree; `kworld::defaultRoadFixupsPath()` = `$KSIM_ROAD_FIXUPS`, else that file). `RoadNetwork::load(dataDir, fixupsPath)`
takes the path explicitly (empty = no table). The file ships with the header only; the editor writes the rows.

**Text:** UTF-8 TSV. `#` lines are comments, except the first, which must be `# kenshi-road-fixups 1`. Then the header row, then one row per edit:

| column | meaning |
|---|---|
| `road` | the ROAD record's string id (`424-Newwworld`) |
| `op` | `move`, `insert`, `delete`, `mark` |
| `index` | the marker's index in the road **as the game data loads it** (hex-key order). Empty only on a `mark` row: the whole road |
| `x y z` | the new position (world units): `move`, `insert`. Empty otherwise |
| `old_x old_z` | the original marker's xz, the guard: required on `move` / `delete`, optional on `mark` |
| `status` | `ok` / `unsure` / `bad`: the reviewer's mark |
| `note` | free text (optional last column) |

**Rules.** Indices always refer to the original list, so rows are independent and order-free. At most one `move`/`delete` per original marker (a second is
skipped). `insert` puts a new marker after original marker `index` (not after the last); several inserts after one index keep file order. A road's first and
last markers may move but not be deleted. `mark` changes nothing in the sim. Reconnecting an end to another junction is v2 (kenshi-dkh3).

**Apply.** A guard mismatch (xz off by more than 0.5 u), an unknown road, an index out of range or a rule break skips that row with a warning; the rest apply. A
malformed row (unknown op or status, bad number) is skipped and reported with its line (`RoadFixupTable::errors`); a missing or newer version line, a bad header or an
unreadable file makes `load` log a loud warning naming the file and load the roads UNPATCHED. A bad table never takes routing down or stops the server. A patched road
keeps its `original` markers and has `patched = true`; its stored `length` is rescaled by (new polyline / old polyline). Each applied row is logged once on
stderr: road, marker, before -> after, road length before -> after. `Road` also carries `sid` and `hidden`.

**Hash.** A table with at least one row folds its content hash (FNV-1a 64 of the bytes) into the server's world data hash (`foldRoadFixups`): a save or client
with another table is flagged as a data mismatch. A header-only table leaves the hash unchanged.

**Writer.** Deterministic: rows sorted by road id, then index (a road-level `mark` first; ties keep their order), `%.2f` coordinates, tabs and newlines in notes
written as spaces.

## Deviation from the original

A data fix the user makes, not the original's behaviour: the original follows the broken marker (a 3 km out-and-back into the Hub from `424-Newwworld` #3).
With rows in the table the sim's road routes differ from the original's there. Listed in `docs/reference/PATHFINDING_STATIC.md` section 2, item 8.
