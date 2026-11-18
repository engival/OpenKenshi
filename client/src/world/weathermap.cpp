#include "world/weathermap.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <map>

#include "kworld/gamedata.hpp"
#include "render/renderqueue.hpp"

// Weather per region (rule written down in docs/CLIENT_GOALS.md, "Regional weather"):
//  - areasmap.tga: 256^2, 24 bpp, bottom-left origin; the pixel colour 0xRRGGBB equals BIOME_GROUP.index (TERRAIN_FORMAT.md). Rows are flipped so row 0 = north.
//  - BIOME_GROUP.seasons: refs (SEASON, v0 = order, v1 = relative duration). Default season = the longest duration; ties go to the lower order, then to list order.
//  - SEASON.weathers: refs (WEATHER, v0 = probability, v1/v2 = min/max duration in minutes; fcs.def). Representative weather = the highest probability; ties go to list order.
//  - A group with no seasons (or no weathers), and cells the map gives no group, show the default SkyWeather.

namespace
{

const FogMapBinding* g_binding = nullptr;

// The one function that decides which weather a group shows. The sim will own real weather state (ARCHITECTURE.md "Runtime state") and arrive by snapshot:
// replace this body (or the WeatherAtlas::weathers entries it feeds) then. nullptr = the default weather.
const kdata::Record* representativeWeather(const kdata::GameData& gd, const kdata::Record& group, const kdata::Record** seasonOut)
{
	const kdata::RefCategory* seasons = group.findRefs("seasons");
	if (!seasons) { return nullptr; }
	const kdata::Record* best = nullptr;
	const kdata::RefEntry* bestRef = nullptr;
	for (const kdata::RefEntry& e : seasons->entries)
	{
		const kdata::Record* s = gd.find(e.targetID);
		if (!s) { continue; }
		if (!best || e.v1 > bestRef->v1 || (e.v1 == bestRef->v1 && e.v0 < bestRef->v0)) { best = s; bestRef = &e; }
	}
	if (!best) { return nullptr; }
	*seasonOut = best;
	const kdata::RefCategory* weathers = best->findRefs("weathers");
	if (!weathers) { return nullptr; }
	const kdata::Record* w = nullptr;
	int bestProb = -1;
	for (const kdata::RefEntry& e : weathers->entries)
	{
		const kdata::Record* r = gd.find(e.targetID);
		if (r && e.v0 > bestProb) { w = r; bestProb = e.v0; }
	}
	return w;
}

uint16_t to_half(float f)
{
	uint32_t x;
	memcpy(&x, &f, 4);
	const uint32_t sign = (x >> 16) & 0x8000;
	int e = (int)((x >> 23) & 255) - 127 + 15;
	uint32_t m = x & 0x7fffff;
	if (e <= 0)
	{
		if (e < -10) { return (uint16_t)sign; }
		m |= 0x800000;
		return (uint16_t)(sign | ((m >> (14 - e)) + ((m >> (13 - e)) & 1)));
	}
	if (e >= 31) { return (uint16_t)(sign | 0x7bff); }
	return (uint16_t)(sign | (e << 10) | (m >> 13));
}

}

const FogMapBinding& fog_map_binding()
{
	static const FogMapBinding none;
	return g_binding ? *g_binding : none;
}

