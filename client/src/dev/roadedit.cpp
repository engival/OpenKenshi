// The road editor, step 1: see roadedit.hpp.
#include "dev/roadedit.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <exception>
#include <memory>
#include <map>
#include <unordered_map>
#include <vector>

#include "app/config.hpp"
#include "kworld/heightmap.hpp"
#include "kworld/roads.hpp"
#include "render/overlaylist.hpp"
#include "render/roadribbons.hpp"

namespace dev::roadedit
{
namespace
{
const cfg::Bool kRoads{ cfg::Group{ "Road editor" }, "debug.roads", false, "Debug (kenshi-ix9r): the original's road network drawn on the terrain (a line through each road's markers, a handle on each marker, an orange diamond on each road's two junction ends), the suspect markers (legs over twice the chord of their neighbours and a turn over 140 degrees) in red; debug.road_next / debug.road_prev fly the camera to the next / previous suspect, the marker under the cursor is described in the debug text" };

const cfg::Bool kRibbon{ cfg::Group{ "Road editor" }, "debug.road_ribbon", true, "Debug (kenshi-y8ni): the road overlay draws real-width ribbons draped on the terrain with colour (debug.road_color: direction, gradient or distance; suspect segments outlined in red, parts hidden behind terrain dimmed and dashed, faded out toward the range edge); off draws the thin 1-px lines of kenshi-ix9r" };

const cfg::Text kColor{ cfg::Group{ "Road editor" }, "debug.road_color", "direction", "Debug (kenshi-5t9w): how the road overlay colours a segment. direction (default): its compass heading on a colour wheel (legend in the corner), each segment a half-width strip from its centre line out to its right-hand side, so roads of opposite directions fill opposite sides and a same-direction duplicate is hatched; gradient: start junction green to end violet; distance: one colour, darker with distance. Suspect segments are outlined in red in every mode. Changing it rebuilds the strips" };

constexpr float kLift = 1.5f;	// over the terrain
// The road width in u. kworld::RoadNetwork::Road has no width yet: 100 u is the median of the data. Its loader gains the width with kenshi-k16.6.711 and
// step 2 (kenshi-wsm5) switches to it; until then every road is drawn this wide (the debug line says "width: data n/a"). Never a guessed per-road width.
constexpr double kRoadWidth = 100.0;
constexpr double kStep = 25.0;	// u: the ribbon mesh samples the terrain this often
constexpr int kBudget = 40000;	// this overlay's share of the overlay context's vertices (k16.19.6.1: the context holds 56000, the navmesh and collision overlays share it)
constexpr double kHandleRange = 4000.0;	// u from the camera: handles drawn
constexpr double kRatioMin = 2.0, kTurnMin = 140.0;	// outliers.py

constexpr double kTouch = 1.0;	// u: two markers this close are one point; a marker this close to another road's segment touches that road (kenshi-tqtd item 8)

struct Suspect
{
	int road = 0, k = 0;
	double ratio = 0, turn = 0, extra = 0;
	const char* kind = "reversal";	// "reversal", "overlap" (repeated points / stacked roads), "T-junction"
};

struct State
{
	bool tried = false, ok = false;
	std::string error;
	kworld::RoadNetwork net;
	std::vector<Suspect> suspects;
	size_t markers = 0;
	std::vector<std::vector<float>> cum;	// per road, the length along it at each marker
	std::map<int, int> degree;	// junction id -> the roads that start or end there
	std::unordered_map<int64_t, std::vector<std::pair<int, int>>> mgrid, sgrid;	// 64 u cells: the markers; the segments (by their sampled points)
	int sel = -1;	// index into suspects (-1: the selection is not a suspect, or none)
	int selRoad = -1, selK = -1;	// the selected marker, any marker
	int hoverRoad = -1, hoverK = -1;
	std::shared_ptr<const roadribbons::Mesh> mesh;	// built at the first draw and when debug.road_color changes (buildMesh)
	int meshMode = -1;
	std::map<std::pair<int, int>, int> marks;	// (road, marker) -> ui::RoadMark, in memory (step 2 saves them as the fixup table's mark rows)
	ui::RoadsPanelState ps;
	bool psDirty = true;
	float mvp[16] = {};
	double cam[3] = {};
	float w = 0, h = 0;
	const kworld::Heightmap* hm = nullptr;
};
State g;

double hd(const kworld::RoadNetwork::Marker& a, const kworld::RoadNetwork::Marker& b) { return std::hypot(a.x - b.x, a.z - b.z); }

int64_t cellKey(int cx, int cz) { return (int64_t)cx * 1000003 + cz; }
int cellOf(double v) { return (int)std::floor(v / 64.0); }

// One road through a marker: another road whose marker lies within kTouch of it (other >= 0: that marker's index) or whose segment does (other -1).
struct Touch
{
	int road, other;
};

std::vector<Touch> touching(int road, int k)
{
	const auto& roads = g.net.roads();
	const auto& m = roads[road].markers[k];
	std::map<int, int> best;	// other road -> the marker (>= 0) or -1
	const int cx = cellOf(m.x), cz = cellOf(m.z);
	for (int dx = -1; dx <= 1; dx++)
	{
		for (int dz = -1; dz <= 1; dz++)
		{
			const auto im = g.mgrid.find(cellKey(cx + dx, cz + dz));
			if (im != g.mgrid.end())
			{
				for (const auto& [r2, k2] : im->second) { if (r2 != road && hd(m, roads[r2].markers[k2]) < kTouch) { best[r2] = k2; } }
			}
			const auto is = g.sgrid.find(cellKey(cx + dx, cz + dz));
			if (is == g.sgrid.end()) { continue; }
			for (const auto& [r2, k2] : is->second)
			{
				if (r2 == road || best.count(r2) && best[r2] >= 0) { continue; }
				const auto &a = roads[r2].markers[k2], &b = roads[r2].markers[k2 + 1];
				const double sx = b.x - a.x, sz = b.z - a.z, l2 = sx * sx + sz * sz;
				const double t = l2 > 0 ? std::clamp(((m.x - a.x) * sx + (m.z - a.z) * sz) / l2, 0.0, 1.0) : 0.0;
				if (std::hypot(m.x - (a.x + sx * t), m.z - (a.z + sz * t)) < kTouch) { best[r2] = -1; }
			}
		}
	}
	std::vector<Touch> out;
	for (const auto& [r2, k2] : best) { out.push_back({ r2, k2 }); }
	return out;
}

std::string roadText(int road)
{
	const auto& r = g.net.roads()[road];
	return "#" + std::to_string(road) + " (junctions " + std::to_string(r.start) + "-" + std::to_string(r.end) + ")";
}

// Every road through the marker, this one first ("" when it is alone).
std::string throughText(int road, int k)
{
	const auto t = touching(road, k);
	if (t.empty()) { return ""; }
	std::string s = roadText(road);
	for (const Touch& x : t) { s += ", " + roadText(x.road) + (x.other >= 0 ? " #" + std::to_string(x.other) : " segment"); }
	return s;
}

// An interior marker of both roads shared by two roads (a road end meeting a junction is no overlap).
bool sharedInterior(int road, int k)
{
	const auto& roads = g.net.roads();
	if (k <= 0 || k + 1 >= (int)roads[road].markers.size()) { return false; }
	for (const Touch& t : touching(road, k)) { if (t.other > 0 && t.other + 1 < (int)roads[t.road].markers.size()) { return true; } }
	return false;
}

const char* classify(int road, int k)
{
	for (const Touch& t : touching(road, k)) { if (t.other >= 0) { return "overlap"; } }
	if (!touching(road, k).empty()) { return "T-junction"; }
	if (sharedInterior(road, k - 1) || sharedInterior(road, k + 1)) { return "overlap"; }
	return "reversal";
}

void load(const std::string& dataDir)
{
	if (g.tried) { return; }
	g.tried = true;
	try { g.net = kworld::RoadNetwork::load(dataDir); }
	catch (const std::exception& e) { g.error = e.what(); fprintf(stderr, "roads: cannot load the road network from %s: %s\n", dataDir.c_str(), e.what()); return; }
	g.ok = true;
	const auto& roads = g.net.roads();
	for (size_t i = 0; i < roads.size(); i++)
	{
		const auto& m = roads[i].markers;
		g.markers += m.size();
		g.cum.emplace_back(m.size(), 0.0f);
		g.degree[roads[i].start]++;
		g.degree[roads[i].end]++;
		for (size_t k = 0; k < m.size(); k++)
		{
			g.mgrid[cellKey(cellOf(m[k].x), cellOf(m[k].z))].emplace_back((int)i, (int)k);
			if (k + 1 == m.size()) { continue; }
			const double len = hd(m[k], m[k + 1]);
			const int n = std::max(1, (int)(len / 32.0));
			std::vector<int64_t> seen;
			for (int j = 0; j <= n; j++)
			{
				const double u = (double)j / n;
				const int64_t key = cellKey(cellOf(m[k].x + (m[k + 1].x - m[k].x) * u), cellOf(m[k].z + (m[k + 1].z - m[k].z) * u));
				if (std::find(seen.begin(), seen.end(), key) == seen.end()) { seen.push_back(key); g.sgrid[key].emplace_back((int)i, (int)k); }
			}
		}
		for (size_t k = 1; k < m.size(); k++) { g.cum.back()[k] = g.cum.back()[k - 1] + (float)hd(m[k - 1], m[k]); }
		for (size_t k = 1; k + 1 < m.size(); k++)
		{
			const double l1 = hd(m[k - 1], m[k]), l2 = hd(m[k], m[k + 1]), ch = hd(m[k - 1], m[k + 1]);
			if (l1 == 0 || l2 == 0) { continue; }
			const double cosv = ((m[k - 1].x - m[k].x) * (m[k + 1].x - m[k].x) + (m[k - 1].z - m[k].z) * (m[k + 1].z - m[k].z)) / (l1 * l2);
			const double turn = 180.0 - std::acos(std::clamp(cosv, -1.0, 1.0)) * 180.0 / 3.14159265358979;	// 0 straight, 180 a full reversal
			const double ratio = (l1 + l2) / std::max(ch, 1.0);
			if (ratio > kRatioMin && turn > kTurnMin) { g.suspects.push_back({ (int)i, (int)k, ratio, turn, l1 + l2 - ch }); }
		}
	}
	for (Suspect& s : g.suspects) { s.kind = classify(s.road, s.k); }
	// the bent-marker reversals first (the worst on top), then the overlaps and T-junctions: kept apart, never ranked among the reversals
	std::sort(g.suspects.begin(), g.suspects.end(), [](const Suspect& a, const Suspect& b)
	{
		const bool ra = std::string(a.kind) == "reversal", rb = std::string(b.kind) == "reversal";
		return ra != rb ? ra : a.extra > b.extra;
	});
	printf("roads: %zu roads, %zu markers, %zu suspects\n", roads.size(), g.markers, g.suspects.size());
}

bool project(const float mvp[16], const double cam[3], float w, float h, double x, double y, double z, float& sx, float& sy)
{
	const float v[3] = { (float)(x - cam[0]), (float)(y - cam[1]), (float)(z - cam[2]) };
	float c[4];
	for (int i = 0; i < 4; i++) { c[i] = mvp[i] * v[0] + mvp[4 + i] * v[1] + mvp[8 + i] * v[2] + mvp[12 + i]; }
	if (c[3] < 1.0f) { return false; }
	sx = (c[0] / c[3] * 0.5f + 0.5f) * w;
	sy = (0.5f - c[1] / c[3] * 0.5f) * h;
	return true;
}

float smooth(double a, double b, double x) { const double t = std::clamp((x - a) / (b - a), 0.0, 1.0); return (float)(t * t * (3 - 2 * t)); }

// Pixels per world unit at a point (the projection's focal scale over the clip depth).
float pxPerUnit(const float mvp[16], const double cam[3], float h, double x, double y, double z)
{
	const float f = std::sqrt(mvp[1] * mvp[1] + mvp[5] * mvp[5] + mvp[9] * mvp[9]);
	const float v[3] = { (float)(x - cam[0]), (float)(y - cam[1]), (float)(z - cam[2]) };
	const float w = mvp[3] * v[0] + mvp[7] * v[1] + mvp[11] * v[2] + mvp[15];
	return w < 1.0f ? 0.0f : 0.5f * h * f / w;
}

// Direction colour: the compass heading (0 north = -z, 90 east = +x) as a hue on the full wheel at full saturation.
void headingRgb(double dx, double dz, float& r, float& gg, float& b)
{
	const double deg = std::fmod(std::atan2(dx, -dz) * 57.29577951308232 + 360.0, 360.0), h6 = deg / 60.0;
	const int i = (int)h6 % 6;
	const float f = (float)(h6 - std::floor(h6)), q = 1.0f - f;
	const float v[6][3] = { { 1, f, 0 }, { q, 1, 0 }, { 0, 1, f }, { 0, q, 1 }, { f, 0, 1 }, { 1, 0, q } };
	r = v[i][0]; gg = v[i][1]; b = v[i][2];
}

int colorMode()
{
	const std::string s = kColor.get();
	return s == "gradient" ? 1 : s == "distance" ? 2 : 0;
}

// The whole network as strips, once (kenshi-qjzb): the terrain does not change, so nothing here runs per frame. Each segment is cut every kStep u and sits on the terrain (the
// lift, the width floor, the fade and the dark are the shader's). A segment is a HALF-width strip from its centre line out to its right-hand side (kenshi-5t9w, right-hand
// traffic seen from above): two opposite roads on one stretch fill both halves, an overshoot-and-back shows on one side then the other, and a same-direction duplicate of an
// earlier segment is hatched. Colour by mode: direction = the segment's heading on the colour wheel; gradient = start junction green to end violet; distance = one neutral
// colour (the shader darkens it with distance). A suspect keeps its colour and gets a red outline along its strip's two long edges.
std::shared_ptr<const roadribbons::Mesh> buildMesh(const kworld::Heightmap& hm, int mode)
{
	auto mesh = std::make_shared<roadribbons::Mesh>();
	const auto& roads = g.net.roads();
	std::vector<std::pair<int, int>> susp;
	for (const Suspect& s : g.suspects) { susp.push_back({ s.road, s.k - 1 }); susp.push_back({ s.road, s.k }); }
	std::sort(susp.begin(), susp.end());
	const float hw = (float)(kRoadWidth * 0.5);	// the strip: half of the road width, centre line to the right
	const float ol = 7.0f;	// the suspect outline's width

	// same-direction duplicates: a segment is one when an earlier segment runs the same way (within ~37 degrees), its centre line within 0.3 road widths of this midpoint,
	// and the midpoint falls inside its extent
	struct Seg { double ax, az, tx, tz, len; };
	std::vector<Seg> segs;
	std::unordered_map<int64_t, std::vector<int>> cells;
	const auto cellOf = [](double x, double z) { return ((int64_t)(int32_t)std::floor(x / 128.0) << 32) ^ (int64_t)(uint32_t)(int32_t)std::floor(z / 128.0); };
	std::vector<std::vector<char>> dup(roads.size());
	for (size_t i = 0; i < roads.size(); i++)
	{
		const auto& m = roads[i].markers;
		dup[i].assign(m.size(), 0);
		for (size_t k = 0; k + 1 < m.size(); k++)
		{
			const double len = hd(m[k], m[k + 1]);
			if (len <= 0) { continue; }
			const Seg s{ m[k].x, m[k].z, (m[k + 1].x - m[k].x) / len, (m[k + 1].z - m[k].z) / len, len };
			const double mx = (m[k].x + m[k + 1].x) * 0.5, mz = (m[k].z + m[k + 1].z) * 0.5;
			bool d = false;
			for (int cx = -1; cx <= 1 && !d; cx++)
			{
				for (int cz = -1; cz <= 1 && !d; cz++)
				{
					const auto it = cells.find(cellOf(mx + cx * 128.0, mz + cz * 128.0));
					if (it == cells.end()) { continue; }
					for (int e : it->second)
					{
						const Seg& o = segs[e];
						if (o.tx * s.tx + o.tz * s.tz < 0.8) { continue; }
						const double rx = mx - o.ax, rz = mz - o.az, al = rx * o.tx + rz * o.tz, lat = -rx * o.tz + rz * o.tx;
						if (al > -10.0 && al < o.len + 10.0 && std::fabs(lat) < kRoadWidth * 0.3) { d = true; break; }
					}
				}
			}
			dup[i][k] = d;
			const int e = (int)segs.size();
			segs.push_back(s);
			for (double t = 0; t < len; t += 64.0) { cells[cellOf(s.ax + s.tx * t, s.az + s.tz * t)].push_back(e); }
			cells[cellOf(m[k + 1].x, m[k + 1].z)].push_back(e);
		}
	}

	struct Strip { float a, b, r, g, bl, hatch; };
	for (size_t i = 0; i < roads.size(); i++)
	{
		const auto& m = roads[i].markers;
		const double total = std::max((double)g.cum[i].back(), 1.0);
		for (size_t k = 0; k + 1 < m.size(); k++)
		{
			const double len = hd(m[k], m[k + 1]);
			if (len <= 0) { continue; }
			const double tx = (m[k + 1].x - m[k].x) / len, tz = (m[k + 1].z - m[k].z) / len;
			const float nx = (float)-tz, nz = (float)tx;	// the right-hand side seen from above, north = -z
			const bool isSusp = std::binary_search(susp.begin(), susp.end(), std::make_pair((int)i, (int)k));
			float dr, dgr, dbl;
			headingRgb(tx, tz, dr, dgr, dbl);
			const int n = std::max(1, (int)std::ceil(len / kStep));
			for (int si = 0; si < (isSusp ? 3 : 1); si++)	// the body, then the suspect's two outline strips
			{
				for (int j = 0; j <= n; j++)
				{
					const double u = (double)j / n, x = m[k].x + (m[k + 1].x - m[k].x) * u, z = m[k].z + (m[k + 1].z - m[k].z) * u;
					const double along = g.cum[i][k] + len * u, al = along / total;
					Strip st;
					if (si == 0)
					{
						st = { 0.0f, hw, 0, 0, 0, dup[i][k] ? 1.0f : 0.0f };
						if (mode == 0) { st.r = dr; st.g = dgr; st.bl = dbl; }
						else if (mode == 1) { st.r = 0.15f + 0.4f * (float)al; st.g = 0.95f - 0.6f * (float)al; st.bl = 0.5f + 0.5f * (float)al; }
						else { st.r = 0.3f; st.g = 0.85f; st.bl = 0.9f; }
					}
					else { st = si == 1 ? Strip{ 0.0f, ol, 1.0f, 0.1f, 0.1f, 0.0f } : Strip{ hw - ol, hw, 1.0f, 0.1f, 0.1f, 0.0f }; }
					const uint32_t first = (uint32_t)(mesh->verts.size() / 12);
					for (int e = 0; e < 2; e++)	// inner edge, outer edge
					{
						const float off = e == 0 ? st.a : st.b;
						const double ex = x + nx * off, ez = z + nz * off;
						const float v[12] = { (float)ex, (float)hm.height(ex, ez), (float)ez, nx, nz, hw, st.r, st.g, st.bl, (float)along, off, st.hatch };
						mesh->verts.insert(mesh->verts.end(), v, v + 12);
					}
					if (j > 0)
					{
						const uint32_t a = first - 2;
						for (uint32_t q : { a, a + 1, a + 3, a, a + 3, a + 2 }) { mesh->idx.push_back(q); }
					}
				}
			}
		}
	}
	printf("roads: ribbon mesh %zu vertices, %zu triangles\n", mesh->verts.size() / 12, mesh->idx.size() / 3);
	return mesh;
}

std::string describeBase(int road, int k, const Suspect* s)
{
	const auto& r = g.net.roads()[road];
	const auto& m = r.markers[k];
	char b[320];
	int n = snprintf(b, sizeof b, "road #%d (junctions %d-%d) marker %d/%zu (%.1f, %.1f, %.1f)", road, r.start, r.end, k, r.markers.size(), m.x, m.y, m.z);
	if (!s) { for (const Suspect& q : g.suspects) { if (q.road == road && q.k == k) { s = &q; break; } } }
	if (s) { snprintf(b + n, sizeof b - n, " ratio %.2f turn %.0f extra %.0f", s->ratio, s->turn, s->extra); }
	else if (k > 0 && k + 1 < (int)r.markers.size())
	{
		const auto& a = r.markers[k - 1];
		const auto& c = r.markers[k + 1];
		const double l1 = hd(a, m), l2 = hd(m, c), ch = hd(a, c);
		snprintf(b + n, sizeof b - n, " ratio %.2f extra %.0f", (l1 + l2) / std::max(ch, 1.0), l1 + l2 - ch);
	}
	return b;
}
const Suspect* suspectAt(int road, int k)
{
	for (const Suspect& q : g.suspects) { if (q.road == road && q.k == k) { return &q; } }
	return nullptr;
}

int markOf(int road, int k)
{
	const auto it = g.marks.find({ road, k });
	return it == g.marks.end() ? 0 : it->second;
}

std::string describe(int road, int k, const Suspect* s)
{
	if (!s) { s = suspectAt(road, k); }
	std::string r = describeBase(road, k, s);
	if (s) { r += std::string(" kind ") + s->kind; }
	if (const int mk = markOf(road, k)) { r += std::string(" mark ") + ui::roadMarkName(mk); }
	const std::string th = throughText(road, k);
	if (!th.empty()) { r += " roads: " + th; }
	return r;
}

void select(int road, int k)
{
	g.selRoad = road;
	g.selK = k;
	g.sel = -1;
	for (size_t i = 0; i < g.suspects.size(); i++) { if (g.suspects[i].road == road && g.suspects[i].k == k) { g.sel = (int)i; } }
}
}	// namespace

bool on() { return kRoads.get(); }
void toggle() { kRoads.set(!kRoads.get()); }

roadribbons::Frame ribbons(const std::string& dataDir, const kworld::Heightmap& hm)
{
	roadribbons::Frame f;
	if (!on()) { return f; }
	load(dataDir);
	if (!g.ok) { return f; }
	const int mode = colorMode();
	if (!g.mesh || g.meshMode != mode) { g.mesh = buildMesh(hm, mode); g.meshMode = mode; }
	f.mesh = g.mesh;
	f.params.ribbon = kRibbon.get();
	return f;
}

bool step(int dir, const std::string& dataDir, const kworld::Heightmap& hm, Pose& out)
{
	load(dataDir);
	if (!g.ok || g.suspects.empty()) { return false; }
	const int n = (int)g.suspects.size();
	return selectSuspect(g.sel < 0 ? (dir > 0 ? 0 : n - 1) : ((g.sel + (dir > 0 ? 1 : -1)) % n + n) % n, dataDir, hm, out);
}

bool selectSuspect(int index, const std::string& dataDir, const kworld::Heightmap& hm, Pose& out)
{
	load(dataDir);
	if (!g.ok || index < 0 || index >= (int)g.suspects.size()) { return false; }
	const Suspect& s = g.suspects[index];
	select(s.road, s.k);
	const auto& m = g.net.roads()[s.road].markers;
	const auto &a = m[s.k - 1], &b = m[s.k], &c = m[s.k + 1];
	const double cx = (a.x + b.x + c.x) / 3, cz = (a.z + b.z + c.z) / 3;
	double r = 0;
	for (const auto* p : { &a, &b, &c }) { r = std::max(r, std::hypot(p->x - cx, p->z - cz)); }
	// look across the chord a-c, from the side opposite the spike so it points away up the screen
	double dx = c.x - a.x, dz = c.z - a.z;
	if (std::hypot(dx, dz) < 1.0) { dx = b.x - a.x; dz = b.z - a.z; }
	double nx = -dz, nz = dx;
	if (nx * (b.x - (a.x + c.x) * 0.5) + nz * (b.z - (a.z + c.z) * 0.5) < 0) { nx = -nx; nz = -nz; }
	const double nl = std::hypot(nx, nz);
	nx /= nl; nz /= nl;
	out.pitch = -40.0 * 3.14159265358979 / 180.0;
	out.yaw = std::atan2(nx, -nz);
	const double dist = std::max(150.0, r * 3.2);
	const double ty = std::max((a.y + b.y + c.y) / 3, hm.height(cx, cz));
	const double f[3] = { std::sin(out.yaw) * std::cos(out.pitch), std::sin(out.pitch), -std::cos(out.yaw) * std::cos(out.pitch) };
	out.eye[0] = cx - f[0] * dist;
	out.eye[1] = ty - f[1] * dist;
	out.eye[2] = cz - f[2] * dist;
	printf("%s\n", dump().c_str());
	return true;
}

void draw(const std::string& dataDir, const kworld::Heightmap& hm, const float mvp[16], const double cam[3], float width, float height, double mouseX, double mouseY)
{
	if (!on()) { g.hoverRoad = g.hoverK = -1; return; }
	load(dataDir);
	if (!g.ok) { return; }
	std::copy(mvp, mvp + 16, g.mvp);
	std::copy(cam, cam + 3, g.cam);
	g.w = width;
	g.h = height;
	g.hm = &hm;
	ovl::disable_texture();
	const int base = ovl::num_vertices();
	const auto room = [&](int need) { return ovl::num_vertices() - base + need <= kBudget; };
	const auto& roads = g.net.roads();
	float sx, sy, ex, ey;
	ovl::begin_lines();

	// the handles, junction ends and suspects near the camera; the nearest handle to the pointer is the hover
	auto at = [&](const kworld::RoadNetwork::Marker& m, float& x, float& y) { return project(mvp, cam, width, height, m.x, hm.height(m.x, m.z) + kLift * 2, m.z, x, y); };
	auto cross = [&](float x, float y, float h) { ovl::v2f(x - h, y); ovl::v2f(x + h, y); ovl::v2f(x, y - h); ovl::v2f(x, y + h); };
	auto box = [&](float x, float y, float h) { ovl::v2f(x - h, y - h); ovl::v2f(x + h, y - h); ovl::v2f(x + h, y - h); ovl::v2f(x + h, y + h); ovl::v2f(x + h, y + h); ovl::v2f(x - h, y + h); ovl::v2f(x - h, y + h); ovl::v2f(x - h, y - h); };
	double best = 12.0 * 12.0;
	int hr = -1, hk = -1;
	for (size_t i = 0; i < roads.size(); i++)
	{
		const auto& m = roads[i].markers;
		for (size_t k = 0; k < m.size(); k++)
		{
			const double dd = std::hypot(m[k].x - cam[0], m[k].z - cam[2]);
			if (dd > kHandleRange || !room(16)) { continue; }
			if (!at(m[k], sx, sy)) { continue; }
			const double gy = hm.height(m[k].x, m[k].z);
			const float ppu = pxPerUnit(mvp, cam, height, m[k].x, gy, m[k].z);
			const float fade = 1.0f - smooth(kHandleRange * 0.7, kHandleRange, dd);
			if (std::fabs(m[k].y - gy) > 3.0)	// the data height: a thin stem from the ground up (or down) to the marker's own y, a cap on it
			{
				float tx, ty;
				if (project(mvp, cam, width, height, m[k].x, m[k].y, m[k].z, tx, ty))
				{
					ovl::c4f(1.0f, 0.9f, 0.3f, 0.8f * fade);
					ovl::v2f(sx, sy); ovl::v2f(tx, ty);
					ovl::v2f(tx - 3, ty); ovl::v2f(tx + 3, ty);
				}
			}
			const bool end = k == 0 || k + 1 == m.size();
			if (end)
			{
				const float h = std::clamp(35.0f * ppu, 6.0f, 16.0f);
				ovl::c4f(1.0f, 0.6f, 0.1f, fade);
				ovl::v2f(sx - h, sy); ovl::v2f(sx, sy - h); ovl::v2f(sx, sy - h); ovl::v2f(sx + h, sy); ovl::v2f(sx + h, sy); ovl::v2f(sx, sy + h); ovl::v2f(sx, sy + h); ovl::v2f(sx - h, sy);
			}
			else { ovl::c4f(0.9f, 1.0f, 1.0f, 0.9f * fade); cross(sx, sy, std::clamp(15.0f * ppu, 2.5f, 7.0f)); }
			const double d2 = (sx - mouseX) * (sx - mouseX) + (sy - mouseY) * (sy - mouseY);
			if (d2 < best) { best = d2; hr = (int)i; hk = (int)k; }
		}
	}
	for (const Suspect& s : g.suspects)	// suspects in red at any range (they are few)
	{
		if (!room(8) || !at(roads[s.road].markers[s.k], sx, sy)) { continue; }
		ovl::c4f(1.0f, 0.15f, 0.1f, 1.0f);
		cross(sx, sy, 7.0f);
		const double d2 = (sx - mouseX) * (sx - mouseX) + (sy - mouseY) * (sy - mouseY);
		if (d2 < best) { best = d2; hr = s.road; hk = s.k; }
	}
	g.hoverRoad = hr;
	g.hoverK = hk;
	for (const auto& [key, mk] : g.marks)	// a marked handle shows its mark colour (ok green, unsure amber, bad magenta), at any range
	{
		if (!room(8) || !at(roads[key.first].markers[key.second], sx, sy)) { continue; }
		if (mk == ui::kRoadOk) { ovl::c4f(0.2f, 1.0f, 0.3f, 1.0f); } else if (mk == ui::kRoadUnsure) { ovl::c4f(1.0f, 0.7f, 0.1f, 1.0f); } else { ovl::c4f(1.0f, 0.2f, 0.9f, 1.0f); }
		box(sx, sy, 11.0f);
		box(sx, sy, 10.0f);
	}
	if (g.selRoad >= 0)	// the selection, any marker: a double yellow box with a diamond, and a small box on each neighbour
	{
		const int n = (int)roads[g.selRoad].markers.size();
		for (int d = -1; d <= 1; d++)
		{
			const int k = g.selK + d;
			if (k < 0 || k >= n || !room(24) || !at(roads[g.selRoad].markers[k], sx, sy)) { continue; }
			ovl::c4f(1.0f, 0.95f, 0.2f, 1.0f);
			if (d != 0) { box(sx, sy, 6.0f); continue; }
			box(sx, sy, 9.0f);
			box(sx, sy, 13.0f);
			const float h = 17.0f;
			ovl::v2f(sx - h, sy); ovl::v2f(sx, sy - h); ovl::v2f(sx, sy - h); ovl::v2f(sx + h, sy); ovl::v2f(sx + h, sy); ovl::v2f(sx, sy + h); ovl::v2f(sx, sy + h); ovl::v2f(sx - h, sy);
		}
	}
	if (hr >= 0 && room(8) && at(roads[hr].markers[hk], ex, ey)) { ovl::c4f(1.0f, 1.0f, 1.0f, 1.0f); box(ex, ey, 8.0f); }

	if (colorMode() == 0 && kRibbon.get())	// the legend: the heading wheel with N / E / S / W, bottom left
	{
		const float cx = 64.0f, cy = height - 72.0f;
		const float PI = 3.14159265f;
		for (int a = 0; a < 360; a += 2)
		{
			const float th = (float)a * PI / 180.0f, dx = std::sin(th), dz = -std::cos(th);
			float r, gg, b;
			headingRgb(dx, dz, r, gg, b);
			ovl::c4f(r, gg, b, 1.0f);
			ovl::v2f(cx + dx * 20.0f, cy + dz * 20.0f);
			ovl::v2f(cx + dx * 34.0f, cy + dz * 34.0f);
		}
		ovl::c4f(1.0f, 1.0f, 1.0f, 1.0f);
		const auto ln = [&](float x0, float y0, float x1, float y1) { ovl::v2f(x0, y0); ovl::v2f(x1, y1); };
		{ const float x = cx, y = cy - 46.0f; ln(x - 4, y + 5, x - 4, y - 5); ln(x - 4, y - 5, x + 4, y + 5); ln(x + 4, y + 5, x + 4, y - 5); }	// N
		{ const float x = cx + 46.0f, y = cy; ln(x - 4, y - 5, x - 4, y + 5); ln(x - 4, y - 5, x + 4, y - 5); ln(x - 4, y, x + 3, y); ln(x - 4, y + 5, x + 4, y + 5); }	// E
		{ const float x = cx, y = cy + 46.0f; ln(x + 4, y - 5, x - 4, y - 5); ln(x - 4, y - 5, x - 4, y); ln(x - 4, y, x + 4, y); ln(x + 4, y, x + 4, y + 5); ln(x + 4, y + 5, x - 4, y + 5); }	// S
		{ const float x = cx - 46.0f, y = cy; ln(x - 5, y - 5, x - 3, y + 5); ln(x - 3, y + 5, x, y - 2); ln(x, y - 2, x + 3, y + 5); ln(x + 3, y + 5, x + 5, y - 5); }	// W
	}
	ovl::count_lines();
	ovl::end();
}

bool legend(std::string& line)
{
	if (!on()) { return false; }
	if (!g.tried) { line = "roads: loading"; return true; }
	if (!g.ok) { line = "roads: " + (g.error.empty() ? std::string("not loaded") : g.error); return true; }
	if (g.hoverRoad >= 0) { line = "roads hover: " + describe(g.hoverRoad, g.hoverK, nullptr); }
	else if (g.selRoad >= 0 && g.sel < 0) { line = "roads selected: " + describe(g.selRoad, g.selK, nullptr); }
	else if (g.sel >= 0) { line = "roads suspect " + std::to_string(g.sel + 1) + "/" + std::to_string(g.suspects.size()) + ": " + describe(g.suspects[g.sel].road, g.suspects[g.sel].k, &g.suspects[g.sel]) + " width: data n/a"; }
	else { line = "roads: " + std::to_string(g.suspects.size()) + " suspects (road_next / road_prev)"; }
	return true;
}

std::string dump()
{
	if (g.selRoad < 0) { return "roads: none selected"; }
	if (g.sel < 0) { return "roads marker: " + describe(g.selRoad, g.selK, nullptr); }
	const Suspect& s = g.suspects[g.sel];
	return "roads suspect " + std::to_string(g.sel + 1) + "/" + std::to_string(g.suspects.size()) + ": " + describe(s.road, s.k, &s);
}

std::string summary()
{
	return "roads: " + std::to_string(g.net.roads().size()) + " roads, " + std::to_string(g.markers) + " markers, " + std::to_string(g.suspects.size()) + " suspects";
}

bool selectMarker(int road, int marker, const std::string& dataDir)
{
	load(dataDir);
	if (!g.ok || road < 0 || road >= (int)g.net.roads().size() || marker < 0 || marker >= (int)g.net.roads()[road].markers.size()) { return false; }
	select(road, marker);
	return true;
}

void clearSelection() { g.selRoad = g.selK = g.sel = -1; }

void click()
{
	if (g.hoverRoad >= 0) { select(g.hoverRoad, g.hoverK); }
	else { clearSelection(); }
	printf("%s\n", dump().c_str());
}

bool mark(int m)
{
	if (g.selRoad < 0 || m < 0 || m > ui::kRoadBad) { return false; }
	if (m == ui::kRoadUnmarked) { g.marks.erase({ g.selRoad, g.selK }); } else { g.marks[{ g.selRoad, g.selK }] = m; }
	g.psDirty = true;
	return true;
}

std::string coords()
{
	if (g.selRoad < 0) { return ""; }
	const auto& m = g.net.roads()[g.selRoad].markers[g.selK];
	return ui::roadsVec({ (float)m.x, (float)m.y, (float)m.z });
}

bool screenOf(int road, int marker, double& x, double& y)
{
	if (!g.ok || !g.hm || road < 0 || road >= (int)g.net.roads().size() || marker < 0 || marker >= (int)g.net.roads()[road].markers.size()) { return false; }
	const auto& m = g.net.roads()[road].markers[marker];
	float sx, sy;
	if (!project(g.mvp, g.cam, g.w, g.h, m.x, g.hm->height(m.x, m.z) + kLift * 2, m.z, sx, sy)) { return false; }
	x = sx;
	y = sy;
	return true;
}

const ui::RoadsPanelState& panelState(const std::string& dataDir)
{
	load(dataDir);
	if (!g.ok) { g.ps.status = "roads: " + g.error; return g.ps; }
	const auto& roads = g.net.roads();
	const auto vec = [](const kworld::RoadNetwork::Marker& m) { return ui::RoadsVec{ (float)m.x, (float)m.y, (float)m.z }; };
	if (g.psDirty)
	{
		g.psDirty = false;
		g.ps.suspects.clear();
		int rank = 0;
		for (const Suspect& s : g.suspects)
		{
			ui::RoadsSuspect r;
			r.rank = ++rank;
			r.road = roadText(s.road);
			r.marker = s.k;
			r.count = (int)roads[s.road].markers.size();
			r.pos = vec(roads[s.road].markers[s.k]);
			r.ratio = (float)s.ratio;
			r.turn = (float)s.turn;
			r.extra = (float)s.extra;
			r.mark = markOf(s.road, s.k);
			r.kind = s.kind;
			g.ps.suspects.push_back(std::move(r));
		}
	}
	g.ps.selected = g.sel;
	ui::RoadsInfo in;
	if (g.selRoad >= 0)
	{
		const auto& r = roads[g.selRoad];
		const int n = (int)r.markers.size();
		in.valid = true;
		in.road = roadText(g.selRoad);
		in.marker = g.selK;
		in.count = n;
		in.pos = vec(r.markers[g.selK]);
		in.hasPrev = g.selK > 0;
		in.hasNext = g.selK + 1 < n;
		if (in.hasPrev) { in.prev = vec(r.markers[g.selK - 1]); }
		if (in.hasNext) { in.next = vec(r.markers[g.selK + 1]); }
		in.suspect = g.sel;
		if (g.selK == 0 || g.selK + 1 == n)
		{
			const int j = g.selK == 0 ? r.start : r.end;
			in.junction = std::to_string(j);
			in.degree = g.degree.count(j) ? g.degree[j] : 0;
		}
		in.mark = markOf(g.selRoad, g.selK);
		in.roads = throughText(g.selRoad, g.selK);
	}
	g.ps.info = in;
	g.ps.status = summary() + ", " + std::to_string(g.marks.size()) + " marked";
	return g.ps;
}

std::string marksDump()
{
	std::string s = "roads marks: " + std::to_string(g.marks.size());
	for (const auto& [key, mk] : g.marks)
	{
		s += "\nroads mark " + roadText(key.first) + " marker " + std::to_string(key.second) + ": " + ui::roadMarkName(mk);
	}
	return s;
}

std::string list()
{
	std::string s;
	for (size_t i = 0; i < g.suspects.size(); i++)
	{
		s += "roads list " + std::to_string(i + 1) + "/" + std::to_string(g.suspects.size()) + ": " + describe(g.suspects[i].road, g.suspects[i].k, &g.suspects[i]) + "\n";
	}
	return s;
}
}
