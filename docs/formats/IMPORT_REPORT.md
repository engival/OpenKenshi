# Import report (format 1)

Written by `ksim_server --import-save SAVEDIR --import-report PATH` right after the import (kenshi-cym.42): the id map from the original save's handles to our entity ids, for the side-by-side agent (kenshi-cym.40.1). Written to `PATH.tmp` and renamed, so a reader never sees half a file. Source: `saveimport::Report` (cpp/include/ksim/save/save_import.hpp). Serves the original's handles only; ids are valid for the session that wrote the report.

```json
{"format":1, "save":"<SAVEDIR as given>",
 "squads":  [{"pool":<platoon pool index>, "id":<squad entity id>}],
 "members": [{"pool":<platoon pool index>, "serial":<member serial>, "id":<member entity id>}],
 "selected": <entity id of CAMERA's selected character, 0 = none>,
 "selection": [<entity ids>],
 "camera": {"has":<bool>, "pos":[x,y,z], "rot":[w,x,y,z], "zoom":z},
 "counts": {"members":n, "skipped":n, "unresolvedHandles":n}}
```

- `pool` / `serial` are the original's handle fields (`handleC` and `handleI` of a PLATOON / character record). The player's squad maps to the player squad's id.
- A member the combat data could not build, and a never-activated platoon's members, have no row (`counts.skipped` counts the first).
- `selected` / `selection` are resolved through the member map; handles that named no imported member are `counts.unresolvedHandles`.
- `camera` is CAMERA's position, rotation (w x y z) and zoom as saved; `has` is false when the save had none.
- Ids are unsigned 64-bit integers, other numbers print with 9 significant digits. Rows are sorted by pool (then serial).
- A change of shape bumps `format`.

Server flags (documented at the top of `cpp/src/ksim/server_main.cpp`): `--import-save SAVEDIR`, `--paused`, `--import-report PATH`. `--paused` sets the admin pause at start; the first speed order (41) or pause order (40) of a client releases it (`ServerContext::pausedUntilOrder`; the wire is unchanged).
Test: `ksim_server_import`.
