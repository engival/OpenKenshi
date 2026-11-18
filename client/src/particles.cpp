#include "particles.hpp"
#include "drawstats.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

#include "dds.hpp"
#include "renderqueue.hpp"
#include "particles.glsl.h"
#include "stb_image.h"

// ParticleUniverse script subset. Public documentation of the format: the ParticleUniverse Editor / Plugin manual (Henry van Bakel, MIT). Only what the weather scripts
// use is read: Billboard renderer (point / oriented_self / oriented_common / oriented_shape, origin), Box, Circle, Point, SphereSurface and Line emitters, Colour / Scale / TextureRotator / LinearForce / Vortex / Align affectors (exclude_emitter), the OnPosition observer
// as a ground kill plane. Everything else (SineForce, Gravity, Jet, ForceField, Randomiser, beams, ribbons, light, observers' other handlers) is skipped, with the technique's other parts intact.

namespace
{

constexpr float kPi = 3.14159265f;
constexpr float kPrewarmStep = 0.05f;
constexpr float kPrewarmMax = 20.0f;
// Room for every instance the weather hands over: Venge's ~70 live sky beams at ~930 particles each want ~65 000 (kenshi-h2v.14.2.1: at 16 384 only the first
// ~17 beams fitted and the rest were dropped, so a cap below the beam count is a pop-in of its own). 3.6 MB of transient vertex data.
constexpr int kMaxInstances = 65536;
constexpr float kSoft = 60.0f;		// alpha-blended particles fade over this distance in front of the scene (the game intersects hard)
// Cap on a billboard's width / height after the Scale affector. 20000 keeps Venge's SkyBeam strip whole: 800 x the system scale 20 = 16 000 u
// tall, a column from the ground into the sky (kenshi-rwh.48 5); the old 4000 cut it to a quarter of its height.
constexpr float kMaxSize = 20000.0f;
constexpr int kInstFloats = 15;		// pos 3, size 2, dir 3, rot 1, origin 1, shape 1, colour 4
// kCoverAhead (the camera-cover distance, EFFECT types 1/5/6) and the free camera's pivot distance: Particles::kCoverAhead (particles.hpp).

// The particle record colours, the material colours and the particle textures are the game's display-referred values and are used raw, as the
// game does (particles.hlsl:15): the scene target holds the display base (k16.2.82), so a decode here would darken them.

struct Node
{
	std::vector<std::string> tok;
	std::vector<Node> kids;
};

std::vector<std::string> split(const std::string& l)
{
	std::vector<std::string> t;
	std::istringstream in(l);
	std::string w;
	while (in >> w) { t.push_back(w); }
	return t;
}

std::string trim(std::string s)
{
	const size_t c = s.find("//");
	if (c != std::string::npos) { s.erase(c); }
	const size_t a = s.find_first_not_of(" \t\r\n");
	if (a == std::string::npos) { return ""; }
	return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}

// One block of lines: "header" followed by "{" ... "}" becomes a node with kids; a bare line a leaf.
void parseLines(const std::vector<std::string>& lines, size_t& i, std::vector<Node>& out)
{
	while (i < lines.size())
	{
		const std::string& l = lines[i];
		if (l == "}") { i++; return; }
		if (l == "{" || l.empty()) { i++; continue; }
		Node n;
		n.tok = split(l);
		i++;
		if (i < lines.size() && lines[i] == "{")
		{
			i++;
			parseLines(lines, i, n.kids);
		}
		out.push_back(std::move(n));
	}
}

float num(const Node& n, size_t k, float fallback = 0) { return n.tok.size() > k ? (float)atof(n.tok[k].c_str()) : fallback; }

Particles::Dyn parseDyn(const Node& n)
{
	Particles::Dyn d;
	const std::string kind = n.tok.size() > 1 ? n.tok[1] : "";
	if (kind == "dyn_random")
	{
		d.kind = Particles::Dyn::Random;
		for (const Node& k : n.kids) { if (k.tok[0] == "min") { d.a = num(k, 1); } else if (k.tok[0] == "max") { d.b = num(k, 1); } }
	} else if (kind == "dyn_curved_linear" || kind == "dyn_curved_spline") {
		d.kind = kind == "dyn_curved_linear" ? Particles::Dyn::Linear : Particles::Dyn::Spline;
		for (const Node& k : n.kids) { if (k.tok[0] == "control_point") { d.pts.push_back({ num(k, 1), num(k, 2) }); } }
		std::sort(d.pts.begin(), d.pts.end());
		if (d.pts.empty()) { d.kind = Particles::Dyn::Fixed; }
	} else if (kind == "dyn_oscillate") {
		d.kind = Particles::Dyn::Oscillate;
		for (const Node& k : n.kids)
		{
			if (k.tok[0] == "oscillate_frequency") { d.osc[0] = num(k, 1); }
			else if (k.tok[0] == "oscillate_phase") { d.osc[1] = num(k, 1); }
			else if (k.tok[0] == "oscillate_base") { d.osc[2] = num(k, 1); }
			else if (k.tok[0] == "oscillate_amplitude") { d.osc[3] = num(k, 1); }
			else if (k.tok[0] == "oscillate_type") { d.sine = k.tok.size() < 2 || k.tok[1] != "square"; }
		}
	} else {
		d.a = num(n, 1);
	}
	return d;
}

void vec(const Node& n, float* out, int count) { for (int k = 0; k < count; k++) { out[k] = num(n, 1 + k, out[k]); } }

float rnd01(std::mt19937& g) { return (float)(g() >> 8) * (1.0f / 16777216.0f); }

// v rotated by the quaternion (w, x, y, z).
void rotateQ(const float q[4], const double v[3], double out[3])
{
	const double w = q[0], x = q[1], y = q[2], z = q[3];
	const double tx = 2 * (y * v[2] - z * v[1]), ty = 2 * (z * v[0] - x * v[2]), tz = 2 * (x * v[1] - y * v[0]);
	out[0] = v[0] + w * tx + y * tz - z * ty;
	out[1] = v[1] + w * ty + z * tx - x * tz;
	out[2] = v[2] + w * tz + x * ty - y * tx;
}

// Every .pu key the parser knows, in any block. Anything else is counted and logged once per script: the format has no schema, and a
// silently dropped keyword is a behaviour change we would never see. Known-but-not-implemented keys (billboard_rotation_type, mass, ...)
// are deliberately in this list; their port waits for a later slice.
bool knownKey(const std::string& k)
{
	static const std::set<std::string> keys = {
		"system", "category", "scale", "scale_velocity", "fast_forward", "iteration_interval", "nonvisible_update_timeout", "smooth_lod",
		"lod_distances", "keep_local", "technique", "visual_particle_quota", "default_particle_width", "default_particle_height",
		"default_particle_depth", "material", "renderer", "emitter", "affector", "observer", "enabled", "emitted_emitter_quota",
		"emitted_technique_quota", "emitted_system_quota", "billboard_type", "billboard_origin", "billboard_rotation_type", "common_direction",
		"emission_rate", "time_to_live", "velocity", "angle", "duration", "repeat_delay", "all_particle_dimensions", "particle_width",
		"particle_height", "particle_depth", "position", "direction", "box_width", "box_height", "box_depth", "radius", "colour",
		"start_colour_range", "end_colour_range", "mass", "step", "force_emission", "orientation", "start_orientation_range",
		"end_orientation_range", "auto_direction", "min_increment", "max_increment", "max_deviation", "emits", "master_technique_name",
		"master_emitter_name", "emit_random", "end", "time_colour", "colour_operation", "xyz_scale", "x_scale", "y_scale", "z_scale",
		"rotation", "rotation_speed", "force_vector", "force_application", "rotation_axis", "mass_affector", "max_deviation_x",
		"max_deviation_y", "max_deviation_z", "exclude_emitter", "use_own_rotation", "time_step", "gravity", "max_frequency",
		"min_frequency", "force", "movement", "forcefield_size", "delta", "acceleration", "resize", "octaves", "frequency", "amplitude",
		"persistence", "movement_frequency", "worldsize", "velocity_scale", "since_start_system", "ignore_negative_y", "position_x",
		"position_y", "position_z", "observe_interval", "handler", "DoExpire", "DoPlacementParticle", "DoStopSystem", "min", "max",
		"control_point", "oscillate_frequency", "oscillate_phase", "oscillate_base", "oscillate_amplitude", "oscillate_type" };
	return keys.count(k) != 0;
}

void countUnknown(const Node& n, std::map<std::string, int>& unknown)
{
	if (!n.tok.empty() && !knownKey(n.tok[0])) { unknown[n.tok[0]]++; }
	for (const Node& k : n.kids) { countUnknown(k, unknown); }
}

}

float Particles::Dyn::eval(float x, float rnd, float t) const
{
	switch (kind)
	{
	case Fixed: return a;
	case Random: return a + (b - a) * rnd;
	case Oscillate:
	{
		const float s = std::sin(2 * kPi * (osc[0] * t + osc[1]));
		return osc[2] + osc[3] * (sine ? s : (s < 0 ? -1.0f : 1.0f));
	}
	default: break;
	}
	if (x <= pts.front().first) { return pts.front().second; }
	if (x >= pts.back().first) { return pts.back().second; }
	size_t i = 1;
	while (pts[i].first < x) { i++; }
	const auto& p0 = pts[i - 1];
	const auto& p1 = pts[i];
	const float u = (x - p0.first) / std::max(1e-6f, p1.first - p0.first);
	if (kind == Linear) { return p0.second + (p1.second - p0.second) * u; }
	// Catmull-Rom through the control points (finite-difference tangents), the smooth spline of the editor's curves
	auto tangent = [&](size_t k)
	{
		const auto& lo = pts[k > 0 ? k - 1 : k];
		const auto& hi = pts[k + 1 < pts.size() ? k + 1 : k];
		return hi.first > lo.first ? (hi.second - lo.second) / (hi.first - lo.first) : 0.0f;
	};
	const float h = p1.first - p0.first, u2 = u * u, u3 = u2 * u;
	return (2 * u3 - 3 * u2 + 1) * p0.second + (u3 - 2 * u2 + u) * h * tangent(i - 1) + (-2 * u3 + 3 * u2) * p1.second + (u3 - u2) * h * tangent(i);
}