WeatherAtlas WeatherAtlas::load(const std::string& dataDir)
{
	WeatherAtlas a;
	const std::string path = dataDir + "/newland/land/areasmap.tga";
	FILE* f = fopen(path.c_str(), "rb");
	if (!f) { a.source = "areasmap.tga not found: " + path; return a; }
	unsigned char h[18];
	std::vector<unsigned char> px;
	bool ok = fread(h, 1, 18, f) == 18 && h[2] == 2 && h[16] == 24 && (h[12] | h[13] << 8) == kN && (h[14] | h[15] << 8) == kN && !fseek(f, h[0], SEEK_CUR);
	if (ok)
	{
		px.resize((size_t)kN * kN * 3);
		ok = fread(px.data(), 1, px.size(), f) == px.size();
	}
	fclose(f);
	if (!ok) { a.source = "areasmap.tga is not the expected 256x256 uncompressed 24-bit TGA"; return a; }
	const bool topFirst = (h[17] & 0x20) != 0;
	try
	{
		const kdata::GameData gd = kworld::loadMergedGameData(dataDir);
		const bool log = getenv("KWEATHERLOG") != nullptr;
		for (const kdata::Record* w : gd.byType(kworld::itemTypeByName("WEATHER"))) { a.byId.emplace(w->stringID, SkyWeather::fromRecord(*w, gd)); }
		std::map<uint32_t, uint8_t> groupByColour;
		a.groupNames.assign(1, std::string());
		a.groupIds.assign(1, std::string());
		a.groupWeather.resize(1);
		a.groupWeather[0].name = "default";
		for (const kdata::Record* g : gd.byType(kworld::itemTypeByName("BIOME_GROUP")))
		{
			const kdata::Record* season = nullptr;
			const kdata::Record* w = representativeWeather(gd, *g, &season);
			groupByColour[(uint32_t)g->intOr("index", 0) & 0xffffff] = (uint8_t)a.groupNames.size();
			a.groupNames.push_back(g->name);
			a.groupIds.push_back(g->stringID);
			a.groupWeather.push_back(w ? a.byId.at(w->stringID) : a.groupWeather[0]);
			if (log) { fprintf(stderr, "weather: group %06x %-22s season %-28s weather %s\n", (unsigned)g->intOr("index", 0), g->name.c_str(), season ? season->name.c_str() : "-", w ? w->name.c_str() : "-"); }
		}
		a.group.assign((size_t)kN * kN, 0);
		a.groupCells.assign(a.groupNames.size(), {});
		for (int row = 0; row < kN; row++)
		{
			const int src = topFirst ? row : kN - 1 - row;
			for (int col = 0; col < kN; col++)
			{
				const unsigned char* p = &px[((size_t)src * kN + col) * 3];	// B, G, R
				const auto g = groupByColour.find((uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0]);
				a.group[(size_t)row * kN + col] = g == groupByColour.end() ? 0 : g->second;
				a.groupCells[a.group[(size_t)row * kN + col]].emplace_back((uint16_t)col, (uint16_t)row);
			}
		}
		char buf[200];
		snprintf(buf, sizeof buf, "areasmap %d groups, %zu weathers", (int)groupByColour.size(), a.byId.size());
		a.source = buf;
	}
	catch (const std::exception& e)
	{
		a.group.clear();
		a.source = std::string("weather atlas failed: ") + e.what();
	}
	return a;
}

void WeatherMap::init()
{
	const uint16_t zero[4] = {};
	sg_image_desc id = {};
	id.width = id.height = 1;
	id.pixel_format = SG_PIXELFORMAT_RGBA16F;
	id.data = SG_RANGE(zero);
	id.label = "weather-map-dummy";
	dummy_ = sg_make_image(&id);
	sg_view_desc vd = {};
	vd.texture.image = dummy_;
	dummyView_ = sg_make_view(&vd);
	sg_sampler_desc sd = {};
	sd.min_filter = sd.mag_filter = SG_FILTER_LINEAR;
	sd.wrap_u = sd.wrap_v = SG_WRAP_CLAMP_TO_EDGE;
	sd.label = "weather-map";
	smp_ = sg_make_sampler(&sd);
	static FogMapBinding b;
	b.view = dummyView_;
	b.smp = smp_;
	g_binding = &b;
}

void WeatherMap::shutdown()
{
	if (view_.id != SG_INVALID_ID) { sg_destroy_view(view_); }
	if (img_.id != SG_INVALID_ID) { sg_destroy_image(img_); }
	sg_destroy_view(dummyView_);
	sg_destroy_image(dummy_);
	sg_destroy_sampler(smp_);
	g_binding = nullptr;
}

// Stage 2 texture: per cell the effective global fog (SkyWeather::fogRho / fogRaw) premultiplied by its density (so a clear cell with no fog does not wash out a storm's
// colour when blurred), x 1e4 to keep half floats in range; then a separable Gaussian. The shader divides rgb by a again (lighting.inc light_finish).
void WeatherMap::setAtlas(WeatherAtlas&& a, float frontBlurKm)
{
	atlas_ = std::move(a);
	frontBlurKm_ = frontBlurKm;
	fprintf(stderr, "kclient: %s\n", atlas_.source.c_str());
	if (atlas_.group.empty()) { return; }
	for (size_t i = 1; i < atlas_.groupIds.size(); i++) { groupIndex_[atlas_.groupIds[i]] = (uint8_t)i; }
	buildFogMap();
}

