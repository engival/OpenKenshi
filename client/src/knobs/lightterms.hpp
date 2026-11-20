#pragma once
// The scene light debugger's host half (kenshi-jj3.27.1; docs/CLIENT.md "Light terms"): one on/off switch per light term of the frame, as a bit mask that rides
// in the shared light block (shaders/lighting.inc, pl_head.w) and the frame packet. Dev state of this run, never saved. The UI window comes after, on this API.
//
// The mask holds the terms that are OFF (bit set = off), so a zeroed LightParams (portraits, tools) and every release build read "all on". The logic thread owns the
// state (script ops, the sidecar, the future window); it copies the mask into the packet (FramePacket::lightOff, SkyFrame's light block) and the render thread reads
// only that copy. Release builds (KCLIENT_DEV_TOOLS off) compile the mask to 0 and the lamp list to nothing.
#include <cstdint>
#include <string>
#include <vector>

namespace dev
{
// The bits of the mask; the order is the table of docs/CLIENT.md "Light terms". 0..13 are read by the shaders (lighting.inc / sky.glsl / mesh.glsl), 14..19 by the render passes.
enum LightBit : uint32_t
{
	kLtSunDirect = 0, kLtSunShadows, kLtSkyAmbient, kLtAo, kLtIndirect, kLtSunBounce, kLtInterior, kLtEnvSpec, kLtLamps, kLtEmissive, kLtFogAtmos, kLtFogWeather, kLtSky, kLtCutBand,
	kLtSunShafts, kLtDust, kLtFogVolumes, kLtBloom, kLtExposure, kLtGrade, kLtLampBeams,
	kLtCount,
	// Not a term (no switch, not in lightTerms): set by `solo dust_motes` only. The motes' own light: the sun beam and the lamps' cones light the motes as the dust shader
	// samples them, though sun_direct / lamps / lamp_beams are off for the scene (kenshi-jj3.8.14). Any other op clears it.
	kLtMotesOwnLight = 21
};

struct LightTerm
{
	std::string name;	// "sun_direct", ..., "lamp:<id>"
	std::string group;	// sun, ambient, lamps, emissive, fog, sky, air, post
	bool on = true;
};

#ifdef KCLIENT_DEV_TOOLS
std::vector<LightTerm> lightTerms();	// every term, then one "lamp:<id>" row per lamp of the last frame's view (nearest first)
bool setLight(const std::string& name, bool on);	// false: no such term
bool soloLight(const std::string& name);	// only this term and what it needs to show (docs/CLIENT.md); false: no such term
void allLights();	// every term on again, every lamp too
uint32_t lightOffMask();	// bits of the terms that are off (kLt*)
bool lampOff(uint64_t id);	// WorldBuildings::packLights drops an off lamp
void noteLamps(const std::vector<uint64_t>& ids);	// WorldBuildings::packLights: the lamps in view this frame (logic side)
std::string lightLine();	// the sidecar line: "light: all on" or "light: off: a, b"
bool lightOp(const std::vector<std::string>& args, std::string& out);	// `light <name>|all on|off|solo`; out = the reply / error. false: not a light op
std::string lightDump();	// `dump light`: the table, one "on|off group name" line each
#else
inline uint32_t lightOffMask() { return 0; }
inline bool lampOff(uint64_t) { return false; }
inline void noteLamps(const std::vector<uint64_t>&) {}
inline std::string lightLine() { return {}; }
inline bool lightOp(const std::vector<std::string>&, std::string&) { return false; }
inline std::string lightDump() { return {}; }
#endif

inline bool lightOff(uint32_t mask, LightBit b) { return (mask >> b) & 1u; }
}
