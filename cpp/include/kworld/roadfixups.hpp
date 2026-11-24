#pragma once

// The road fixup table (docs/formats/ROAD_FIXUPS.md, kenshi-k16.6.713 v1): a user-made data fix applied over the game's ROAD markers at
// RoadNetwork::load. One code path for the sim and the client's world editor: parse -> rows, apply -> patched roads + a result per row,
// write -> text (deterministic, so diffs stay readable).

#include "kworld/roads.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace kworld {

enum class FixupOp { Move, Insert, Delete, Mark };

struct RoadFixup
{
	std::string road;           // the ROAD record's string id
	FixupOp op = FixupOp::Mark;
	int index = -1;             // marker index in the ORIGINAL list; -1 = none (a mark row of the whole road). Insert: after this marker.
	double x = 0.0, y = 0.0, z = 0.0; // move / insert
	bool hasOld = false;        // old_x / old_z present: the guard
	double oldX = 0.0, oldZ = 0.0;
	std::string status;         // ok / unsure / bad
	std::string note;
};

struct RoadFixupTable
{
	int version = 1;
	std::vector<RoadFixup> rows;
	std::vector<std::string> errors; // the rows that did not parse ("road fixups: line N: ..."): skipped, the rest apply
};

struct RoadFixupResult
{
	bool applied = false;       // false: skipped; `why` says why
	std::string why;
	RoadNetwork::Marker before; // the marker before / after (move; insert: after only)
	RoadNetwork::Marker after;
	double lengthBefore = 0.0;  // the road's length before / after this row
	double lengthAfter = 0.0;
};

// Throws kworld::Error on a missing or newer version line (the whole file is then unusable). A malformed row, an unknown op or status is skipped and
// reported in `errors` with its line.
RoadFixupTable parseRoadFixups(std::string_view text);

// Applies the rows to `roads` (in place); rows refer to the original marker indices, so they are independent and order-free. A guard mismatch,
// an unknown road, an index out of range, a second edit of one marker or a delete of a road's end marker skips that row. The result vector
// matches `rows` one to one. A patched road keeps its original markers, and its length is rescaled to the new polyline (stored x new / old).
std::vector<RoadFixupResult> applyRoadFixups(std::vector<RoadNetwork::Road>& roads, const std::vector<RoadFixup>& rows);

// Sorted by road id, then index (rows of one index keep their order), %.2f coordinates, tabs and newlines in notes as spaces.
std::string writeRoadFixups(const RoadFixupTable& table);

// The FNV-1a 64 of the file's bytes when it has at least one row, else 0 (absent, header only or unusable: no table).
std::uint64_t roadFixupsHash(std::string_view path);

// `dataHash` with the table's hash mixed in (unchanged when roadFixupsHash is 0): a save taken with another table differs.
std::uint64_t foldRoadFixups(std::uint64_t dataHash, std::string_view path);

// $KSIM_ROAD_FIXUPS, else the source tree's data/fixups/roads.tsv when it exists, else empty.
std::string defaultRoadFixupsPath();

} // namespace kworld