void WeatherMap::buildFogMap()
{
	constexpr int N = WeatherAtlas::kN;
	const double cellUnits = WeatherAtlas::kWorld / N;
	std::vector<float> m((size_t)N * N * 4), t(m.size());
	for (size_t i = 0; i < (size_t)N * N; i++)
	{
		const SkyWeather& w = atlas_.groupWeather[atlas_.group[i]];
		const float s = w.fogRho * 1e4f;
		for (int k = 0; k < 3; k++) { m[i * 4 + k] = w.fogRaw[k] * s; }
		m[i * 4 + 3] = s;
	}
	const double sigma = frontBlurKm_ * 1000.0 / cellUnits;
	if (sigma > 0.05)
	{
		const int R = (int)std::ceil(sigma * 3);
		std::vector<float> k(R * 2 + 1);
		float sum = 0;
		for (int i = -R; i <= R; i++) { k[i + R] = (float)std::exp(-0.5 * i * i / (sigma * sigma)); sum += k[i + R]; }
		for (float& v : k) { v /= sum; }
		for (int pass = 0; pass < 2; pass++)
		{
			const std::vector<float>& in = pass == 0 ? m : t;
			std::vector<float>& out = pass == 0 ? t : m;
			for (int y = 0; y < N; y++)
			{
				for (int x = 0; x < N; x++)
				{
					float acc[4] = {};
					for (int i = -R; i <= R; i++)
					{
						const int xx = pass == 0 ? std::clamp(x + i, 0, N - 1) : x, yy = pass == 0 ? y : std::clamp(y + i, 0, N - 1);
						for (int c = 0; c < 4; c++) { acc[c] += k[i + R] * in[((size_t)yy * N + xx) * 4 + c]; }
					}
					for (int c = 0; c < 4; c++) { out[((size_t)y * N + x) * 4 + c] = acc[c]; }
				}
			}
		}
	}
	std::vector<uint16_t> half(m.size());
	for (size_t i = 0; i < m.size(); i++) { half[i] = to_half(m[i]); }
	// render side (renderqueue.hpp, frame lane): the image swap and the binding the shaders read, before this packet's draw
	const size_t bytes = half.size() * sizeof(uint16_t);
	rq::push("weather-map", [this, half = std::move(half)] { makeFogMap(half); }, rq::Lane::kFrame, bytes);
}

void WeatherMap::makeFogMap(const std::vector<uint16_t>& half)
{
	constexpr int N = WeatherAtlas::kN;
	sg_image_desc id = {};
	id.width = id.height = N;
	id.pixel_format = SG_PIXELFORMAT_RGBA16F;
	id.data = { half.data(), half.size() * sizeof(uint16_t) };
	id.label = "weather-map";
	if (img_.id) { sg_destroy_view(view_); sg_destroy_image(img_); }	// a live fog change rebuilds it (immutable images)
	img_ = sg_make_image(&id);
	sg_view_desc vd = {};
	vd.texture.image = img_;
	view_ = sg_make_view(&vd);
	static FogMapBinding b;
	b.view = view_;
	b.smp = smp_;
	g_binding = &b;
}

const std::string& WeatherAtlas::groupAt(double x, double z) const
{
	static const std::string none;
	const double cellUnits = kWorld / kN;
	const int cx = (int)std::floor((x + kWorld * 0.5) / cellUnits), cz = (int)std::floor((z + kWorld * 0.5) / cellUnits);
	if (group.empty() || cx < 0 || cz < 0 || cx >= kN || cz >= kN) { return none; }
	return groupNames[group[(size_t)cz * kN + cx]];
}

int WeatherAtlas::groupIndexAt(double x, double z) const
{
	const double cellUnits = kWorld / kN;
	const int cx = (int)std::floor((x + kWorld * 0.5) / cellUnits), cz = (int)std::floor((z + kWorld * 0.5) / cellUnits);
	if (group.empty() || cx < 0 || cz < 0 || cx >= kN || cz >= kN) { return -1; }
	return group[(size_t)cz * kN + cx];
}

