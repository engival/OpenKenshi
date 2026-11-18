// The scene light debugger's state and script ops (dev/lightterms.hpp). Built only with KCLIENT_DEV_TOOLS.
#include "dev/lightterms.hpp"

#include <algorithm>
#include <cstdio>
#include <set>

namespace dev
{
namespace
{
struct Def { const char* name; const char* group; uint32_t needs; };
constexpr uint32_t bit(LightBit b) { return 1u << b; }
constexpr uint32_t kScene = (1u << kLtBloom) - 1;	// every term but the three post ones: what a post term modifies
const Def kDefs[kLtCount] = {
	{ "sun_direct", "sun", 0 },
	{ "sun_shadows", "sun", bit(kLtSunDirect) },	// off = the sun lights everything unshadowed; solo needs the sun it shadows
	{ "sky_ambient", "ambient", 0 },
	{ "ao", "ambient", bit(kLtSkyAmbient) },	// off = no occlusion (factor 1); solo needs the ambient it darkens
	{ "post_indirect", "ambient", 0 },
	{ "sun_bounce", "ambient", 0 },
	{ "interior_light", "ambient", bit(kLtSkyAmbient) },
	{ "env_specular", "ambient", 0 },
	{ "lamps", "lamps", 0 },
	{ "emissive", "emissive", 0 },
	{ "fog_atmosphere", "fog", 0 },
	{ "fog_weather", "fog", 0 },
	{ "sky", "sky", 0 },
	{ "cut_band", "marker", 0 },	// the warm band under a cut roof (mesh.glsl): a marker the cut adds, not a light of the game
	{ "sun_shafts", "air", 0 },
	{ "dust_motes", "air", 0 },
	{ "fog_volumes", "air", 0 },
	{ "bloom", "post", kScene },
	{ "exposure", "post", kScene },
	{ "post_grade", "post", kScene },
	{ "lamp_beams", "air", 0 },
};

struct State
{
	uint32_t off = 0;
	std::set<uint64_t> lampsOff;
	std::vector<uint64_t> lamps;	// in view, nearest first
	bool lampSolo = false;		// the last solo was one lamp: the other lamps of the view are in lampsOff
};
State& st() { static State s; return s; }

std::string lampName(uint64_t id) { return "lamp:" + std::to_string(id >> 32) + "." + ((id & 0x80000000u) ? "o" : "l") + std::to_string(id & 0x7fffffffu); }

bool findLamp(const std::string& name, uint64_t& id)
{
	for (const uint64_t l : st().lamps) { if (lampName(l) == name) { id = l; return true; } }
	for (const uint64_t l : st().lampsOff) { if (lampName(l) == name) { id = l; return true; } }
	return false;
}
int findTerm(const std::string& name)
{
	for (int i = 0; i < kLtCount; i++) { if (name == kDefs[i].name) { return i; } }
	return -1;
}
}

uint32_t lightOffMask() { return st().off; }
bool lampOff(uint64_t id) { return st().lampsOff.count(id) != 0; }
void noteLamps(const std::vector<uint64_t>& ids) { st().lamps = ids; }

std::vector<LightTerm> lightTerms()
{
	std::vector<LightTerm> out;
	for (int i = 0; i < kLtCount; i++) { out.push_back({ kDefs[i].name, kDefs[i].group, !((st().off >> i) & 1u) }); }
	std::vector<uint64_t> lamps = st().lamps;
	for (const uint64_t l : st().lampsOff) { if (std::find(lamps.begin(), lamps.end(), l) == lamps.end()) { lamps.push_back(l); } }
	for (const uint64_t l : lamps) { out.push_back({ lampName(l), "lamps", !st().lampsOff.count(l) }); }
	return out;
}

bool setLight(const std::string& name, bool on)
{
	State& s = st();
	s.off &= ~bit(kLtMotesOwnLight);
	if (const int i = findTerm(name); i >= 0) { s.off = on ? s.off & ~(1u << i) : s.off | (1u << i); return true; }
	uint64_t id;
	if (!findLamp(name, id)) { return false; }
	if (on) { s.lampsOff.erase(id); } else { s.lampsOff.insert(id); }
	return true;
}

bool soloLight(const std::string& name)
{
	State& s = st();
	s.off &= ~bit(kLtMotesOwnLight);
	if (const int i = findTerm(name); i >= 0)
	{
		const uint32_t keep = (1u << i) | kDefs[i].needs;
		s.off = ((1u << kLtCount) - 1) & ~keep;
		if (i == kLtDust) { s.off |= bit(kLtMotesOwnLight); }	// the motes' light is light ON the motes: the scene stays dark, the shader reads this flag
		s.lampsOff.clear();
		return true;
	}
	uint64_t id;
	if (!findLamp(name, id)) { return false; }
	s.off = ((1u << kLtCount) - 1) & ~bit(kLtLamps);	// one lamp needs the lamps switch
	s.lampsOff.clear();
	for (const uint64_t l : s.lamps) { if (l != id) { s.lampsOff.insert(l); } }
	return true;
}

void allLights() { st().off = 0; st().lampsOff.clear(); }

std::string lightLine()
{
	std::string out;
	for (const LightTerm& t : lightTerms()) { if (!t.on) { out += (out.empty() ? "" : ", ") + t.name; } }
	return out.empty() ? "light: all on\n" : "light: off: " + out + "\n";
}

std::string lightDump()
{
	std::string out;
	for (const LightTerm& t : lightTerms()) { out += std::string(t.on ? "on  " : "off ") + t.group + " " + t.name + "\n"; }
	return out;
}

bool lightOp(const std::vector<std::string>& a, std::string& out)
{
	if (a.empty()) { out = "usage: light <name>|all on|off|solo"; return true; }
	if (a[0] == "all" && (a.size() == 1 || a[1] == "on")) { allLights(); out = "light: all on"; return true; }
	if (a.size() < 2) { out = "usage: light <name>|all on|off|solo"; return true; }
	const std::string& m = a[1];
	bool ok = false;
	if (a[0] == "all" && m == "off")
	{
		st().off = (1u << kLtCount) - 1;
		for (const uint64_t l : st().lamps) { st().lampsOff.insert(l); }
		ok = true;
	}
	else if (m == "on") { ok = setLight(a[0], true); }
	else if (m == "off") { ok = setLight(a[0], false); }
	else if (m == "solo") { ok = soloLight(a[0]); }
	else { out = "light: " + m + " is not on|off|solo"; return true; }
	out = ok ? lightLine() : "light: no term " + a[0];
	if (!out.empty() && out.back() == '\n') { out.pop_back(); }
	return true;
}
}
