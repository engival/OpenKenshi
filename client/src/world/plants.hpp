#pragma once
// The ONE plant predicate (kenshi-k16.2.420.6.1): does this FCS record draw a plant?
//
// The leaf sway (render.leaf_sway; render/wbdraw.cpp swayTop reads the part's `plant` flag) bends plants only. The
// user's rock (Foliage_1K_RockStack01, an FCS FOLIAGE_MESH record) swayed because the foliage path set the bend on
// EVERY FOLIAGE_MESH record: the draw path, not the data, decided. Trees and the scattered vegetation rocks are both
// FOLIAGE_MESH records and the shipped data has no plant flag, so the predicate reads the fields that decide how the
// exe draws the mesh:
//   - a `leaves mesh` (a canopy drawn with the vegetation material): a plant;
//   - the record's own mesh drawn with the FOLIAGE material (`material type` 4 on FOLIAGE_MESH / MAP_FEATURES
//     `texture mode` 4, the vegetation alpha material) and no `collision` mesh: a plant (palms, bushes, ferns and the
//     solid grass clumps are type-4 cards and solids);
//   - anything else (TERRAIN / UV / TRIPLANAR / DUAL materials, or any record carrying a `collision` mesh): a rock,
//     wreck, ruin or prop, static.
// Measured over all 748 FOLIAGE_MESH records (kenshi-k16.2.420.6.1 findings): the diffuse texture's alpha is the
// gloss mask (a rock's texture is as non-opaque as a palm's) and the FOLIAGE alpha test cuts on the NORMAL map's
// alpha, so neither texture separates plants from props. What the fields above still get wrong is data: the FCS ids
// and their reasons are in <client data>/foliage_plants.txt.
// The occluder fade's foliage exemption (kenshi-k16.2.411.4.1) calls the same isPlant.
#include <string>
#include <string_view>

namespace kdata { class GameData; }

namespace plants
{
struct Override
{
	bool plant = false;		// the data file's verdict: 1 = a plant, 0 = not (static)
	std::string reason;		// why, as written in the file
};

// Reads <client data dir>/foliage_plants.txt once, from the host before any world part is made (host/session.cpp
// "buildings and materials"). One line per record:
//   <FCS string id> <0|1> <reason...>
// `#` starts a comment; blank lines are skipped. A malformed line is one stderr line and is skipped (the record then
// takes the material rule). The table is immutable afterwards, so the load must run before the first isPlant.
// Returns false when the file cannot be read: the caller treats that as fatal (kenshi-k16.2.467; the material rule still
// decides every record the list does not name, but is no stand-in for the list).
bool load(const std::string& clientDataDir);

// The predicate: the data file's override when it names the record, else the FOLIAGE material rule. Rocks and props
// are false, so their bend weight (wbdraw.cpp swayTop) is 0.
bool isPlant(const kdata::GameData& gameData, std::string_view recordID);

// The data file's entry for an id, nullptr when it has none (`dump foliage` prints it).
const Override* overrideOf(std::string_view recordID);

// How many ids the file overrode (the client's report line).
int loadedOverrides();
}