SkyWeather WeatherMap::blend(const double pos[3], float widthKm, std::string& mix) const
{
	constexpr int N = WeatherAtlas::kN;
	const double cellUnits = WeatherAtlas::kWorld / N;
	const double sigma = std::fmax(widthKm * 1000.0 / 2.355, 1.0);	// full width at half maximum -> sigma
	const double step = sigma * 0.5;
	std::vector<float> weight(atlas_.groupWeather.size(), 0.0f);
	for (int j = -4; j <= 4; j++)
	{
		for (int i = -4; i <= 4; i++)
		{
			const double dx = i * step, dz = j * step;
			const double w = std::exp(-0.5 * (dx * dx + dz * dz) / (sigma * sigma));
			const int cx = std::clamp((int)std::floor((pos[0] + dx + WeatherAtlas::kWorld * 0.5) / cellUnits), 0, N - 1);
			const int cz = std::clamp((int)std::floor((pos[2] + dz + WeatherAtlas::kWorld * 0.5) / cellUnits), 0, N - 1);
			weight[atlas_.group[(size_t)cz * N + cx]] += (float)w;
		}
	}
	std::vector<std::pair<const SkyWeather*, float>> parts;
	float total = 0;
	for (size_t i = 0; i < weight.size(); i++)
	{
		if (weight[i] > 0) { parts.emplace_back(&atlas_.groupWeather[i], weight[i]); total += weight[i]; }
	}
	std::sort(parts.begin(), parts.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
	mix.clear();
	for (size_t i = 0; i < parts.size() && i < 3; i++)
	{
		const int pct = (int)std::lround(parts[i].second / total * 100.0f);
		if (pct < 1) { break; }
		char buf[96];
		snprintf(buf, sizeof buf, "%s%s %d%%", i ? " / " : "", parts[i].first->name.c_str(), pct);
		mix += buf;
	}
	SkyWeather out = SkyWeather::blend(parts);
	out.name = "auto";
	return out;
}

void WeatherMap::setLive(const std::vector<simlink::Weather>& live, const simlink::SimLinkDecoder& dec)
{
	if (atlas_.group.empty()) { return; }
	const SkyWeather none = atlas_.groupWeather[0];
	const auto find = [&](uint32_t id) -> const SkyWeather*
	{
		const auto it = atlas_.byId.find(dec.str(id));
		return it == atlas_.byId.end() ? nullptr : &it->second;
	};
	const auto lerp = [](float a, float b, float t) { return a + (b - a) * t; };
	static const bool log = getenv("KWEATHERLOG") != nullptr;
	bool fogChanged = false;
	for (const simlink::Weather& l : live)
	{
		const auto gi = groupIndex_.find(dec.str(l.group));
		if (gi == groupIndex_.end()) { continue; }
		const SkyWeather* cur = find(l.weather);
		const SkyWeather& now = cur ? *cur : none;
		const SkyWeather* prev = find(l.previous);
		const SkyWeather& was = prev ? *prev : now;
		SkyWeather w = now;
		const float fb = std::clamp(l.fogBlend, 0.0f, 1.0f), s = std::clamp(l.strength, 0.0f, 1.0f);
		// sky colour mult and clouds density stay the record's: the sky eases them itself (SkyRamp), from its current value rather than from this group's
		// previous record, so the wire's skyBlend (the same 30 s) is not used
		w.rain *= s;
		w.wetness *= s;
		w.dust *= s;
		w.dustInside *= s;
		w.heatHaze *= s;
		w.windSpeed = lerp(now.windSpeedMin, now.windSpeed, s);	// 1409dcf60: wind speed = lerp(min, max, strength)
		if (dec.windValid) { w.windValid = true; w.windSpeed = dec.windSpeed; w.windYaw = dec.windYaw; }	// the frame's tail overrides it: the live wind at the camera
		for (SkyWeather::Effect& e : w.effects) { e.amount *= s; }	// the exe: effect = value x strength (WEATHER_CYCLE_LIVE section 5)
		w.finalize();
		// fog: the effective density and colour (finalize: the record's fog, or the cloud haze) from the previous weather's over the first 2.5 % (fogBlend)
		w.fogRho = lerp(was.fogRho, w.fogRho, fb);
		for (int k = 0; k < 3; k++) { w.fogRaw[k] = lerp(was.fogRaw[k], w.fogRaw[k], fb); }
		SkyWeather& slot = atlas_.groupWeather[gi->second];
		if (log && slot.name != w.name) { fprintf(stderr, "weather: live %-22s %s -> %s (fog %.2f strength %.2f wind %.1f)\n", atlas_.groupNames[gi->second].c_str(), slot.name.c_str(), w.name.c_str(), fb, s, w.windSpeed); }
		fogChanged |= std::fabs(slot.fogRho - w.fogRho) > 1e-9f || slot.fogRaw[0] != w.fogRaw[0] || slot.fogRaw[1] != w.fogRaw[1] || slot.fogRaw[2] != w.fogRaw[2];
		slot = std::move(w);
	}
	if (fogChanged) { buildFogMap(); }
}
