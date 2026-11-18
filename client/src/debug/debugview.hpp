#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

// The sim's DEBUG VIEW (docs/formats/SIMLINK.md "DEBUG VIEW", frames 200-219; built only with KCLIENT_DEBUG_VIEW): the AI fields of every
// squad in SQUADS, and one query's tree. Decodes what SimView hands its sink and writes the two control frames; draws nothing itself.
namespace dbg
{

struct Squad	// DEBUG_SQUADS, one squad (raw sim enums; names: ksim/debugview/enums.hpp, goalKindName etc.)
{
	uint32_t templ = 0, package = 0;	// strids
	uint8_t goalKind = 0, fallbackReason = 0, orderStatus = 0, detail = 0;
	uint8_t members = 0, effective = 0, dead = 0, intact = 0;
	uint8_t targetKind = 0;	// 0 none, 1 point, 2 town, 3 squad, 4 character
	float tx = 0, tz = 0;
	uint64_t targetId = 0;
	uint32_t townSid = 0;	// strid
	bool routeValid = false;
	float along = 0, length = 0;
	std::vector<uint64_t> memberIds;
};

struct TreeRow { int depth; std::string key, value; };

struct Tree	// DEBUG_TREE: one query's answer
{
	uint64_t tick = 0, id = 0;
	uint8_t kind = 0;	// 0 squad, 1 character
	std::vector<TreeRow> rows;
};

class DebugView
{
public:
	// The sink for SimView::setDebugSink (DEBUG_SQUADS names stay strids: resolve them with the decoder's str())
	void frame(uint16_t type, const uint8_t* p, size_t n);
	static std::vector<uint8_t> subscribe(bool on);	// DEBUG_SUBSCRIBE (210)
	static std::vector<uint8_t> query(uint8_t kind, uint64_t id);	// DEBUG_QUERY (211)
	// DEBUG_SPAWN_SQUAD (212): a wilderness squad of `templ` ("" = the sim's default, Dust Bandits) at (sx, sz), raiding the town near (tx, tz)
	static std::vector<uint8_t> spawnSquad(uint32_t seq, const std::string& templ, float sx, float sz, float tx, float tz);
	static std::vector<uint8_t> setWeather(uint32_t seq, const std::string& sid, const std::string& group, float strength);	// sid "": release; group "": the player's
	static std::vector<uint8_t> setTime(uint32_t seq, float hour, uint8_t extraDays);	// forward to the next `hour`, + extraDays
	// DEBUG_CHEAT (216): op 0 heal, 1 knock out, 2 wake, 3 kill on `character` (0 = the session's first player character)
	static std::vector<uint8_t> cheat(uint32_t seq, uint64_t character, uint8_t op);
	void reset();	// a new connection or a load: nothing known

	bool infoSeen = false, enabled = false;	// DEBUG_INFO: the server answered, and its debug view is on for us
	uint8_t version = 0;
	uint32_t infoSerial = 0;	// bumped per DEBUG_INFO
	uint64_t squadsTick = 0;
	uint32_t squadsSerial = 0, treeSerial = 0;	// bumped per DEBUG_SQUADS / DEBUG_TREE
	std::unordered_map<uint64_t, Squad> squads;	// the last DEBUG_SQUADS, by squad id
	Tree tree;	// the last DEBUG_TREE
	struct Reply { uint32_t seq = 0; uint16_t frame = 0; bool accepted = false; uint64_t squad = 0; std::string reason; };
	Reply reply;	// the last answer to a debug command (DEBUG_SPAWNED 203: frame = 212; DEBUG_ACK 204: the command's frame)
	uint32_t replySerial = 0;	// bumped per answer
	std::string error;	// a malformed frame (the first one), else ""
};

void goalColour(uint8_t goalKind, float rgb[3]);	// the overlay's and the tree window's colour key

} // namespace dbg