namespace
{

void readEmitter(const Node& n, Particles::Emitter& e)
{
	const std::string type = n.tok.size() > 1 ? n.tok[1] : "";
	e.kind = type == "Circle" ? Particles::Emitter::Circle : type == "Point" ? Particles::Emitter::Point
		: type == "SphereSurface" ? Particles::Emitter::SphereSurface : type == "Line" ? Particles::Emitter::Line : Particles::Emitter::Box;
	e.name = n.tok.size() > 2 ? n.tok[2] : "";
	bool hasStart = false, hasEnd = false;
	for (const Node& k : n.kids)
	{
		const std::string& key = k.tok[0];
		if (key == "emission_rate") { e.rate = parseDyn(k); }
		else if (key == "time_to_live") { e.ttl = parseDyn(k); }
		else if (key == "velocity") { e.vel = parseDyn(k); }
		else if (key == "mass") { e.mass = parseDyn(k); e.hasMass = true; }
		else if (key == "angle") { e.angle = parseDyn(k); }
		else if (key == "duration") { e.duration = parseDyn(k); e.hasDuration = true; }
		else if (key == "repeat_delay") { e.repeat = parseDyn(k); e.hasRepeat = true; }
		else if (key == "all_particle_dimensions") { e.dimW = e.dimH = parseDyn(k); e.hasDim = true; }
		else if (key == "particle_width" && !e.hasDim) { e.dimW = parseDyn(k); }
		else if (key == "particle_height" && !e.hasDim) { e.dimH = parseDyn(k); }
		else if (key == "position") { vec(k, e.pos, 3); }
		else if (key == "direction") { vec(k, e.dir, 3); }
		else if (key == "box_width") { e.box[0] = num(k, 1); }
		else if (key == "box_height") { e.box[1] = num(k, 1); }
		else if (key == "box_depth") { e.box[2] = num(k, 1); }
		else if (key == "radius") { e.radius = num(k, 1); }
		else if (key == "end") { vec(k, e.end, 3); }
		else if (key == "min_increment") { e.minInc = num(k, 1); }
		else if (key == "max_increment") { e.maxInc = num(k, 1); }
		else if (key == "max_deviation") { e.maxDev = num(k, 1); }
		else if (key == "auto_direction") { e.autoDirection = k.tok.size() > 1 && (k.tok[1] == "true" || k.tok[1] == "1"); }
		else if (key == "colour") { vec(k, e.col, 4); }
		else if (key == "start_colour_range") { vec(k, e.colStart, 4); hasStart = true; }
		else if (key == "end_colour_range") { vec(k, e.colEnd, 4); hasEnd = true; }
	}
	e.hasColourRange = hasStart || hasEnd;
	if (hasStart && !hasEnd) { for (int c = 0; c < 4; c++) { e.colEnd[c] = e.colStart[c]; } }
	if (hasEnd && !hasStart) { for (int c = 0; c < 4; c++) { e.colStart[c] = e.colEnd[c]; } }
}

void readAffector(const Node& n, Particles::Affector& a, bool& keep)
{
	const std::string& type = n.tok[1];
	keep = true;
	if (type == "Colour")
	{
		a.kind = Particles::Affector::Colour;
		for (const Node& k : n.kids)
		{
			if (k.tok[0] == "time_colour") { a.timeColour.push_back({ num(k, 1), { num(k, 2), num(k, 3), num(k, 4), num(k, 5, 1) } }); }
			else if (k.tok[0] == "colour_operation") { a.multiply = k.tok.size() > 1 && k.tok[1] == "multiply"; }
		}
		std::sort(a.timeColour.begin(), a.timeColour.end(), [](const auto& l, const auto& r) { return l.first < r.first; });
		keep = !a.timeColour.empty();
	} else if (type == "Scale") {
		a.kind = Particles::Affector::Scale;
		for (const Node& k : n.kids)
		{
			if (k.tok[0] == "xyz_scale") { a.xyz = parseDyn(k); a.hasXyz = true; }
			else if (k.tok[0] == "x_scale") { a.x = parseDyn(k); a.hasX = true; }
			else if (k.tok[0] == "y_scale") { a.y = parseDyn(k); a.hasY = true; }
			else if (k.tok[0] == "z_scale") { a.z = parseDyn(k); a.hasZ = true; }
			else if (k.tok[0] == "since_start_system") { a.sinceStartSystem = k.tok.size() > 1 && (k.tok[1] == "true" || k.tok[1] == "1"); }
		}
	} else if (type == "TextureRotator") {
		a.kind = Particles::Affector::Rotator;
		for (const Node& k : n.kids)
		{
			if (k.tok[0] == "rotation") { a.rotation = parseDyn(k); }
			else if (k.tok[0] == "rotation_speed") { a.rotSpeed = parseDyn(k); }
		}
	} else if (type == "LinearForce") {
		a.kind = Particles::Affector::Force;
		for (const Node& k : n.kids) { if (k.tok[0] == "force_vector") { vec(k, a.force, 3); } }
	} else if (type == "Gravity") {
		// PU GravityAffector (kenshi-rwh.174): pulls toward the affector's position, vel += d * (gravity * p.mass * mass_affector / |d|^2) * dt;
		// defaults gravity 1, mass_affector 1, position (0,0,0) in the technique's frame
		a.kind = Particles::Affector::Gravity;
		for (const Node& k : n.kids)
		{
			if (k.tok[0] == "gravity") { a.gravity = num(k, 1); }
			else if (k.tok[0] == "mass_affector") { a.affMass = num(k, 1); }
			else if (k.tok[0] == "position") { vec(k, a.gpos, 3); }
		}
	} else if (type == "Align") {
		a.kind = Particles::Affector::Align;
		for (const Node& k : n.kids) { if (k.tok[0] == "resize") { a.resize = k.tok.size() > 1 && (k.tok[1] == "true" || k.tok[1] == "1"); } }
	} else if (type == "Vortex") {
		// PU Vortex: rotates a particle around the axis through the affector's position (we use the system origin) at rotation_speed rad/s
		a.kind = Particles::Affector::Vortex;
		for (const Node& k : n.kids)
		{
			if (k.tok[0] == "rotation_axis") { vec(k, a.axis, 3); }
			else if (k.tok[0] == "rotation_speed") { a.vortexSpeed = parseDyn(k); }
		}
		const float l = std::sqrt(a.axis[0] * a.axis[0] + a.axis[1] * a.axis[1] + a.axis[2] * a.axis[2]);
		if (l > 1e-6f) { for (float& c : a.axis) { c /= l; } }
	} else {
		keep = false;
	}
	for (const Node& k : n.kids)
	{
		if (k.tok[0] == "enabled" && k.tok.size() > 1 && k.tok[1] == "false") { keep = false; }
		if (k.tok[0] == "exclude_emitter") { for (size_t i = 1; i < k.tok.size(); i++) { a.exclude.push_back(k.tok[i]); } }
	}
}

}

// The EFFECT `particle system` field is the .pu `system` name, not the filename: 14 shipped files differ (Peelerblood.pu is system
// peelerblood01, Volc-Pumper.pu is Volk-Cloud, Street_Torch_Flames.pu is Simple_Flames, ...). Scan the directory once for the mapping.
std::string Particles::scriptPath(const std::string& name)
{
	if (!scriptFilesScanned_)
	{
		scriptFilesScanned_ = true;
		std::error_code ec;
		for (const auto& de : std::filesystem::directory_iterator(dir_ + "/particles/scripts", ec))
		{
			if (de.path().extension() != ".pu") { continue; }
			std::ifstream in(de.path());
			for (std::string l; std::getline(in, l);)
			{
				const size_t c = l.find("//");
				if (c != std::string::npos) { l.erase(c); }
				std::istringstream is(l);
				std::string key, sys;
				is >> key >> sys;
				if (key != "system" || sys.empty()) { continue; }
				scriptFiles_.emplace(sys, de.path().string());
				break;
			}
		}
	}
	const auto it = scriptFiles_.find(name);
	return it != scriptFiles_.end() ? it->second : dir_ + "/particles/scripts/" + name + ".pu";
}

std::string Particles::inspectScript(const std::string& name)
{
	const std::shared_ptr<Script> sc = loadScript(name);
	if (!sc) { return name + ": not parsed"; }
	char buf[512];
	snprintf(buf, sizeof buf, "%-28s %d/%d techniques drawable, %d disabled  %s", name.c_str(), (int)sc->techniques.size(), sc->techniquesTotal,
		sc->techniquesDisabled, sc->file.c_str());
	return buf;
}

