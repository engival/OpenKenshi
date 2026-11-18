# kdata

C++20 library (standard library only) for Kenshi record files (`.base`, `.mod`, `.zone`, `.level`): `kdata::parse`/`write` (versions 1..17, byte-exact round trip) and `kdata::GameData` (overlay merge of several files). Tools: `kdump` (kmod.py-style summary), `kmerge` (merged-record TSV). Format: `docs/formats/MOD_FORMAT.md`; open questions: `NOTES.md`.

```
nice -n 10 cmake -S cpp -B cpp/build
nice -n 10 cmake --build cpp/build -j8
ctest --test-dir cpp/build
```

Tests read the game data (read-only) from the cache variable `KENSHI_DATA`
(default `$KENSHI_DATA` from the environment, else `~/.steam/steam/steamapps/common/Kenshi/data`); override with `-DKENSHI_DATA=...`. The parity test runs `tools/kmod.py` (`-DKDATA_KMOD=...`).

# kworld

World readers on top of kdata: `Heightmap` (fullmap.tif, bilinear), `Zones` (all zone files, placements with terrain-relative y converted to absolute, BUILDING scale), `Towns`, `GlobalPathing`. Tools `kworld_survey` (matches `tools/world_survey.py`), `kheight`. Tests compare against `tools/{terrain,kmod,global_pathing}.py` (`-DKWORLD_REF_TOOLS=...`). See `NOTES.md` (kworld section, UNSURE list).

# ksim

flecs (v4.0.4, `third_party/flecs`) world model on kdata+kworld: one prefab per live record (factions, towns, squad templates, characters, buildings, races, AI packages), relationships as pairs, 11,714 placed instances (layer-merged), and a fixed 20 Hz headless tick (`Simulation`) with Full/Squad/Strategic scheduling, per-subsystem RNG and a command queue as the only writer. Tools `ksim_stats`, `ksim_run`; tests `ksim_*`. See `NOTES.md` (ksim sections, UNSURE list, review).

Region scheduler (`ksim/world/region.hpp`): region-owned two-phase multithreaded tick over a stand-in workload (`RegionEngine`, tests `ksim_regions_*`). Design and review: `NOTES.md` ("ksim regions", "regions review"). TSan: configure a separate `cpp/build-tsan` with `-DCMAKE_CXX_FLAGS=-fsanitize=thread`.
