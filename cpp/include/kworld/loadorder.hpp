#pragma once

// The game-data load order: which .base/.mod files are merged, in what order.
// The simulation owns it (docs/ARCHITECTURE.md "Runtime state") and publishes
// the resolved list; everything static in kworld is a function of it.
//
// mods.cfg rule: one mod file name per line, in load order. Blank lines and
// lines starting with '#' are ignored, whitespace is trimmed, a name without
// ".mod" gets it appended, and gamedata.base always loads first. An empty or
// missing file means the vanilla set below (the four shipped files).
// The game's own <data>/mods.cfg is never read: it belongs to the player's
// install (the user edits it for the original game, e.g. a UI mod for
// screenshots), so a mod list is always ours and passed explicitly
// (ksim_server/ksim_run --mods FILE). See docs/ARCHITECTURE.md "The sim owns
// the load order" and docs/reference/SUPPORTED_MODS.md.
// UNSURE: the real launcher's mods.cfg grammar is not documented in this repo
// (the shipped copy was 0 bytes); this is the simplest rule that fits.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace kworld {

struct LoadOrderEntry
{
	std::string name; // file name, e.g. "rebirth.mod"
	std::string path; // where it was found
};

struct LoadOrder
{
	std::vector<LoadOrderEntry> files; // apply in this order
	std::vector<std::string> missing; // listed in mods.cfg, not found anywhere
};

// A mod folder under <install>/mods/ or the Steam workshop tree.
struct ModFolder
{
	std::string id; // <id> of its first _*.info (Workshop ID), empty if none
	std::string path;
};

// Scans <install>/mods/*/ then <steamapps>/workshop/content/233860/*/, in
// that order. install is the parent of dataDir; steamapps is the install's
// parent's parent. Folders sort by path within each root.
std::vector<ModFolder> discoverModFolders(std::string_view dataDir);

// gamedata.base, Newwworld.mod, Dialogue.mod, rebirth.mod from dataDir.
LoadOrder vanillaLoadOrder(std::string_view dataDir);

// Reads modsCfgPath; an empty path is the vanilla order (the install's own
// dataDir/mods.cfg is ignored, see above). A listed name is looked up in
// dataDir, then in each discoverModFolders() folder (first hit wins).
LoadOrder resolveLoadOrder(std::string_view dataDir, std::string_view modsCfgPath = {});

// FNV-1a 64 over the merged load order (SIMLINK.md section 2): for each file in load order,
// its name bytes, a 0 byte, the size as u64 LE, then the FNV-1a 64 of its content as u64 LE.
// The sim's HELLO and the client's must agree, so both call this one function.
std::uint64_t dataHash(const LoadOrder& order);

} // namespace kworld