std::shared_ptr<Particles::Script> Particles::loadScript(const std::string& name)
{
	const auto it = scripts_.find(name);
	if (it != scripts_.end()) { return it->second; }
	std::shared_ptr<Script> sc;
	const std::string path = scriptPath(name);
	std::ifstream in(path);
	if (!in) { fprintf(stderr, "kclient: particles: no script for system %s (%s)\n", name.c_str(), path.c_str()); scripts_[name] = nullptr; return nullptr; }
	std::vector<std::string> lines;
	for (std::string l; std::getline(in, l);) { lines.push_back(trim(l)); }
	std::vector<Node> roots;
	size_t i = 0;
	parseLines(lines, i, roots);
	std::map<std::string, int> unknown;
	for (const Node& root : roots) { countUnknown(root, unknown); }
	for (const auto& [key, count] : unknown) { fprintf(stderr, "kclient: particles: %s: unknown key \"%s\" x%d\n", name.c_str(), key.c_str(), count); }
	for (const Node& root : roots)
	{
		if (root.tok[0] != "system") { continue; }
		sc = std::make_shared<Script>();
		sc->name = name;
		sc->file = path;
		for (const Node& k : root.kids)
		{
			if (k.tok[0] == "scale") { vec(k, sc->scale, 3); }
			else if (k.tok[0] == "scale_velocity") { sc->scaleVelocity = num(k, 1, 1); }
			else if (k.tok[0] == "technique")
			{
				Technique t;
				bool billboard = true, enabled = true;
				sc->techniquesTotal++;
				for (const Node& c : k.kids)
				{
					const std::string& key = c.tok[0];
					if (key == "enabled" && c.tok.size() > 1 && c.tok[1] == "false") { enabled = false; }
					if (key == "position") { vec(c, t.pos, 3); }	// the technique's own offset (Lightning_Bolt: SPARKS -0.4 -1 0)
					// keep_local: the particles live in the system's frame, so they move with a placed system (SkyBeam's strip: the original's
					// column rides the wandering handler, rwh.175 2.6). A system that never moves takes no change.
					if (key == "keep_local") { t.keepLocal = c.tok.size() > 1 && c.tok[1] == "true"; }
					if (key == "visual_particle_quota") { t.quota = (int)num(c, 1); }
					else if (key == "material") { t.material = c.tok.size() > 1 ? c.tok[1] : ""; }
					else if (key == "default_particle_width") { t.defW = num(c, 1); }
					else if (key == "default_particle_height") { t.defH = num(c, 1); }
					else if (key == "renderer")
					{
						billboard = c.tok.size() > 1 && c.tok[1] == "Billboard";
						for (const Node& r : c.kids)
						{
							if (r.tok[0] == "billboard_type" && r.tok.size() > 1)
							{
								t.oriented = r.tok[1] == "oriented_self";
								t.common = r.tok[1] == "oriented_common";
								t.shape = r.tok[1] == "oriented_shape";
							}
							else if (r.tok[0] == "billboard_origin") { t.bottomCentre = r.tok.size() > 1 && r.tok[1] == "bottom_center"; }
							else if (r.tok[0] == "common_direction") { vec(r, t.commonDir, 3); }
						}
					}
					else if (key == "emitter" && c.tok.size() > 1 && (c.tok[1] == "Box" || c.tok[1] == "Circle" || c.tok[1] == "Point" || c.tok[1] == "SphereSurface" || c.tok[1] == "Line"))
					{
						Emitter e;
						e.rate.a = 10; e.ttl.a = 3; e.vel.a = 100; e.angle.a = 20;
						e.dimW.a = e.dimH.a = -1;
						readEmitter(c, e);
						t.emitters.push_back(e);
					}
					else if (key == "affector" && c.tok.size() > 1)
					{
						Affector a;
						bool keep;
						readAffector(c, a, keep);
						if (keep) { t.affectors.push_back(a); }
						if (keep && a.kind == Affector::Align) { t.align = true; t.alignResize = a.resize; }
					}
					else if (key == "observer" && c.tok.size() > 1 && c.tok[1] == "OnPosition")
					{
						for (const Node& o : c.kids)
						{
							if (o.tok[0] == "position_y" && o.tok.size() > 2 && o.tok[1] == "less_than") { t.killBelow = true; t.killY = num(o, 2); }
						}
					}
				}
				if (!enabled) { sc->techniquesDisabled++; }
				else if (billboard && !t.emitters.empty() && !t.material.empty())
				{
					for (Affector& a : t.affectors)	// an affector's exclude_emitter names its technique's emitters
					{
						for (const std::string& nm : a.exclude)
						{
							for (size_t i = 0; i < t.emitters.size(); i++) { if (t.emitters[i].name == nm) { a.excludeIdx.push_back((int)i); } }
						}
						a.exclude.clear();
					}
					sc->techniques.push_back(std::move(t));
				}
			}
		}
		break;
	}
	if (sc && sc->techniques.empty()) { fprintf(stderr, "kclient: particles: %s has no billboard technique we can draw\n", name.c_str()); sc = nullptr; }
	scripts_[name] = sc;
	return sc;
}

const Particles::Material* Particles::material(const std::string& name)
{
	if (!materialsLoaded_)
	{
		materialsLoaded_ = true;
		std::error_code ec;
		for (const auto& de : std::filesystem::directory_iterator(dir_ + "/particles/materials", ec))
		{
			if (de.path().extension() != ".material") { continue; }
			std::ifstream in(de.path());
			Material* cur = nullptr;
			bool haveTex = false;
			for (std::string l; std::getline(in, l);)
			{
				const std::vector<std::string> t = split(trim(l));
				if (t.empty()) { continue; }
				if (t[0] == "material" && t.size() > 1) { cur = &materials_[t[1]]; *cur = Material(); haveTex = false; }
				else if (cur && t[0] == "scene_blend" && t.size() > 1) { cur->additive = t[1] == "add"; }
				else if (cur && t[0] == "vertex_program_ref" && t.size() > 1) { cur->ambient = t[1].find("Ambient") != std::string::npos; }
				else if (cur && t[0] == "fragment_program_ref" && t.size() > 1)
				{
					cur->clip = t[1].find("Clipped") != std::string::npos;
					cur->soft = t[1].find("Particle_Blend_Depth") != std::string::npos;
					// `param_named colour float4 R G B A`: the material's fragment multiplier (sparks 5x, some twisters 1.5x)
					for (size_t i = 2; i + 5 < t.size(); i++)
					{
						if (t[i] == "colour" && t[i + 1] == "float4") { for (int k = 0; k < 4; k++) { cur->colour[k] = (float)atof(t[i + 2 + k].c_str()); } break; }
					}
				}
				else if (cur && !haveTex && t[0] == "texture" && t.size() > 1) { cur->texture = t[1]; haveTex = true; }
			}
		}
	}
	const auto it = materials_.find(name);
	return it == materials_.end() || it->second.texture.empty() ? nullptr : &it->second;
}

// Logic side: the file's pixels (the material may name the other extension: PNG and DDS both ship).
Particles::TexCpu Particles::loadTexture(const std::string& name) const
{
	TexCpu t;
	std::string file = name;
	std::error_code ec;
	if (file.find('.') == std::string::npos) { file += ".dds"; }
	std::string path = dir_ + "/particles/textures/" + file;
	if (!std::filesystem::exists(path, ec))
	{
		// The material names a texture that the install names with the other extension (PNG and DDS both ship).
		const std::string stem = file.substr(0, file.rfind('.'));
		const char* const alternatives[] = { ".dds", ".png", ".tga" };
		for (const char* ext : alternatives)
		{
			if (std::filesystem::exists(dir_ + "/particles/textures/" + stem + ext, ec)) { path = dir_ + "/particles/textures/" + stem + ext; file = stem + ext; break; }
		}
	}
	std::string lower = file;
	for (char& c : lower) { c = (char)std::tolower((unsigned char)c); }
	if (lower.size() >= 4 && lower.compare(lower.size() - 4, 4, ".dds") == 0)
	{
		std::string err;
		t.dds = true;
		if (!dds_load(path, t.image, err)) { fprintf(stderr, "kclient: particles: texture %s: %s\n", name.c_str(), err.c_str()); return t; }
	} else {
		int comp = 0;
		stbi_uc* px = stbi_load(path.c_str(), &t.w, &t.h, &comp, 4);
		if (!px) { fprintf(stderr, "kclient: particles: texture %s: cannot decode %s\n", name.c_str(), path.c_str()); return t; }
		t.rgba.assign(px, px + (size_t)t.w * (size_t)t.h * 4);
		stbi_image_free(px);
	}
	t.ok = true;
	return t;
}

void Particles::requestTexture(const std::string& name)
{
	if (!requested_.insert(name).second) { return; }
	TexCpu cpu = loadTexture(name);
	const size_t bytes = cpu.rgba.size() + cpu.image.data.size();
	rq::push("particle-texture", [this, name, cpu = std::move(cpu)] { makeTexture(name, cpu); }, rq::Lane::kFrame, bytes);
}

// Render side: a failed load still gets its entry (ok false: its batches draw nothing).
void Particles::makeTexture(const std::string& name, const TexCpu& cpu)
{
	Tex& t = textures_[name];
	if (!cpu.ok) { return; }
	if (cpu.dds) { t.image = dds_make_image(cpu.image, "particle"); }
	else
	{
		sg_image_desc id = {};
		id.width = cpu.w; id.height = cpu.h; id.pixel_format = SG_PIXELFORMAT_RGBA8; id.label = "particle";
		id.data = { cpu.rgba.data(), cpu.rgba.size() };
		t.image = sg_make_image(&id);
	}
	if (t.image.id == SG_INVALID_ID || sg_query_image_state(t.image) != SG_RESOURCESTATE_VALID) { return; }
	sg_view_desc vd = {};
	vd.texture.image = t.image;
	t.view = sg_make_view(&vd);
	t.ok = true;
}

