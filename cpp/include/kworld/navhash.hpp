#pragma once

// The original's navtile hashes (kinspect findings/navhash.md sections 1-4 and 10; the reader's stale-tile check compares
// them with those stored in data/newland/land/navtiles/tileI.J.hkt, knav/native.hpp). A 32-bit boost-style combine folds
// building positions, the interiors' children and a zone's vegetation into the values the game bakes beside each mesh.
// This is our own code written from those notes.

#include "kworld/foliage.hpp"

#include <cstdint>
#include <span>

namespace kworld {

// The mix the game folds with: state ^ (value + 0x9e3779b9 + (state << 6) + (state >> 2)), all 32 bits. Order matters.
std::uint32_t comb(std::uint32_t state, std::uint32_t value);

// A float's hash word: (uint)(int64)value ^ 0xdeadbeef (truncate toward zero), after values >= 2^32 subtract 2^32 first.
std::uint32_t F(float value);

// A building's own hash: x, z, then the four orientation words, then 1 when destroyed (section 2).
// The orientation words come from the game's rotation scratch buffer; every shipped one truncates to 0 (section 7:
// ~16,000 values over six towns, none reaching +-1), so they are folded as four F(0) here.
std::uint32_t buildingHash(double x, double z, bool destroyed);

// An interior's hash: the building's own hash (`destroyedGate`: its destroyed term), then every child's own hash in the
// game's gather order, then 0x100 when `destroyed` (section 3). The two flags differ: the building's own term needs the
// gate flag too, the interior's does not. The caller gates the children (the game folds all of a parent's gathered
// children or none).
std::uint32_t interiorHash(double x, double z, bool destroyed, bool destroyedGate, std::span<const std::uint32_t> children);

// A zone's hash: the included buildings' own hashes in ZoneMapContent list order, then the forest word when the zone has
// vegetation (section 4). The caller applies the game's inclusion filters to pick the hashes.
std::uint32_t zoneHash(std::span<const std::uint32_t> buildingHashes, bool hasVegetation, std::uint32_t vegetation);

// The forest word a zone's vegetation contributes (section 11, VERIFIED-live on zones 10,10 and 20,32): every placed
// tree-loader instance whose mesh passes the gate (FoliageInstance::navFold), in spawn order, its raw x and then its
// HEIGHT (world y, not z), each as F().
std::uint32_t vegetationWord(std::span<const FoliageInstance> trees);

} // namespace kworld