void Particles::init(const std::string& dataDir)
{
	dir_ = dataDir;
	shd_ = sg_make_shader(particles_shader_desc(sg_query_backend()));
	const float quad[8] = { -0.5f, 0, 0.5f, 0, -0.5f, 1, 0.5f, 1 };
	sg_buffer_desc bd = {};
	bd.data = SG_RANGE(quad);
	bd.label = "particle-quad";
	quad_ = sg_make_buffer(&bd);
	bd = {};
	bd.usage.vertex_buffer = true;
	bd.usage.write_transient = true;
	bd.size = (size_t)kMaxInstances * kInstFloats * sizeof(float);
	bd.label = "particle-instances";
	instBuf_ = sg_make_buffer(&bd);
	for (int additive = 0; additive < 2; additive++)
	{
		sg_pipeline_desc pd = {};
		pd.shader = shd_;
		pd.primitive_type = SG_PRIMITIVETYPE_TRIANGLE_STRIP;
		pd.layout.buffers[0].stride = 8;
		pd.layout.buffers[1].stride = kInstFloats * 4;
		pd.layout.buffers[1].step_func = SG_VERTEXSTEP_PER_INSTANCE;
		pd.layout.attrs[ATTR_particles_corner] = { .buffer_index = 0, .offset = 0, .format = SG_VERTEXFORMAT_FLOAT2 };
		pd.layout.attrs[ATTR_particles_i_pos] = { .buffer_index = 1, .offset = 0, .format = SG_VERTEXFORMAT_FLOAT3 };
		pd.layout.attrs[ATTR_particles_i_size] = { .buffer_index = 1, .offset = 12, .format = SG_VERTEXFORMAT_FLOAT2 };
		pd.layout.attrs[ATTR_particles_i_dir] = { .buffer_index = 1, .offset = 20, .format = SG_VERTEXFORMAT_FLOAT3 };
		pd.layout.attrs[ATTR_particles_i_misc] = { .buffer_index = 1, .offset = 32, .format = SG_VERTEXFORMAT_FLOAT3 };
		pd.layout.attrs[ATTR_particles_i_col] = { .buffer_index = 1, .offset = 44, .format = SG_VERTEXFORMAT_FLOAT4 };
		pd.cull_mode = SG_CULLMODE_NONE;
		pd.color_count = 1;
		pd.colors[0].pixel_format = SG_PIXELFORMAT_RGBA16F;
		pd.colors[0].blend.enabled = true;
		pd.colors[0].blend.src_factor_rgb = additive ? SG_BLENDFACTOR_ONE : SG_BLENDFACTOR_SRC_ALPHA;	// Ogre scene_blend add / alpha_blend
		pd.colors[0].blend.dst_factor_rgb = additive ? SG_BLENDFACTOR_ONE : SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
		pd.colors[0].blend.src_factor_alpha = SG_BLENDFACTOR_ZERO;
		pd.colors[0].blend.dst_factor_alpha = SG_BLENDFACTOR_ONE;
		pd.depth.pixel_format = SG_PIXELFORMAT_NONE;
		pd.sample_count = 1;
		pd.label = additive ? "particles-add" : "particles-alpha";
		(additive ? pipAdd_ : pipAlpha_) = sg_make_pipeline(&pd);
	}
	sg_sampler_desc sd = {};
	sd.min_filter = sd.mag_filter = sd.mipmap_filter = SG_FILTER_LINEAR;
	sd.wrap_u = sd.wrap_v = SG_WRAP_CLAMP_TO_EDGE;
	sd.max_anisotropy = 4;
	smp_ = sg_make_sampler(&sd);
	sd = {};
	sd.min_filter = sd.mag_filter = SG_FILTER_NEAREST;
	sd.wrap_u = sd.wrap_v = SG_WRAP_CLAMP_TO_EDGE;
	depthSmp_ = sg_make_sampler(&sd);
	// 1x1 zero interior mask (noMaskView): frames without a mask pass bind it, and a clip test against 0 never drops anything
	const uint16_t zero[2] = { 0, 0 };
	sg_image_desc zd = {};
	zd.width = zd.height = 1;
	zd.pixel_format = SG_PIXELFORMAT_RG16F;
	zd.data = { zero, sizeof zero };
	zd.label = "particle-mask-zero";
	zeroMaskImg_ = sg_make_image(&zd);
	sg_view_desc zv = {};
	zv.texture.image = zeroMaskImg_;
	zeroMask_ = sg_make_view(&zv);
}

void Particles::shutdown()
{
	for (auto& [name, t] : textures_)
	{
		if (t.ok) { sg_destroy_view(t.view); sg_destroy_image(t.image); }
	}
	textures_.clear();
	requested_.clear();
	sg_destroy_view(zeroMask_);
	sg_destroy_image(zeroMaskImg_);
	sg_destroy_sampler(depthSmp_);
	sg_destroy_sampler(smp_);
	sg_destroy_pipeline(pipAlpha_);
	sg_destroy_pipeline(pipAdd_);
	sg_destroy_buffer(instBuf_);
	sg_destroy_buffer(quad_);
	sg_destroy_shader(shd_);
}

void Particles::reset(uint32_t seed)
{
	sims_.clear();
	seed_ = seed;
	rng_.seed(seed);
	count_ = 0;
}

void Particles::setEffects(const std::vector<SkyWeather::Effect>& effects, float rainForce, float dustForce)
{
	// Drawn here: the anchored weather effects (kenshi-inspect world_effects §2). Types 1 (ash), 5 (rain) and 6 sit on the camera node + kCoverAhead
	// along the view; type 4 (sand stream/mist) is ONE handler on the player/view node, terrain-snapped and y-clamped to the EFFECT altitude range.
	// The ref's cap/respawn are ignored for these, as the exe does. Every other class (2/3/7/8/9/10) is a world instance placed by the weather
	// driver; the scene key `dust=` draws one player-anchored Sand-Stream.
	// (by type, not by name: in the merged data every rain EFFECT is type 5 and type 1 is the ash camera cover; rebirth.mod overrides the types)
	std::vector<SkyWeather::Effect> want;
	for (const SkyWeather::Effect& e : effects)
	{
		if (e.anchor() == SkyWeather::World) { continue; }
		if (e.type == 5 && rainForce >= 0) { continue; }	// rainForce replaces the weather's own rain
		want.push_back(e);
	}
	if (rainForce > 0) { SkyWeather::Effect e; e.system = "Kenshi_Heavy_Rain"; e.type = 5; e.amount = rainForce; want.push_back(e); }
	if (dustForce > 0) { SkyWeather::Effect e; e.system = "Sand-Stream"; e.type = 4; e.amount = dustForce; e.windAffected = true; want.push_back(e); }
	std::vector<Sim> next;
	for (const SkyWeather::Effect& e : want)
	{
		if (e.amount < 0.002f) { continue; }
		auto it = std::find_if(sims_.begin(), sims_.end(), [&](const Sim& s) { return !s.placed && s.name == e.system; });
		if (it != sims_.end())
		{
			it->amount = e.amount;
			it->windAffected = e.windAffected;
			it->anchor = (int)e.anchor();
			it->minAlt = e.minAltitude;
			it->maxAlt = e.maxAltitude;
			next.push_back(std::move(*it));
			sims_.erase(it);
			continue;
		}
		Sim s;
		s.script = loadScript(e.system);
		if (!s.script) { continue; }
		s.name = e.system;
		s.amount = e.amount;
		s.windAffected = e.windAffected;
		s.anchor = (int)e.anchor();
		s.minAlt = e.minAltitude;
		s.maxAlt = e.maxAltitude;
		s.pools.resize(s.script->techniques.size());
		s.em.resize(s.script->techniques.size());
		for (size_t t = 0; t < s.script->techniques.size(); t++) { s.em[t].resize(s.script->techniques[t].emitters.size()); }
		// The type 4 ParticleWrapExtern radius (rwh.81 §3): 1.5 x the largest emitter extent (box side, circle radius) x the script scale.
		const float sc = std::max({ s.script->scale[0], s.script->scale[1], s.script->scale[2] });
		s.wrapR.resize(s.script->techniques.size());
		for (size_t t = 0; t < s.script->techniques.size(); t++)
		{
			float ext = 0;
			for (const Emitter& E : s.script->techniques[t].emitters)
			{
				ext = std::max(ext, E.kind == Emitter::Point ? 0.0f : E.kind == Emitter::Line ? std::sqrt(E.end[0] * E.end[0] + E.end[1] * E.end[1] + E.end[2] * E.end[2]) : E.kind == Emitter::Circle || E.kind == Emitter::SphereSurface ? E.radius
					: std::max({ E.box[0], E.box[1], E.box[2] }));
			}
			s.wrapR[t] = 1.5f * ext * sc;
		}
		s.warm = true;
		next.push_back(std::move(s));
	}
	for (Sim& s : sims_) { if (s.placed) { next.push_back(std::move(s)); } }	// the world systems are ParticleWorld's
	sims_ = std::move(next);
}

// The placed systems of this frame (ParticleWorld). Diffed by key: a new key spawns a pre-warmed system with its own seed, a surviving one
// keeps its particles and only takes the fresh fade, a missing one is dropped (the driver fades it out before removing it).
void Particles::setPlaced(const std::vector<PlacedSystem>& systems)
{
	std::vector<Sim> next;
	for (const PlacedSystem& p : systems)
	{
		Sim s;
		bool found = false;
		for (Sim& old : sims_)
		{
			if (!old.placed || old.key != p.key) { continue; }
			s = std::move(old);	// leaves old.script null: not re-added below
			found = true;
			break;
		}
		if (!found)
		{
			s.script = loadScript(p.script);
			if (!s.script) { continue; }
			s.name = p.script;
			s.placed = true;
			s.key = p.key;
			s.rng.seed(p.seed);
			s.pools.resize(s.script->techniques.size());
			s.em.resize(s.script->techniques.size());
			for (size_t t = 0; t < s.script->techniques.size(); t++) { s.em[t].resize(s.script->techniques[t].emitters.size()); }
			s.prewarm = p.prewarm;
			s.warm = true;
		}
		// keep_local: a surviving system that moved carries its techniques' particles with it (their positions are in the system's
		// frame). The beams' strip rides the wandering foot this way (rwh.175 2.6); the foot fire (no keep_local) stays where it burned.
		double dp[3] = { p.pos[0] - s.pos[0], p.pos[1] - s.pos[1], p.pos[2] - s.pos[2] };
		if (found && s.script && (dp[0] != 0.0 || dp[1] != 0.0 || dp[2] != 0.0))
		{
			for (size_t ti = 0; ti < s.pools.size() && ti < s.script->techniques.size(); ti++)
			{
				if (!s.script->techniques[ti].keepLocal) { continue; }
				for (Particle& q : s.pools[ti]) { for (int k = 0; k < 3; k++) { q.pos[k] += dp[k]; } }
			}
		}
		for (int k = 0; k < 3; k++) { s.pos[k] = p.pos[k]; }
		for (int k = 0; k < 4; k++) { s.quat[k] = p.quat[k]; }
		s.fade = p.fade;
		s.nodeScale = p.nodeScale;
		s.amount = p.amount;
		s.emitting = true;
		s.outlive = p.outlive;
		next.push_back(std::move(s));
	}
	for (Sim& s : sims_)
	{
		if (!s.placed && s.script) { next.push_back(std::move(s)); continue; }
		// A psys whose instance left the list but whose particles are not run out yet keeps going with emission off (the exe's teardown:
		// the handler goes, the stop ends emission, the system is destroyed when the last particle dies). Only an event marked `outlive`
		// does this: a weather-driven system is handed over every frame while it lives, so leaving early means its driver dropped it.
		if (s.placed && s.script && s.outlive && s.hasParticles())
		{
			s.emitting = false;
			next.push_back(std::move(s));
		}
	}
	sims_ = std::move(next);
}

void Particles::emit(Sim& s, int ti, int ei, const Frame& f, float dt)
{
	if (!s.emitting) { return; }	// the instance is gone (its TTL): the psys only runs its particles out (PlacedSystem::outlive)
	const Technique& T = s.script->techniques[ti];
	std::mt19937& g = s.placed ? s.rng : rng_;	// a placed system owns its RNG (stable per placement); the weather keeps the shared one
	// the emitter's origin: the camera node + kCoverAhead along the view (types 1/5/6), or the player/view node, clamped to the EFFECT altitude
	// range (type 4). MINE: the clamp reads the altitude fields as world y (the exe's terrain snap is not traced).
	double org[3];
	if (s.anchor == (int)SkyWeather::Player)
	{
		// the exe's type 4 handler rides the pivot at y = terrain + 100 (rwh.81 §3), clamped into the EFFECT's altitude range when it has one
		for (int k = 0; k < 3; k++) { org[k] = f.anchor[k]; }
		org[1] += 100.0;
		if (s.minAlt > 0 && org[1] < s.minAlt) { org[1] = s.minAlt; }
		// MINE: the max-altitude clamp only when it is a ceiling above the pivot. Taken literally it would put the Fog Islands mist (max altitude
		// 600) below the 1500-unit terrain there and out of sight; the traced Desert-dust-swirls "handler at y 1000 indoors" (max 1000, terrain
		// below that) still clamps.
		if (s.maxAlt > 0 && s.maxAlt >= f.anchor[1] && org[1] > s.maxAlt) { org[1] = s.maxAlt; }
	}
	else { for (int k = 0; k < 3; k++) { org[k] = f.cam[k] + f.view[k] * kCoverAhead; } }
	const Emitter& E = T.emitters[ei];
	Sim::EmitState& st = s.em[ti][ei];
	std::vector<Particle>& pool = s.pools[ti];
	if (E.hasDuration)
	{
		// PU starts an emitter on its `duration` (sparks: 10 s on, 2 s repeat_delay; furnace smoke: 5 on,
		// 4-8 off); the first phase is on at spawn, not a repeat pause. Without a `repeat_delay` the duration is the whole
		// emission: the emitter runs it once and is done (rwh.175 2.6: SkyBeam's 9 s foot fire stops ~11 s into a 10-30 s
		// handler; a repetition would have kept its psys, and so its fire, alive to the handler's end).
		if (!st.started)
		{
			st.started = true;
			st.on = true;
			st.phaseStart = s.time;
			st.phaseEnd = s.time + std::max(0.01, (double)E.duration.sample(rnd01(g), (float)s.time));
		}
		if (s.time >= st.phaseEnd)
		{
			if (!E.hasRepeat) { return; }
			while (s.time >= st.phaseEnd)
			{
				st.on = !st.on;
				st.phaseStart = s.time;
				st.phaseEnd = s.time + std::max(0.01, (double)(st.on ? E.duration : E.repeat).sample(rnd01(g), (float)s.time));
			}
			if (!st.on) { return; }
		}
	}
	st.acc += (double)E.rate.sample(rnd01(g), (float)s.time) * s.amount * dt;
	// The script's own (system) scale. The owning node's scale never enters the particle math: emitter offsets, velocities and forces are
	// script units in the node's frame at scale 1 (kenshi-rwh.155 Q1/Q3: BDC's 1.1-scaled post puts the emitter at 40.0 u, not 44).
	const float sc[3] = { s.script->scale[0], s.script->scale[1], s.script->scale[2] };
	while (st.acc >= 1)
	{
		st.acc -= 1;
		if ((int)pool.size() >= T.quota) { st.acc = 0; break; }
		Particle p = {};
		// the emitter's local position: the technique's own `position` + the emitter's, x the system scale (SPARKS sits -200 u under the bolt base)
		float lp[3] = { (T.pos[0] + E.pos[0]) * sc[0], (T.pos[1] + E.pos[1]) * sc[1], (T.pos[2] + E.pos[2]) * sc[2] };
		float surf[3] = { 0, 1, 0 };
		if (E.kind == Emitter::Circle)
		{
			const float a = rnd01(g) * 2 * kPi, r = std::sqrt(rnd01(g)) * E.radius * sc[0];
			lp[0] += std::cos(a) * r;
			lp[2] += std::sin(a) * r;
		} else if (E.kind == Emitter::Box) {
			for (int k = 0; k < 3; k++) { lp[k] += (rnd01(g) - 0.5f) * E.box[k] * sc[k]; }
		} else if (E.kind == Emitter::SphereSurface) {
			// PU SphereSurface: three RangeRandom(-1, 1) values normalised to the unit sphere; the offset is normal x scale x radius
			// along the emitter orientation (applied to the whole system), and the emission direction is that normal, deviated by `angle`.
			surf[0] = rnd01(g) * 2 - 1; surf[1] = rnd01(g) * 2 - 1; surf[2] = rnd01(g) * 2 - 1;
			const float sl = std::sqrt(surf[0] * surf[0] + surf[1] * surf[1] + surf[2] * surf[2]);
			if (sl > 1e-6f) { for (int k = 0; k < 3; k++) { surf[k] /= sl; } }
			else { surf[0] = 0; surf[1] = 1; surf[2] = 0; }
			for (int k = 0; k < 3; k++) { lp[k] += surf[k] * E.radius * sc[k]; }
		}
		if (E.kind == Emitter::Line)
		{
			// PU LineEmitter: the next point of the segment (start = the emitter position, end = `end`), a random step further than the last one; past
			// the end it starts over. Each point is shaken by up to max_deviation on every axis (MINE: uniform per axis, the exe's draw is not traced).
			const float len = std::sqrt(E.end[0] * E.end[0] + E.end[1] * E.end[1] + E.end[2] * E.end[2]);
			if (len > 1e-6f)
			{
				const float f = (float)st.lineLen / len;
				for (int k = 0; k < 3; k++) { lp[k] += E.end[k] * f * sc[k]; }
				st.lineLen += E.minInc + (E.maxInc - E.minInc) * rnd01(g);
				if (st.lineLen > len) { st.lineLen = 0; }
			}
			for (int k = 0; k < 3; k++) { lp[k] += (rnd01(g) * 2 - 1) * E.maxDev * sc[k]; }
		}
		// direction: the base emitter's `direction` (default +y) deviated by up to `angle` degrees; a SphereSurface only uses the surface normal
		// when the script sets `auto_direction` (PU SphereSurfaceEmitter::_initParticleDirection delegates to the base class otherwise).
		const bool normalDir = E.kind == Emitter::SphereSurface && E.autoDirection;
		float d[3] = { normalDir ? surf[0] : E.dir[0], normalDir ? surf[1] : E.dir[1], normalDir ? surf[2] : E.dir[2] };
		float dl = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
		float speed = E.vel.sample(rnd01(g), 0) * s.script->scaleVelocity;
		if (dl < 1e-6f) { d[0] = 0; d[1] = 1; d[2] = 0; dl = 1; speed = std::fabs(speed) * 0.0f; }
		for (float& v : d) { v /= dl; }
		const float dev = E.angle.sample(rnd01(g), 0) * kPi / 180.0f * (E.angle.kind == Dyn::Fixed ? rnd01(g) : 1.0f), az = rnd01(g) * 2 * kPi;
		float t1[3] = { d[1], -d[0], 0 };	// any vector perpendicular to d
		float tl = std::sqrt(t1[0] * t1[0] + t1[1] * t1[1]);
		if (tl < 1e-4f) { t1[0] = 1; t1[1] = 0; t1[2] = 0; tl = 1; }
		for (float& v : t1) { v /= tl; }
		const float t2[3] = { d[1] * t1[2] - d[2] * t1[1], d[2] * t1[0] - d[0] * t1[2], d[0] * t1[1] - d[1] * t1[0] };
		for (int k = 0; k < 3; k++) { p.vel[k] = speed * (d[k] * std::cos(dev) + std::sin(dev) * (std::cos(az) * t1[k] + std::sin(az) * t2[k])); }
		if (s.placed)
		{
			// A world effect: the emitter's local position and direction go through the effect instance's rotation and origin.
			const double l[3] = { lp[0], lp[1], lp[2] }, v[3] = { p.vel[0], p.vel[1], p.vel[2] };
			double wp[3], wv[3];
			rotateQ(s.quat, l, wp);
			rotateQ(s.quat, v, wv);
			for (int k = 0; k < 3; k++) { p.pos[k] = s.pos[k] + wp[k]; p.vel[k] = (float)wv[k]; }
		}
		else { for (int k = 0; k < 3; k++) { p.pos[k] = org[k] + lp[k]; } }
		p.ttl = std::max(0.01f, E.ttl.sample(rnd01(g), 0));
		const float rr = rnd01(g);
		// The emitted dimensions: a dyn value is read at the emitter's own phase time within its `duration` (PU), not a random abscissa; an
		// emitter without a dimension dyn falls back to the technique's default size.
		const float dimU = E.hasDuration ? std::clamp((float)((s.time - st.phaseStart) / std::max(1e-6, st.phaseEnd - st.phaseStart)), 0.0f, 1.0f) : rr;
		const auto dimAt = [&](const Dyn& d, float def) {
			if (d.a < 0 && d.kind == Dyn::Fixed) { return def; }
			return d.eval(d.maxX() * dimU, rr, (float)s.time);
		};
		p.w = dimAt(E.dimW, T.defW) * sc[0];
		p.h = dimAt(E.dimH, T.defH) * sc[1];

		for (int k = 0; k < 4; k++) { p.col[k] = E.col[k]; }
		if (E.hasColourRange)
		{
			// PU: each channel is an independent uniform in [start, end] per particle; not one lerp factor.
			for (int k = 0; k < 4; k++) { p.col[k] = E.colStart[k] + (E.colEnd[k] - E.colStart[k]) * rnd01(g); }
		}
		p.emitter = ei;
		for (const Affector& a : T.affectors)
		{
			if (a.kind == Affector::Rotator) { p.rot = a.rotation.sample(rnd01(g), 0); p.rotSpeed = a.rotSpeed.sample(rnd01(g), 0); }
			else if (a.kind == Affector::Vortex && std::find(a.excludeIdx.begin(), a.excludeIdx.end(), ei) == a.excludeIdx.end())
			{
				p.vortex = a.vortexSpeed.sample(rnd01(g), 0);	// PU: per particle, sampled at emission
			}
		}
		p.mass = E.hasMass ? E.mass.sample(rnd01(g), 0) : 1.0f;
		p.age = rnd01(g) * dt;	// emitted somewhere inside the step: no pulsing
		for (int k = 0; k < 3; k++) { p.pos[k] += p.vel[k] * p.age; }
		pool.push_back(p);
	}
}

void Particles::step(Sim& s, const Frame& f, float dt)
{
	std::mt19937& g = s.placed ? s.rng : rng_;	// a placed system owns its RNG (stable per placement); the weather keeps the shared one
	// The type 4 handler's pivot movement this step: the ParticleWrapExtern pushes a straying particle along it (rwh.81 §3), not along the wind.
	if (s.anchor == (int)SkyWeather::Player)
	{
		const double d[3] = { f.anchor[0] - s.prevAnchor[0], f.anchor[1] - s.prevAnchor[1], f.anchor[2] - s.prevAnchor[2] };
		const double len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
		s.wrapDirValid = s.hasPrev && len > 1e-3;
		if (s.wrapDirValid) { for (int k = 0; k < 3; k++) { s.wrapDir[k] = d[k] / len; } }
		for (int k = 0; k < 3; k++) { s.prevAnchor[k] = f.anchor[k]; }
		s.hasPrev = true;
	}
	// the type 4 handler's position this frame (the wrap sphere's centre): the pivot, y = terrain + 100, clamped as in emit()
	double horg[3] = { f.anchor[0], f.anchor[1], f.anchor[2] };
	if (s.anchor == (int)SkyWeather::Player)
	{
		horg[1] += 100.0;
		if (s.minAlt > 0 && horg[1] < s.minAlt) { horg[1] = s.minAlt; }
		if (s.maxAlt > 0 && s.maxAlt >= f.anchor[1] && horg[1] > s.maxAlt) { horg[1] = s.maxAlt; }
	}
	for (size_t ti = 0; ti < s.script->techniques.size(); ti++)
	{
		const Technique& T = s.script->techniques[ti];
		std::vector<Particle>& pool = s.pools[ti];
		// Gravity affector world positions, once per technique per frame (PU GravityAffector::_preProcessParticles, kenshi-rwh.174 §3):
		// psysOrient * (affector `position` * affectorScale) + technique derived position, where the latter is
		// psysOrient * (technique `position` * system scale) + psys position. The affector's own `scale` is unit in every .pu here,
		// and the position escape flag (affector+0x94) is default 0, so the derived path is what the data uses.
		std::vector<std::array<double, 3>> gravWorld;
		for (const Affector& a : T.affectors)
		{
			if (a.kind != Affector::Gravity) { continue; }
			const double lp[3] = { (double)T.pos[0] * s.script->scale[0] + a.gpos[0],
			                       (double)T.pos[1] * s.script->scale[1] + a.gpos[1],
			                       (double)T.pos[2] * s.script->scale[2] + a.gpos[2] };
			std::array<double, 3> gp = {};
			if (s.placed)
			{
				double r[3];
				rotateQ(s.quat, lp, r);
				for (int k = 0; k < 3; k++) { gp[k] = s.pos[k] + r[k]; }
			}
			else { for (int k = 0; k < 3; k++) { gp[k] = f.anchor[k] + lp[k]; } }	// MINE: no weather effect uses Gravity in the data
			gravWorld.push_back(gp);
		}
		size_t w = 0;
		for (size_t i = 0; i < pool.size(); i++)
		{
			Particle p = pool[i];
			p.age += dt;
			if (p.age >= p.ttl) { continue; }
			for (const Affector& a : T.affectors)
			{
				if (a.kind != Affector::Force) { continue; }
				// An additive acceleration in the node's frame (rwh.155 Q1: |v| = v0 + 10 t along the node's up for a placed system).
				float fv[3] = { a.force[0], a.force[1], a.force[2] };
				if (s.placed)
				{
					const double l[3] = { fv[0], fv[1], fv[2] };
					double w3[3];
					rotateQ(s.quat, l, w3);
					for (int k = 0; k < 3; k++) { fv[k] = (float)w3[k]; }
				}
				for (int k = 0; k < 3; k++) { p.vel[k] += fv[k] * dt; }
			}
			size_t gi = 0;
			for (const Affector& a : T.affectors)
			{
				if (a.kind != Affector::Gravity) { continue; }
				// PU GravityAffector::_affect (kenshi-rwh.174 §2): d = affectorWorld - p.pos; skip only when d.d <= 0 (NaN too);
				// vel += d * (timeScale * gravity * p.mass * mass_affector / d.d) * dt * specialisationFactor. There is no epsilon,
				// cut-off radius or clamp: the magnitude grows as k/r. timeScale and the specialisation factor are 1 (no .pu script
				// sets time_scale or affect_specialisation), and p.mass is the emitter's `mass` sampled at birth (default 1).
				const std::array<double, 3>& O = gravWorld[gi++];
				double dv[3];
				for (int k = 0; k < 3; k++) { dv[k] = O[k] - p.pos[k]; }
				const double l2 = dv[0] * dv[0] + dv[1] * dv[1] + dv[2] * dv[2];
				if (!(l2 > 0.0)) { continue; }
				const double fo = (double)a.gravity * p.mass * a.affMass / l2;
				for (int k = 0; k < 3; k++) { p.vel[k] += (float)(dv[k] * fo * dt); }
			}
			for (const Affector& a : T.affectors)
			{
				if (a.kind != Affector::Scale) { continue; }
				// PU: each update adds scale(t) * dt * affectorScale to the particle's dimensions; affectorScale is the system's derived scale, the
				// script's top-level scale (never the building node's: rwh.155 Q2/Q3); t is the particle's time fraction
				// (age / ttl), or the system's time in seconds when `since_start_system` is set.
				const float t = a.sinceStartSystem ? (float)s.time : p.age / std::max(1e-6f, p.ttl);
				const float xyz = a.hasXyz ? a.xyz.eval(t, 0, (float)s.time) : 0.0f;
				p.w = std::fmax(0.0f, p.w + (xyz + (a.hasX ? a.x.eval(t, 0, (float)s.time) : 0.0f)) * s.script->scale[0] * dt);
				p.h = std::fmax(0.0f, p.h + (xyz + (a.hasY ? a.y.eval(t, 0, (float)s.time) : 0.0f)) * s.script->scale[1] * dt);
			}
			for (int k = 0; k < 3; k++) { p.pos[k] += p.vel[k] * dt; }
			if (s.windAffected) { p.pos[0] += f.wind[0] * dt; p.pos[2] += f.wind[1] * dt; }
			// PU Vortex (the twister's swirl): the particle's offset from the system origin and its velocity are rotated about the axis by speed x dt.
			for (const Affector& a : T.affectors)
			{
				if (a.kind != Affector::Vortex || p.vortex == 0) { continue; }
				if (std::find(a.excludeIdx.begin(), a.excludeIdx.end(), p.emitter) != a.excludeIdx.end()) { continue; }
				const double th = (double)p.vortex * dt, cs = std::cos(th), sn = std::sin(th);
				const double* O = s.placed ? s.pos : f.anchor;
				double v[3] = { p.pos[0] - O[0], p.pos[1] - O[1], p.pos[2] - O[2] };
				const double d = a.axis[0] * v[0] + a.axis[1] * v[1] + a.axis[2] * v[2];
				const double cr[3] = { a.axis[1] * v[2] - a.axis[2] * v[1], a.axis[2] * v[0] - a.axis[0] * v[2], a.axis[0] * v[1] - a.axis[1] * v[0] };
				for (int k = 0; k < 3; k++) { v[k] = v[k] * cs + cr[k] * sn + a.axis[k] * d * (1 - cs); p.pos[k] = O[k] + v[k]; }
				double u[3] = { p.vel[0], p.vel[1], p.vel[2] };
				const double du = a.axis[0] * u[0] + a.axis[1] * u[1] + a.axis[2] * u[2];
				const double uc[3] = { a.axis[1] * u[2] - a.axis[2] * u[1], a.axis[2] * u[0] - a.axis[0] * u[2], a.axis[0] * u[1] - a.axis[1] * u[0] };
				for (int k = 0; k < 3; k++) { u[k] = u[k] * cs + uc[k] * sn + a.axis[k] * du * (1 - cs); p.vel[k] = (float)u[k]; }
			}
			// type 4: a particle past the volume's radius (from the handler) is moved by 1.5 r along the pivot's movement and, if still
			// outside, re-randomised inside it; y is held under the EFFECT's max altitude. r = 1.5 x the largest emitter extent x script scale.
			const float wr = ti < s.wrapR.size() ? s.wrapR[ti] : 0.0f;
			if (s.anchor == (int)SkyWeather::Player && wr > 0)
			{
				const auto outside = [&](const Particle& q) {
					const double dx = q.pos[0] - horg[0], dy = q.pos[1] - horg[1], dz = q.pos[2] - horg[2];
					return dx * dx + dy * dy + dz * dz >= (double)wr * (double)wr;
				};
				if (outside(p))
				{
					if (s.wrapDirValid) { for (int k = 0; k < 3; k++) { p.pos[k] += s.wrapDir[k] * (1.5 * wr); } }
					if (outside(p))
					{
						double n[3] = { rnd01(g) * 2 - 1, rnd01(g) * 2 - 1, rnd01(g) * 2 - 1 };
						const double l = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
						if (l > 1e-6) { for (int k = 0; k < 3; k++) { n[k] /= l; } }
						else { n[0] = 0; n[1] = 1; n[2] = 0; }
						const double r = wr * std::cbrt((double)rnd01(g));
						for (int k = 0; k < 3; k++) { p.pos[k] = horg[k] + n[k] * r; }
					}
					if (s.maxAlt > 0 && p.pos[1] > s.maxAlt) { p.pos[1] = s.maxAlt; }
				}
			}
			p.rot += p.rotSpeed * dt;
			if (T.killBelow && p.pos[1] < f.groundY + T.killY) { continue; }
			pool[w++] = p;
		}
		pool.resize(w);
		// Emit after the expiry: a full pool hands the expired slots straight back, the request is cut to the free list (rwh.155 Q4: ~420/s
		// sustained from a 2000/s script, quota 300 full).
		for (size_t ei = 0; ei < T.emitters.size(); ei++) { emit(s, (int)ti, (int)ei, f, dt); }
	}
	s.time += dt;
}

int Particles::prepare(const Frame& f, FrameData& out)
{
	for (Sim& s : sims_)
	{
		if (s.warm)
		{
			s.warm = false;
			if (!s.prewarm) { continue; }	// an event system: first update is its t = 0
			float life = 1;
			for (const Technique& T : s.script->techniques)
			{
				for (const Emitter& E : T.emitters) { life = std::max(life, E.ttl.kind == Dyn::Random ? E.ttl.b : E.ttl.a); }
			}
			const float total = s.placed ? 1.0f : std::min(kPrewarmMax, life * 1.2f);	// placed: ~1 s of fixed 0.05 s steps
			for (float t = 0; t < total; t += kPrewarmStep) { step(s, f, kPrewarmStep); }
		}
		if (f.dt > 0) { step(s, f, f.dt); }
	}
	out.clear();
	out.depthTest = depthTest_;
	std::vector<float>& inst = out.inst;
	const float amb = std::fmin(std::fmax(5.0f * std::fmax(f.sunY, 0.0f) + 0.2f, 0.1f), 1.0f);
	// UNSURE (kenshi-k16.2.54.4, oracle rwh.169 mechanism U: the tint's source is unknown): the Sand-Wisp vertex colour takes a tint after the
	// Colour Affector map. The oracle traced x0.40 neutral at night (01:11, where the traced ambient factor a = clamp(5 sunY + 0.2, 0.1, 1) is 0.2)
	// and ~x1.2 warm at noon (a = 1); the refs do not support x1.2 (riveroverlook1 18:29 and grass_canyon_dust 12:20 get brighter than their refs),
	// so the day end is ~x0.85. The curve between is UNSURE (endpoints only): linear in the same `amb`, 0.40 at a = 0.2 to 0.85 at a = 0.5 and flat
	// above (fit: riveroverlook1 band luma err 5.0, night_moons ratio 0.78). dust_red_dawn (sun below the horizon, a = 0.2 like the night) stays at
	// 0.40, which its ref does not like (rmse 14.6 vs 13.1 untinted): `a` cannot tell dawn from 01:11, the discriminator is not traced.
	const float sandT = 0.40f + 0.45f * std::fmin(std::fmax((amb - 0.2f) / 0.3f, 0.0f), 1.0f);
	const float sandTint[3] = { sandT, sandT, sandT };
	std::vector<std::pair<float, const Particle*>> order;
	for (Sim& s : sims_)
	{
		for (size_t ti = 0; ti < s.script->techniques.size(); ti++)
		{
			const Technique& T = s.script->techniques[ti];
			const Material* m = material(T.material);
			const std::vector<Particle>& pool = s.pools[ti];
			if (!m || pool.empty()) { continue; }
			// The material colour multiplies every particle's authored (display-referred) colour, additive and alpha-blended alike, raw.
			float mcr[3] = { m->colour[0], m->colour[1], m->colour[2] };
			if (T.material == "Sand-Wisp") { for (int k = 0; k < 3; k++) { mcr[k] *= sandTint[k]; } }
			if (inst.size() / kInstFloats + pool.size() > (size_t)kMaxInstances) { break; }
			order.clear();
			for (const Particle& p : pool)
			{
				const double dx = p.pos[0] - f.cam[0], dy = p.pos[1] - f.cam[1], dz = p.pos[2] - f.cam[2];
				order.push_back({ (float)(dx * dx + dy * dy + dz * dz), &p });
			}
			if (!m->additive) { std::sort(order.begin(), order.end(), [](const auto& l, const auto& r) { return l.first > r.first; }); }
			int texIdx = 0;
			while (texIdx < (int)out.texNames.size() && out.texNames[texIdx] != m->texture) { texIdx++; }
			if (texIdx == (int)out.texNames.size()) { out.texNames.push_back(m->texture); requestTexture(m->texture); }
			Batch b = { texIdx, m->additive, m->clip, (int)(inst.size() / kInstFloats), (int)order.size(), m->additive ? 0.0f : kSoft };
			for (const auto& [dist, pp] : order)
			{
				const Particle& p = *pp;
				const float life = p.age / p.ttl;
				float col[4] = { p.col[0], p.col[1], p.col[2], p.col[3] };
				for (const Affector& a : T.affectors)
				{
					if (a.kind == Affector::Colour)
					{
						const auto& tc = a.timeColour;
						size_t i = 1;
						while (i < tc.size() && tc[i].first < life) { i++; }
						float c[4];
						for (int k = 0; k < 4; k++)
						{
							if (life <= tc.front().first || tc.size() == 1) { c[k] = tc.front().second[k]; }
							else if (i >= tc.size()) { c[k] = tc.back().second[k]; }
							else { c[k] = tc[i - 1].second[k] + (tc[i].second[k] - tc[i - 1].second[k]) * (life - tc[i - 1].first) / std::max(1e-6f, tc[i].first - tc[i - 1].first); }
						}
						for (int k = 0; k < 4; k++) { col[k] = a.multiply ? col[k] * c[k] : c[k]; }
					}
				}
				float dir[3] = {};
				float pw = p.w, ph = p.h;
				if (T.shape || T.align)
				{
					// Align: the particle's long axis points at the previous particle of the pool (the one emitted before it); `resize` sets the height to
					// that distance. The first particle has no neighbour and keeps the world up axis.
					dir[1] = 1;
					if (T.align && &p != pool.data())
					{
						const Particle& q = *(&p - 1);
						const double dv[3] = { q.pos[0] - p.pos[0], q.pos[1] - p.pos[1], q.pos[2] - p.pos[2] };
						const double dl = std::sqrt(dv[0] * dv[0] + dv[1] * dv[1] + dv[2] * dv[2]);
						if (dl > 1e-4) { for (int k = 0; k < 3; k++) { dir[k] = (float)(dv[k] / dl); } if (T.alignResize) { ph = (float)dl; } }
					}
				}
				else if (T.oriented)
				{
					const float vl = std::sqrt(p.vel[0] * p.vel[0] + p.vel[1] * p.vel[1] + p.vel[2] * p.vel[2]);
					if (vl > 1e-4f) { for (int k = 0; k < 3; k++) { dir[k] = p.vel[k] / vl; } }
				}
				else if (T.common)
				{
					// oriented_common (Lightning_Bolt: 0 1 0): the technique's fixed world axis, the shader turns the quad around it to face the camera
					const float cl = std::sqrt(T.commonDir[0] * T.commonDir[0] + T.commonDir[1] * T.commonDir[1] + T.commonDir[2] * T.commonDir[2]);
					if (cl > 1e-4f) { for (int k = 0; k < 3; k++) { dir[k] = T.commonDir[k] / cl; } }
				}
				// Per-material lighting: Basic_Coloured_Ambient_VP takes the sun factor, Basic_Coloured_Texture_VP (flames) does not. The
				// driver's fade multiplies the colour (additive blends by RGB, alpha blends by alpha).
				const float lit = (m->ambient ? amb : 1.0f) * f.exposure * s.fade;
				const float row[kInstFloats] = {
					(float)(p.pos[0] - f.cam[0]), (float)(p.pos[1] - f.cam[1]), (float)(p.pos[2] - f.cam[2]),
					std::min(pw, kMaxSize), std::min(ph, kMaxSize),
					dir[0], dir[1], dir[2], p.rot, T.bottomCentre ? 0.0f : 0.5f, T.shape ? 1.0f : 0.0f,
					col[0] * lit * mcr[0], col[1] * lit * mcr[1], col[2] * lit * mcr[2], col[3] * s.fade * m->colour[3] };
				inst.insert(inst.end(), row, row + kInstFloats);
			}
			out.batches.push_back(b);
		}
	}
	out.count = (int)(inst.size() / kInstFloats);
	probe_ = inst;
	count_ = out.count;
	return count_;
}

void Particles::upload(const FrameData& d)
{
	resolved_.clear();
	static const Tex kNone;	// not made (cannot happen: prepare queued the create before this packet's draw); draws nothing
	for (const std::string& n : d.texNames)
	{
		const auto it = textures_.find(n);
		resolved_.push_back(it == textures_.end() ? &kNone : &it->second);
	}
	if (d.count == 0) { return; }
	sg_write_buffer_desc wd = {};
	wd.src.data = { d.inst.data(), d.inst.size() * sizeof(float) };
	wd.dst.buffer = instBuf_;
	sg_write_buffer_transient(&wd);
}

void Particles::render(const FrameData& d, const Camera& c, sg_view depth, sg_view mask)
{
	if (d.count == 0) { return; }
	part_vs_params_t vp = {};
	c.mvp(vp.mvp);
	for (int k = 0; k < 3; k++) { vp.cam_r[k] = (float)c.r[k]; vp.cam_u[k] = (float)c.u[k]; }
	sg_pipeline cur = {};
	for (const Batch& b : d.batches)
	{
		const Tex* tex = resolved_[(size_t)b.tex];
		if (!tex->ok) { continue; }	// the texture failed to load: the batch draws nothing
		const sg_pipeline pip = b.additive ? pipAdd_ : pipAlpha_;
		if (pip.id != cur.id) { drawstats::pipeline(pip); cur = pip; }
		sg_bindings bd = {};
		bd.vertex_buffers[0] = quad_;
		bd.vertex_buffers[1] = instBuf_;
		bd.vertex_buffer_offsets[1] = b.first * kInstFloats * (int)sizeof(float);
		bd.views[VIEW_depth_tex] = depth;
		bd.views[VIEW_part_tex] = tex->view;
		bd.views[VIEW_mask_tex] = mask;
		bd.samplers[SMP_depth_smp] = depthSmp_;
		bd.samplers[SMP_part_smp] = smp_;
		bd.samplers[SMP_mask_smp] = depthSmp_;	// nearest, like the game's mask sample (Nearest)
		drawstats::bindings(&bd);
		sg_apply_uniforms(UB_part_vs_params, SG_RANGE(vp));
		part_fs_params_t fp = {};
		fp.zinfo[0] = c.nearZ;
		fp.zinfo[1] = c.farZ;
#if defined(SOKOL_VULKAN) || defined(SOKOL_METAL)	// z clip 0..1 (GL: -1..1)
		fp.zinfo[2] = 1;
#endif
		fp.zinfo[3] = b.soft > 0 ? 1.0f / b.soft : 0.0f;
		fp.res[0] = (float)c.width;
		fp.res[1] = (float)c.height;
		fp.misc[0] = b.clip ? 1.0f : 0.0f;
		fp.misc[1] = d.depthTest ? 1.0f : 0.0f;
		sg_apply_uniforms(UB_part_fs_params, SG_RANGE(fp));
		drawstats::draw(0, 4, b.count);
	}
}

// Dev probe (KCLIENT_DEV_TOOLS `probe depth`): which of the last built instances sits under a pixel (the front-most covering quad, else the
// nearest centre), with its v_w. Mirrors part_vs's projection: rows are camera relative, the view is rotation-only, the projection is
// Camera::mvp's (t = 1 / tanY, x divided by the aspect). px / pyTop are pixels from the top-left, the screenshot's origin.
Particles::PixelHit Particles::probePixel(const Camera& c, int px, int pyTop) const
{
	PixelHit hit;
	if (c.width <= 0 || c.height <= 0 || c.tanY <= 0) { return hit; }
	const double t = 1.0 / (double)c.tanY;
	PixelHit nearest;
	double nearestD = 1e30, coveredW = 1e30;
	const std::vector<float>& inst = probe_;
	for (size_t i = 0; i + (size_t)kInstFloats <= inst.size(); i += (size_t)kInstFloats)
	{
		const float* r = &inst[i];
		const double fwd = (double)r[0] * c.f[0] + (double)r[1] * c.f[1] + (double)r[2] * c.f[2];
		if (fwd <= 1e-3) { continue; }
		const double vx = (double)r[0] * c.r[0] + (double)r[1] * c.r[1] + (double)r[2] * c.r[2];
		const double vy = (double)r[0] * c.u[0] + (double)r[1] * c.u[1] + (double)r[2] * c.u[2] + (0.5 - (double)r[9]) * (double)r[4];
		const double sx = ((vx * t / c.aspect / fwd) * 0.5 + 0.5) * c.width;
		const double sy = (1.0 - ((vy * t / fwd) * 0.5 + 0.5)) * c.height;
		const double hw = (double)r[3] * 0.5 * t / c.aspect / fwd * c.width * 0.5;
		const double hh = (double)r[4] * t / fwd * c.height * 0.5;
		const double cs = std::fabs(std::cos((double)r[8])), sn = std::fabs(std::sin((double)r[8]));
		const double ex = hw * cs + hh * sn, ey = hw * sn + hh * cs;	// the rotated quad's half extents
		const double dx = std::fabs(px + 0.5 - sx), dy = std::fabs(pyTop + 0.5 - sy);
		const auto fill = [&](PixelHit& h, bool covers) {
			h.any = true;
			h.covers = covers;
			for (int k = 0; k < 3; k++) { h.world[k] = c.pos[k] + (double)r[k]; }
			h.w = (float)fwd;
			h.sx = (float)sx;
			h.sy = (float)sy;
			h.halfW = (float)ex;
			h.halfH = (float)ey;
		};
		if (dx <= ex && dy <= ey)
		{
			if (fwd < coveredW) { coveredW = fwd; fill(hit, true); }
		}
		else
		{
			const double d = std::hypot(dx, dy);
			if (d < nearestD) { nearestD = d; fill(nearest, false); }
		}
	}
	return hit.any ? hit : nearest;
}

// The interior mask is worth rendering while a CLIP_INTERIOR material is in play (41 particle materials: Basic_Texture_Clipped_FP_HLSL). Checked on the
// last update's systems, so a system that starts this frame gets its mask one frame later - the mask target cleared to 0 never clips in the gap.
bool Particles::needsInteriorMask()
{
	for (Sim& s : sims_)
	{
		if (!s.script) { continue; }
		for (const Technique& T : s.script->techniques)
		{
			const Material* m = material(T.material);
			if (m && m->clip) { return true; }
		}
	}
	return false;
}

std::string Particles::describe() const
{
	std::string out;
	char buf[96];
	for (const Sim& s : sims_)
	{
		size_t n = 0;
		for (const auto& p : s.pools) { n += p.size(); }
		snprintf(buf, sizeof buf, "%s%s%s x%.2f %zu", out.empty() ? "" : ", ", s.placed ? "world " : "", s.name.c_str(), s.amount, n);
		out += buf;
	}
	return out.empty() ? "none" : out;
}

std::vector<Particles::PlacedInfo> Particles::placedInfo() const
{
	std::vector<PlacedInfo> out;
	for (const Sim& s : sims_)
	{
		if (!s.placed || !s.script) { continue; }
		PlacedInfo pi;
		pi.key = s.key;
		pi.script = s.name;
		for (int k = 0; k < 3; k++) { pi.pos[k] = s.pos[k]; }
		pi.time = s.time;
		pi.fade = s.fade;
		for (size_t ti = 0; ti < s.pools.size() && ti < s.script->techniques.size(); ti++)
		{
			const std::vector<Particle>& pool = s.pools[ti];
			PlacedTech t;
			t.material = s.script->techniques[ti].material;
			t.count = pool.size();
			if (!pool.empty())
			{
				double c[3] = {};
				for (const Particle& p : pool) { for (int k = 0; k < 3; k++) { c[k] += p.pos[k]; } }
				for (int k = 0; k < 3; k++) { t.centroid[k] = c[k] / (double)pool.size() - s.pos[k]; }
				for (const Particle& p : pool)
				{
					const double dx = p.pos[0] - s.pos[0], dy = p.pos[1] - s.pos[1], dz = p.pos[2] - s.pos[2];
					t.far = std::max(t.far, std::sqrt(dx * dx + dy * dy + dz * dz));
				}
			}
			pi.techs.push_back(std::move(t));
		}
		out.push_back(std::move(pi));
	}
	return out;
}

std::string Particles::placedReport() const
{
	std::string out;
	char buf[256];
	for (const PlacedInfo& p : placedInfo())
	{
		snprintf(buf, sizeof buf, "placed %016llx \"%s\" pos %.1f %.1f %.1f time %.2f fade %.2f\n",
			(unsigned long long)p.key, p.script.c_str(), p.pos[0], p.pos[1], p.pos[2], p.time, p.fade);
		out += buf;
		for (size_t ti = 0; ti < p.techs.size(); ti++)
		{
			const PlacedTech& t = p.techs[ti];
			snprintf(buf, sizeof buf, "  tech %zu \"%s\" n %zu centroid %+.1f %+.1f %+.1f far %.1f\n",
				ti, t.material.c_str(), t.count, t.centroid[0], t.centroid[1], t.centroid[2], t.far);
			out += buf;
		}
	}
	return out;
}
