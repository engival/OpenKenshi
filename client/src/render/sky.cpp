#include "render/sky.hpp"
#include "render/drawstats.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <exception>

#include "assets/dds.hpp"
#include "kworld/gamedata.hpp"
#include "sky.glsl.h"
#include "stb_image.h"

static_assert(sizeof(LightParams) == sizeof(float) * (44 + 4 + 16 * LightParams::kMaxPoints + 36));

namespace
{

constexpr double kPi = 3.14159265358979;

// SkyX atmosphere options as set by the exe (SKY_EXE section 2).
constexpr double kInnerRadius = 9.77501, kOuterRadius = 10.2963, kHeight = 0.00521278;	// kHeight: the dome camera, live uCameraPos.y 9.78022289 - inner (kenshi-inspect SkyXFogParams)
constexpr double kHeightPosition = 0.01;	// the SkyX option HeightPosition (a fraction of the shell thickness): getColorAt's camera radius = Inner + (Outer - Inner) x this
constexpr double kRayleigh = 0.0022, kMie = 0.000675, kSunIntensity = 30.0;
constexpr double kWave[3] = { 0.57, 0.48, 0.44 };
constexpr double kG = -0.991, kAtmExposure = 1.4;
constexpr double kScaleDepth = 0.260645;	// live SkyXFogParams.uScaleDepth (kenshi-inspect, noon and night); the dome drawn matches it, not the 0.125 of the older reading (LIGHTING.md 12)
constexpr int kSamples = 4;		// the exe's NumberOfSamples (4 per vertex; the dome shader runs it per pixel)
constexpr int kColourSamples = 4;	// getColorAt on the CPU: the exe's own sample count

// The exe's sampling of getColorAt (SKY2_EXE Q2, Q5). Naming swap: the game's "zenithLight" samples the horizontal UNIT_Z, its "nadirLight" UNIT_Y (up).
constexpr double kSunSampleOffset[3] = { 0, 0, 0.04 };	// sun colour = getColorAt(sunDirReal + this) / (Exposure x 4), only while sunDirReal.y >= kSunMinY
constexpr double kSunMinY = -0.2;
constexpr double kAmbientBase[3] = { 0.001, 0.001, 0.0015 };	// sky+0xb8: floor of both ambient samples
constexpr double kExposureKey = 0.55;		// EXPOSURE_KEY (common.program); exposure = key / adapted luminance
constexpr float kFogStartFrac = 0.05f, kFogEndFrac = 0.5f;	// atmosphere fog alpha = sat((d - y) / (z - y)), pFogParams (50000, 3000, 30000) measured live (greenlands_bridge): fractions of the 60000 default view distance
constexpr double kAtmFogColour[3] = { 0.94, 0.85, 0.80 };	// the exe's constant scene fog colour (SKY_EXE C51), stands in for the per-pixel atmosphere fog term

// Mine, not the exe's (documented in CLIENT_GOALS.md M3b):
constexpr double kAmbientParamsW = 1.0;	// the exe's ambientParams.w (sun multiplier, deferred.hlsl:197): common.program default, the exe's per-frame value is not traced (docs/reference/LIGHTING.md, Needs exe)
constexpr double kEnvColourW = 1.0;		// the exe's envColour.w (irradiance multiplier, deferred.hlsl:212-213): common.program default, not traced
constexpr double kNoonSceneLum = 0.25;		// estimated scene luminance at noon in HDR units. No luminance readback: assumed below MIN_LUMINANCE at all hours, so adapted = MIN and the exposure follows nightDarkness
constexpr double kHcCloudScale = 3.0;		// cloud+0x24 in s = 1 - 0.1 (coverage + 0.2) x it (LIGHTING_EXE Q6): pinned by three live points, rwh.140 Heavy_Rain d = 1 at 08:27 and 12:00 (s .760, .759), the d = .1 sample (1.138), and clear (1.183)
constexpr double kCloudWind = 0.00005;	// uv per unit of accumulated wind: SkyX_Clouds.hlsl offsets the layer by uWindDirection (the exe's accumulated wind, C38) x this

// Moons (SKY2_EXE Q4, SKY3_EXE Q1): directions are fixed; angular radius = atan(size x tan(0.01 deg) x mesh bounding radius), spin about +Y by minutes x rate / 1440.
constexpr double kMoon1[3] = { 1.0, 0.3, -1.0 }, kMoon2[3] = { 1.0, 0.14, -0.7 };
constexpr double kMoonMeshRadius = 25.87, kMoon1Size = 35.0, kMoon2Size = 10.0;
constexpr double kMoon1Spin = 1.0, kMoon2Spin = 4.731;	// rad per game day
constexpr double kMoonTexWidth[2] = { 4096.0, 2048.0 };

struct Atm
{
	double invWl[3], kr4pi, km4pi, kresun, kmesun, scale, sosd;
	Atm()
	{
		for (int i = 0; i < 3; i++) { invWl[i] = 1.0 / std::pow(kWave[i], 4.0); }
		kr4pi = kRayleigh * 4.0 * kPi;
		km4pi = kMie * 4.0 * kPi;
		kresun = kRayleigh * kSunIntensity;
		kmesun = kMie * kSunIntensity;
		scale = 1.0 / (kOuterRadius - kInnerRadius);
		sosd = scale / kScaleDepth;
	}
};

const Atm& atm()
{
	static const Atm a;
	return a;
}

double atm_scale(double c, double scaleDepth)
{
	const double x = 1.0 - c;
	return scaleDepth * std::exp(-0.00287 + x * (0.459 + x * (3.83 + x * (-6.80 + x * 5.25))));
}

// SkyX AtmosphereManager::getColorAt as the exe's SkyX_x64.dll implements it (RVA 0x38e0, LDR/HDR split at 0x4093, HDR branch; LIGHTING.md section 8). Light along unit l.
// The dome shader (sky.glsl, SkyX_Skydome.hlsl) has the same ~1 ray length and the same live scale depth; only its camera radius differs. Points that matter, all verified against the measured
// zenithLight / nadirLight / sunColour of the live game:
//  - scale depth is (Outer - Inner) / 2 = 0.2606 here, not the material's 0.125;
//  - the camera sits at Inner + (Outer - Inner) x HeightPosition (HeightPosition is a fraction), not Inner + 0.01;
//  - the "ray" is d + (0, Inner - cameraRadius, 0), normalised, and Far is that vector's length (about 1, not the distance to the outer shell);
//  - the Mie/Rayleigh phase cosine uses this tilted ray, not d (the Mie peak at the sun is razor sharp: it matters by 20 %).
// HDR, atmosphere exposure applied; no additive night tail here (it only exists for the sun below the horizon and vanishes for UNIT_Y).
// mie false: the Rayleigh part alone (the dynamic ambient: the Mie lobe is the sun's glare, a razor-sharp spike a sample grid hits or misses frame to frame).
void atm_sky(const double d[3], const double l[3], int samples, double out[3], bool mie = true)
{
	const Atm& a = atm();
	const double sd = (kOuterRadius - kInnerRadius) * 0.5;
	const double sosd = a.scale / sd;
	const double rc = kInnerRadius + (kOuterRadius - kInnerRadius) * kHeightPosition;
	const double dn = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
	double ray[3] = { d[0] / dn, d[1] / dn + kInnerRadius - rc, d[2] / dn };
	const double far = std::sqrt(ray[0] * ray[0] + ray[1] * ray[1] + ray[2] * ray[2]);
	for (double& v : ray) { v /= far; }
	const double off = std::exp(sosd * (kInnerRadius - rc)) * atm_scale(ray[1], sd);
	const double sl = far / samples;
	double p[3] = { ray[0] * sl * 0.5, rc + ray[1] * sl * 0.5, ray[2] * sl * 0.5 };
	double front[3] = {};
	for (int i = 0; i < samples; i++)
	{
		const double h = std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
		const double depth = std::exp(sosd * (kInnerRadius - h));
		const double la = (l[0] * p[0] + l[1] * p[1] + l[2] * p[2]) / h;
		const double ca = (ray[0] * p[0] + ray[1] * p[1] + ray[2] * p[2]) / h;
		const double scatter = off + depth * (atm_scale(la, sd) - atm_scale(ca, sd));
		for (int k = 0; k < 3; k++) { front[k] += std::exp(-scatter * (a.invWl[k] * a.kr4pi + a.km4pi)) * depth * sl * a.scale; }
		for (int k = 0; k < 3; k++) { p[k] += ray[k] * sl; }
	}
	const double c = -(ray[0] * l[0] + ray[1] * l[1] + ray[2] * l[2]);
	const double rp = 0.75 * (1.0 + 0.5 * c * c);
	const double mp = 1.5 * ((1.0 - kG * kG) / (2.0 + kG * kG)) * (1.0 + c * c) / std::pow(1.0 + kG * kG - 2.0 * kG * c, 1.5);
	for (int k = 0; k < 3; k++) { out[k] = kAtmExposure * (rp * front[k] * a.invWl[k] * a.kresun + (mie ? mp * front[k] * a.kmesun : 0.0)); }
}

double luma(const double c[3]) { return 0.30 * c[0] + 0.59 * c[1] + 0.11 * c[2]; }
double saturate(double x) { return std::fmax(0.0, std::fmin(1.0, x)); }

sg_image load_dds(const ResourceIndex& res, const char* name, const char* label)
{
	const std::string path = res.find(name);
	DdsImage img;
	std::string err;
	if (path.empty() || !dds_load(path, img, err))
	{
		fprintf(stderr, "kclient: sky texture %s unavailable (%s)\n", name, path.empty() ? "not in a resource directory" : err.c_str());
		return {};
	}
	return dds_make_image(img, label);
}

}

void GameClock::setHours(double h)
{
	while (h >= 24.0) { h -= 24.0; day++; }
	while (h < 0.0) { h += 24.0; day--; }
	hours = h;
}

void sky_sun_direction(double t, double sunrise, double sunset, double latitude, double out[3])
{
	if (sunset <= sunrise) { sunrise = 6.0; sunset = 20.0; }	// C4/C5 fallback
	double s;
	if (t >= sunrise && t < sunset) { s = (t - sunrise) / (sunset - sunrise); }
	else { s = 1.0 + ((t < sunrise ? t + 24.0 : t) - sunset) / (24.0 - (sunset - sunrise)); }
	const double a = s * kPi;
	out[0] = std::cos(a);
	out[1] = std::cos(latitude) * std::sin(a);
	out[2] = std::sin(latitude) * std::sin(a);
}

SkyConstants SkyConstants::load(const std::string& dataDir)
{
	SkyConstants k;
	try
	{
		const kdata::GameData gd = kworld::loadMergedGameData(dataDir);
		const std::vector<const kdata::Record*> recs = gd.byType(kworld::itemTypeByName("CONSTANTS"));
		if (recs.empty()) { k.source = "defaults (no CONSTANTS record)"; return k; }
		const kdata::Record& r = *recs.front();
		auto field = [&](const char* key, float& v)
		{
			if (const kdata::FieldFloat* f = r.findFloat(key)) { v = kdata::decodeFloat(*f); return true; }
			fprintf(stderr, "kclient: GLOBAL CONSTANTS has no float field '%s'\n", key);
			return false;
		};
		float lat = 0;
		field("sunrise", k.sunrise);
		field("sunset", k.sunset);
		if (field("latitude", lat)) { k.latitude = (float)(lat * kPi / 180.0); }
		field("night darkness", k.nightDarkness);
		field("exposure min", k.exposureMin);
		field("exposure max", k.exposureMax);
		char buf[200];
		snprintf(buf, sizeof buf, "GLOBAL CONSTANTS '%s' (merged): sunrise %.2f sunset %.2f latitude %.1f exposure %.2f..%.2f night darkness %.2f", r.stringID.c_str(),
			k.sunrise, k.sunset, lat, k.exposureMin, k.exposureMax, k.nightDarkness);
		k.source = buf;
	}
	catch (const std::exception& e) { k.source = std::string("defaults (") + e.what() + ")"; }
	return k;
}

SkyWeather SkyWeather::fromRecord(const kdata::Record& rec, const kdata::GameData& gd)
{
	const kdata::Record* r = &rec;
	SkyWeather w;
	auto colour = [&](const char* key, float out[3])
	{
		for (const kdata::FieldInt& f : r->ints)
		{
			if (f.key == key) { for (int k = 0; k < 3; k++) { out[k] = ((f.value >> (16 - 8 * k)) & 255) / 255.0f; } return true; }
		}
		return false;
	};
	w.name = r->name;
	w.cloudsDensity = r->floatOr("clouds density", w.cloudsDensity);
	colour("sky color mult", w.skyColourMult);
	colour("fog color", w.fogColour);
	w.fogEnabled = r->boolOr("fog enabled", w.fogEnabled);
	w.fogDistance = r->floatOr("fog distance min", w.fogDistance);
	w.windSpeed = r->floatOr("wind speed max", w.windSpeed);
	w.windSpeedMin = r->floatOr("wind speed min", w.windSpeed);
	w.heatHaze = r->floatOr("heat haze", w.heatHaze);
	w.dust = r->floatOr("dust", w.dust);
	w.dustInside = r->floatOr("dust inside", w.dustInside);
	w.dustSlope = r->floatOr("dust slope", w.dustSlope);
	w.wetness = r->floatOr("wetness", w.wetness);
	w.rain = r->floatOr("rain intensity", 0.0f) / 100.0f;
	w.windIntensity = r->floatOr("wind intensity", 0.0f);
	if (const kdata::RefCategory* fx = r->findRefs("effects"))
	{
		for (const kdata::RefEntry& e : fx->entries)
		{
			const kdata::Record* ef = gd.find(e.targetID);
			if (!ef || ef->stringOr("particle system").empty()) { continue; }
			Effect o;
			o.system = ef->stringOr("particle system");
			o.type = ef->intOr("type", 0);
			// The EFFECT `type` is the weather effect's class (merged data: 1 ash flakes, 2 steam, 3 dust cloud/twister, 4 sand stream/mist, 5 rain,
			// 7 lightning, 9 beams/poison, 10 detritus); 1 and 5 are the camera-cover classes (particles.cpp setEffects). Rain takes the weather's
			// `rain intensity`, the others its `dust` (1 when 0): MINE, the exe's per-effect strength is not traced.
			// Type 4 (Sand-Stream) is not scaled by `dust` (kenshi-k16.2.54.4): the oracle's rwh.169 count is ONE handler with ~390-560 live particles of
			// quota 900 at night and noon alike. Emission scales the live count linearly (0.5 -> ~384, 0.75 -> ~570 of ours), so 0.63 (~480, the
			// middle of that range) is used. It also gives the night_moons weather/clear ratio 0.78 (game 0.79) and, with the tint, riveroverlook1's
			// band luma closer to the game ref than the old dust-scaled 0.35 (findings/kenshi-k16.2.54.4/sweep). MINE: the exe's strength is not traced.
			o.amount = o.type == 5 ? std::fmin(w.rain, 1.0f) : (o.type == 4 ? 0.63f : (w.dust > 0 ? std::fmin(w.dust, 1.0f) : 1.0f));
			o.windAffected = ef->boolOr("wind affected", false);
			// The ref: v0 = live-instance cap (0 = none), v1 = first delay and min respawn, v2 = max respawn (game-speed seconds, world_effects §1).
			o.cap = e.v0;
			o.delay = (float)e.v1;
			o.respawnMax = (float)e.v2;
			o.ttlMin = ef->floatOr("min time to live", 0.0f);
			o.ttlMax = ef->floatOr("max time to live", 0.0f);
			o.strengthMin = ef->floatOr("effect strength min", 1.0f);
			o.strengthMax = ef->floatOr("effect strength max", 1.0f);
			o.minAltitude = ef->floatOr("min altitude", 0.0f);
			o.maxAltitude = ef->floatOr("max altitude", 0.0f);
			o.minSlope = ef->floatOr("min slope", 0.0f);
			o.maxSlope = ef->floatOr("max slope", 0.0f);
			o.wanderSpeed = ef->floatOr("wandering speed", 0.0f);
			o.radius = ef->floatOr("effect radius", 0.0f);
			o.viewDistance = ef->floatOr("maximum view distance", 0.0f);
			w.effects.push_back(o);
		}
	}
	w.finalize();
	return w;
}

void SkyRamp::retarget(float d, const float c[3], double seconds)
{
	valid = true;
	if (seconds <= 0)
	{
		density = std::clamp(d, 0.0f, 1.0f);
		for (int k = 0; k < 3; k++) { colour[k] = c[k]; }
		updating_ = false;
		return;
	}
	vDensity_ = (float)((d - density) / seconds);
	for (int k = 0; k < 3; k++) { vColour_[k] = (float)((c[k] - colour[k]) / seconds); }
	elapsed_ = 0;
	duration_ = seconds;
	updating_ = true;
}

// 14066f190: the step that crosses the duration still moves (the flag clears first), so the value lands on the target to float precision
void SkyRamp::update(double gameDt)
{
	if (!updating_) { return; }
	elapsed_ += gameDt;
	if (duration_ <= elapsed_) { updating_ = false; }
	density = std::clamp((float)(density + gameDt * vDensity_), 0.0f, 1.0f);
	for (int k = 0; k < 3; k++) { colour[k] = std::max(0.0f, (float)(colour[k] + gameDt * vColour_[k])); }
}

void SkyWeather::finalize()
{
	const float d = cloudsDensity;
	stormDark = std::pow((float)saturate(d - 0.5), 0.3f) * (float)saturate(1.4 * d - 0.3);
	if (fogEnabled && fogDistance > 0)
	{
		fogRho = 1.0f / fogDistance;
		for (int k = 0; k < 3; k++) { fogRaw[k] = fogColour[k]; }
	} else {
		fogRho = stormDark / kStormHazeDistance;
		for (int k = 0; k < 3; k++) { fogRaw[k] = 1.0f - stormDark; }
	}
}

SkyWeather SkyWeather::blend(const std::vector<std::pair<const SkyWeather*, float>>& parts)
{
	SkyWeather o;
	o.name = "blend";
	double total = 0, rho = 0, col[3] = {}, mult[3] = {}, f[10] = {};	// f: clouds, windx, windy, windSpeed, heatHaze, dust, stormDark, dustInside, dustSlope, wetness
	for (const auto& [w, weight] : parts)
	{
		total += weight;
		for (int k = 0; k < 3; k++) { mult[k] += weight * w->skyColourMult[k]; col[k] += weight * w->fogRho * w->fogRaw[k]; }
		rho += weight * w->fogRho;
		f[0] += weight * w->cloudsDensity;
		f[1] += weight * w->wind[0]; f[2] += weight * w->wind[1];
		f[3] += weight * w->windSpeed; f[4] += weight * w->heatHaze; f[5] += weight * w->dust; f[6] += weight * w->stormDark;
		f[7] += weight * w->dustInside; f[8] += weight * w->dustSlope; f[9] += weight * w->wetness;
		o.rain += (float)(weight * w->rain);
		o.windIntensity += (float)(weight * w->windIntensity);
		if (w->windValid && !o.windValid) { o.windValid = true; o.windYaw = w->windYaw; }	// the sim's wind at the camera: every live part carries the same tail
		for (const Effect& e : w->effects)
		{
			auto it = std::find_if(o.effects.begin(), o.effects.end(), [&](const Effect& x) { return x.system == e.system; });
			if (it == o.effects.end()) { it = o.effects.insert(o.effects.end(), e); it->amount = 0; }
			it->amount += (float)(weight * e.amount);
		}
	}
	if (total <= 0) { return o; }
	o.rain = (float)(o.rain / total);
	o.windIntensity = (float)(o.windIntensity / total);
	for (Effect& e : o.effects) { e.amount = (float)(e.amount / total); }
	for (int k = 0; k < 3; k++) { o.skyColourMult[k] = (float)(mult[k] / total); }
	o.cloudsDensity = (float)(f[0] / total);
	o.wind[0] = (float)(f[1] / total); o.wind[1] = (float)(f[2] / total);
	o.windSpeed = (float)(f[3] / total); o.heatHaze = (float)(f[4] / total); o.dust = (float)(f[5] / total); o.stormDark = (float)(f[6] / total);
	o.dustInside = (float)(f[7] / total); o.dustSlope = (float)(f[8] / total); o.wetness = (float)(f[9] / total);
	o.fogRho = (float)(rho / total);
	for (int k = 0; k < 3; k++) { o.fogRaw[k] = rho > 0 ? (float)(col[k] / rho) : 1.0f; }
	o.fogEnabled = o.fogRho > 0;
	o.fogDistance = o.fogEnabled ? 1.0f / o.fogRho : 0.0f;
	for (int k = 0; k < 3; k++) { o.fogColour[k] = o.fogRaw[k]; }
	return o;
}

bool SkyWeather::load(const std::string& dataDir, const std::string& recordName)
{
	auto lower = [](std::string v) { for (char& c : v) { c = (char)std::tolower((unsigned char)c); } return v; };
	try
	{
		const kdata::GameData gd = kworld::loadMergedGameData(dataDir);
		for (const kdata::Record* r : gd.byType(kworld::itemTypeByName("WEATHER")))
		{
			if (lower(r->name) != lower(recordName) && lower(r->stringID) != lower(recordName)) { continue; }
			*this = fromRecord(*r, gd);
			fprintf(stderr, "kclient: weather '%s' (%s): sky mult %.2f %.2f %.2f, clouds %.2f, fog %s colour %.2f %.2f %.2f distance %.0f\n", r->name.c_str(), r->stringID.c_str(),
				skyColourMult[0], skyColourMult[1], skyColourMult[2], cloudsDensity, fogEnabled ? "on" : "off", fogColour[0], fogColour[1], fogColour[2], fogDistance);
			return true;
		}
		fprintf(stderr, "kclient: no WEATHER record named '%s'\n", recordName.c_str());
	}
	catch (const std::exception& e) { fprintf(stderr, "kclient: weather load failed: %s\n", e.what()); }
	return false;
}

std::vector<SkyWeather> SkyWeather::loadAll(const std::string& dataDir)
{
	std::vector<SkyWeather> all;
	try
	{
		const kdata::GameData gd = kworld::loadMergedGameData(dataDir);
		for (const kdata::Record* r : gd.byType(kworld::itemTypeByName("WEATHER")))
		{
			if (!r->name.empty()) { all.push_back(fromRecord(*r, gd)); }
		}
	}
	catch (const std::exception& e) { fprintf(stderr, "kclient: weather list failed: %s\n", e.what()); }
	auto lower = [](std::string v) { for (char& c : v) { c = (char)std::tolower((unsigned char)c); } return v; };
	std::stable_sort(all.begin(), all.end(), [&](const SkyWeather& a, const SkyWeather& b) { return lower(a.name) < lower(b.name); });
	all.erase(std::unique(all.begin(), all.end(), [&](const SkyWeather& a, const SkyWeather& b) { return lower(a.name) == lower(b.name); }), all.end());
	return all;
}

void Sky::init(const ResourceIndex& res)
{
	const float tri[6] = { -1, -1, 3, -1, -1, 3 };
	sg_buffer_desc bd = {};
	bd.data = SG_RANGE(tri);
	bd.label = "sky-tri";
	vbuf_ = sg_make_buffer(&bd);

	uint8_t black[4] = { 0, 0, 0, 255 };
	sg_image_desc id = {};
	id.width = id.height = 1;
	id.pixel_format = SG_PIXELFORMAT_RGBA8;
	id.data = SG_RANGE(black);
	id.label = "sky-black";
	blackImg_ = sg_make_image(&id);
	sg_view_desc vd = {};
	vd.texture.image = blackImg_;
	blackView_ = sg_make_view(&vd);

	// Starfield: SKY_EXE names no star texture. SKY_ASSETS: SkyX_Skydome_STARFIELD samples SkyX_Starfield (4096^2 DXT1 milky way + stars).
	// The cloud layer's three SkyX textures (SKY_ASSETS section 3): CloudsTile noise, Clouds density (red), CloudsNormal. Moons: the textures of the exe's
	// "Moon" / "Moon2" materials.
	static const char* names[6] = { "SkyX_Starfield.dds", "CloudsTile.dds", "moon_HI.dds", "moon2_HI.dds", "Clouds.dds", "CloudsNormal.dds" };
	for (int i = 0; i < 6; i++)
	{
		img_[i] = load_dds(res, names[i], names[i]);
		if (img_[i].id != SG_INVALID_ID)
		{
			vd = {};
			vd.texture.image = img_[i];
			view_[i] = sg_make_view(&vd);
		}
	}
	hasStars_ = view_[0].id != SG_INVALID_ID;
	hasClouds_ = view_[1].id != SG_INVALID_ID && view_[4].id != SG_INVALID_ID && view_[5].id != SG_INVALID_ID;

	const std::string ambPath = res.root() + "/data/newland/land/overlaymaps/ambientmap.png";	// not in a resources.cfg directory (LIGHTING_EXE Q7)
	int aw = 0, ah = 0, ac = 0;
	if (stbi_uc* px = stbi_load(ambPath.c_str(), &aw, &ah, &ac, 4))
	{
		if (aw == 1024 && ah == 1024) { ambMap_.assign(px, px + 4u * 1024 * 1024); }
		stbi_image_free(px);
	}
	if (ambMap_.empty()) { fprintf(stderr, "kclient: ambientmap.png unavailable (%s): ambient and sun multipliers stay 1\n", ambPath.c_str()); }

	sg_sampler_desc sd = {};
	sd.min_filter = sd.mag_filter = SG_FILTER_LINEAR;
	sd.mipmap_filter = SG_FILTER_LINEAR;
	sd.wrap_u = sd.wrap_v = SG_WRAP_REPEAT;
	sd.max_anisotropy = 4;
	smp_ = sg_make_sampler(&sd);

	shd_ = sg_make_shader(sky_shader_desc(sg_query_backend()));
	sg_pipeline_desc pd = {};
	pd.shader = shd_;
	pd.layout.attrs[ATTR_sky_pos].format = SG_VERTEXFORMAT_FLOAT2;
	pd.depth.compare = SG_COMPAREFUNC_ALWAYS;
	pd.depth.write_enabled = false;
	pd.cull_mode = SG_CULLMODE_NONE;
	pd.label = "sky";
	pip_ = sg_make_pipeline(&pd);

	recompute();
}

void Sky::shutdown()
{
	for (int i = 0; i < 6; i++)
	{
		if (view_[i].id != SG_INVALID_ID) { sg_destroy_view(view_[i]); }
		if (img_[i].id != SG_INVALID_ID) { sg_destroy_image(img_[i]); }
	}
	sg_destroy_view(blackView_);
	sg_destroy_image(blackImg_);
	sg_destroy_sampler(smp_);
	sg_destroy_pipeline(pip_);
	sg_destroy_shader(shd_);
	sg_destroy_buffer(vbuf_);
}

// ambientmap.png (the exe-baked BIOMES `ambient light` rgb + `sun brightness` x 127.5 alpha, LIGHTING_EXE Q7): covers world x, z in +-147456, 288 units per texel,
// u follows x, v (row) follows z, clamp addressing. deferred.hlsl:166-167 samples it per pixel; here it is sampled once at the camera (the map is smooth over a view).
void Sky::sampleAmbientMap(const double pos[3], float out[4]) const
{
	constexpr int kN = 1024;
	constexpr double kWorld = 294912.0;
	for (int k = 0; k < 4; k++) { out[k] = 1.0f; }
	if (ambMap_.empty()) { return; }
	const double fx = std::fmin(std::fmax((0.5 + pos[0] / kWorld) * kN - 0.5, 0.0), kN - 1.0), fy = std::fmin(std::fmax((0.5 + pos[2] / kWorld) * kN - 0.5, 0.0), kN - 1.0);
	const int x0 = (int)fx, y0 = (int)fy, x1 = std::min(x0 + 1, kN - 1), y1 = std::min(y0 + 1, kN - 1);
	const double tx = fx - x0, ty = fy - y0;
	for (int k = 0; k < 4; k++)
	{
		auto px = [&](int x, int y) { return (double)ambMap_[((size_t)y * kN + x) * 4 + k]; };
		const double v = (px(x0, y0) * (1 - tx) + px(x1, y0) * tx) * (1 - ty) + (px(x0, y1) * (1 - tx) + px(x1, y1) * tx) * ty;
		out[k] = (float)(v / 255.0 * (k == 3 ? 2.0 : 1.0));
	}
}

void Sky::update(double realDt, float fogDistance, const double camPos[3])
{
	if (ambientBlurKm > 0)
	{
		// the same 9x9 Gaussian stencil as WeatherMap::blend (full width at half maximum = ambientBlurKm)
		const double sigma = ambientBlurKm * 1000.0 / 2.355, step = sigma * 0.5;
		double acc[4] = {}, total = 0;
		for (int j = -4; j <= 4; j++)
		{
			for (int i = -4; i <= 4; i++)
			{
				const double p[3] = { camPos[0] + i * step, 0, camPos[2] + j * step }, w = std::exp(-0.125 * (i * i + j * j));
				float v[4];
				sampleAmbientMap(p, v);
				for (int k = 0; k < 4; k++) { acc[k] += w * v[k]; }
				total += w;
			}
		}
		for (int k = 0; k < 4; k++) { ambMult_[k] = (float)(acc[k] / total); }
	} else {
		sampleAmbientMap(camPos, ambMult_);
	}
	clock.advance(realDt);
	// C124 (SKY2_EXE Q3): wind accumulates velocity x the game dt (real dt x speed; 0 while paused)
	const double gameDt = clock.paused ? 0.0 : realDt * clock.speed;
	for (int k = 0; k < 2; k++) { windAcc_[k] += weather.wind[k] * gameDt; }
	fogDist_ = fogDistance;
	recompute();
}

// Sun, sun colour, ambient, exposure and fog from the clock. Follows the exe (SKY_EXE sections 1, 3, 4; SKY2_EXE Q2, Q3, Q5; SKY3_EXE Q2); the marked parts are mine.
void Sky::recompute()
{
	sky_sun_direction(clock.hours, k_.sunrise, k_.sunset, k_.latitude, sunReal_);
	const double y = sunReal_[1];

	// C40: sunDirection is the real direction clamped to the horizon and renormalised
	double cd[3] = { sunReal_[0], std::fmax(y, 0.0), sunReal_[2] };
	const double cl = std::sqrt(cd[0] * cd[0] + cd[1] * cd[1] + cd[2] * cd[2]);
	for (double& v : cd) { v /= cl; }

	// C44/C45: sun intensity alpha, c = 1
	alpha_ = (float)(y > 0 ? 0.5 + 0.5 * y : std::fmax(0.0, 0.5 * (1.0 + 10.8 * y)));

	// sunColour.rgb = getColorAt(sunDirReal + (0,0,0.04)) / (Exposure x 4), zero below y = -0.2 (SKY2 Q2). atm_sky already applies the atmosphere exposure,
	// so the divisor leaves 1/4 of the raw sum. The sample direction is NOT clamped to the horizon: with the sun below it the ray dips under the horizon and getColorAt
	// reddens it as the live game does (docs/reference/LIGHTING.md section 9; a clamp there gave a 3x too dim red at 23:13).
	const double ln = std::sqrt(sunReal_[0] * sunReal_[0] + y * y + sunReal_[2] * sunReal_[2]);
	const double l[3] = { sunReal_[0] / ln, y / ln, sunReal_[2] / ln };
	double sunColour[3] = {};
	if (y >= kSunMinY)
	{
		double d[3] = { sunReal_[0] + kSunSampleOffset[0], y + kSunSampleOffset[1], sunReal_[2] + kSunSampleOffset[2] };
		const double dl = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
		for (double& v : d) { v /= dl; }
		atm_sky(d, l, kColourSamples, sunColour);
		for (double& v : sunColour) { v /= kAtmExposure * 4.0; }
	}

	// Ambient. "zenithLight" = max(getColorAt(UNIT_Z) x sky+0x48, base) x sunColour.g; "nadirLight" = max(getColorAt(UNIT_Y) x sky+0x48, base) (SKY2 Q2, Q5).
	const double unitZ[3] = { 0, 0, 1 }, unitY[3] = { 0, 1, 0 };
	double zen[3], nad[3];
	atm_sky(unitZ, l, kColourSamples, zen);
	atm_sky(unitY, l, kColourSamples, nad);
	for (int k = 0; k < 3; k++)
	{
		zen[k] = std::fmax(zen[k] * weather.skyColourMult[k], kAmbientBase[k]) * sunColour[1];
		nad[k] = std::fmax(nad[k] * weather.skyColourMult[k], kAmbientBase[k]);
	}

	if (getenv("KSUNLOG"))
	{
		fprintf(stderr, "SUNLOG t=%.2f dir %.5f %.5f %.5f sunColour %.4f %.4f %.4f w %.4f zenith %.4f %.4f %.4f nadir %.4f %.4f %.4f amb %.3f %.3f %.3f %.3f dust %.3f/%.3f/%.3f fogRho %.3g fogRaw %.3f %.3f %.3f\n", clock.hours, sunReal_[0], y, sunReal_[2],
			sunColour[0], sunColour[1], sunColour[2], alpha_, zen[0], zen[1], zen[2], nad[0], nad[1], nad[2], ambMult_[0], ambMult_[1], ambMult_[2], ambMult_[3], weather.dust, weather.dustSlope, weather.dustInside, weather.fogRho, weather.fogRaw[0], weather.fogRaw[1], weather.fogRaw[2]);
	}

	for (int k = 0; k < 3; k++) { sunColour_[k] = (float)sunColour[k]; }

	// Sun light in the world shaders = sunColour.rgb x alpha. The exe passes rgb and alpha separately and the light consumer is not traced: this is my reading.
	double sun[3];
	for (int k = 0; k < 3; k++) { sun[k] = sunColour[k] * alpha_ * ambMult_[3]; }

	// Exposure (SKY2 Q3): exposure = key / adapted, adapted = clamp(scene, MIN, MAX), MIN = exposureMin x (nd + (1 - nd) x sat(5y)). Larger nightDarkness = higher floor
	// = lower night exposure = darker. The scene luminance is not read back: it is estimated from the lights (kNoonSceneLum).
	minLum_ = (float)(k_.exposureMin * (k_.nightDarkness + (1.0 - k_.nightDarkness) * saturate(y * 5.0)));
	maxLum_ = k_.exposureMax;
	double amb[3];
	for (int k = 0; k < 3; k++) { amb[k] = zen[k] + nad[k]; }
	sceneLum_ = (float)(kNoonSceneLum * (0.6 * luma(amb) + 0.4 * luma(sun)));
	const float adapted = std::fmax(minLum_, std::fmin(maxLum_, sceneLum_));
	exposure_ = (float)(kExposureKey / adapted);

	// Atmosphere fog (approximation, mine): the exe's per-pixel scatter term is not reproduced; the constant scene fog colour lit by the sky stands in.
	// Both fog colours below stay display referred and unexposed (the game's raw values, no gamma): the world shaders mix them into the scene and Hdr::composite
	// applies the exposure once (k16.2.81; the old display encode here double-counted it and gamma-lifted the colour).
	float fog[3], glob[3];
	// horizonClouds (LIGHTING_EXE Q6, atmospherefog.hlsl:95): fog colour = lerp(atmosphere, horizonClouds.rgb, horizonClouds.w), horizonClouds.rgb = sat((zenithLight +
	// sunColour.rgb x s) x (1 - overcast)) x sqrt(Exposure), .w = sat(density x 1.4 - 0.3), coverage = density x 1.4 - 0.8, overcast = pow(sat(density - 0.5), 0.3),
	// s = 1 - 0.1 (coverage + 0.2) x 3.0 (kHcCloudScale): 1.18 clear, 0.76 at density 1. The earlier FIT (0.68 at w = 1, linear in w) came from one 10:00 acid / blood
	// rain sample; the live Heavy_Rain deadlands of rwh.140 (08:27 and 12:00, density 1) gives s = .760 / .759 and the formula holds at every density seen.
	const double hcW = saturate(weather.cloudsDensity * 1.4 - 0.3), hcExp = std::sqrt(kAtmExposure), hcSun = 1.0 - 0.1 * (weather.cloudsDensity * 1.4 - 0.8 + 0.2) * kHcCloudScale, overcast = std::pow(saturate(weather.cloudsDensity - 0.5), 0.3);
	double hcLog[3];
	for (int k = 0; k < 3; k++)
	{
		const double hc = hcLog[k] = saturate((zen[k] + sunColour[k] * hcSun) * (1.0 - overcast)) * hcExp;
		fog[k] = (float)(kAtmFogColour[k] * (0.65 * amb[k] + 0.35 * sun[k]) * (1.0 - hcW) + hc * hcW);	// display referred, unexposed (fogvolume.cpp's far colour)
	}
	if (getenv("KSUNLOG")) { fprintf(stderr, "HCLOG horizonClouds %.4f %.4f %.4f w %.3f\n", hcLog[0], hcLog[1], hcLog[2], hcW); }

	// Global weather fog (SharedFogParams: fogColour, fogDensity = 1 / distance; fog.hlsl multiplies the colour by sunColour.w). That factor is 0 at night, which
	// makes the exe's night fog black, yet the in-game night in a dust storm is red-brown: APPROXIMATION, the light factor is max(sunColour.w, nadirLight) per
	// channel, i.e. fog colour x nadir ambient at night. Untraced in the exe. The colour is the raw weather fogColour x that factor, display referred and unexposed, clamped
	// as the regional path's map colour is (lighting.inc light_finish, k16.2.81): the composite's exposure applies once.
	for (int k = 0; k < 3; k++) { glob[k] = (float)std::fmin(std::fmax(weather.fogRaw[k] * std::fmax((double)alpha_, nad[k]), 0.0), 1.0); }
	const float fogRho = fogRhoForce > 0 ? fogRhoForce : weather.fogRho;
	const bool fogOn = fogRho > 0;

	LightParams& L = light_;
	for (int k = 0; k < 3; k++)
	{
		L.v[0][k] = (float)cd[k];
		L.v[1][k] = (float)sun[k];
		L.v[2][k] = (float)zen[k];
		L.v[3][k] = (float)nad[k];
		L.v[4][k] = fog[k];
		L.v[6][k] = glob[k];
	}
	for (int k = 0; k < 3; k++) { L.v[7][k] = ambMult_[k]; }
	L.v[8][0] = weather.wetness;
	L.v[8][1] = weather.dust;
	L.v[8][2] = weather.dustSlope;
	L.v[8][3] = weather.dustInside;
	// horizonClouds.rgb is the game's display-referred HDR uniform, blended into the scene before the composite's exposure and clip (atmospherefog.hlsl:95); the
	// live read at 22:40 is (1.183, 0.868, 0.498) = exactly our hcLog. The retired display encode (exposure, then pow 1/2.2) clamped it to 1 and gamma-lifted it
	// to (1.000, 1.000, 0.874), the wrong colour for that mix.
	for (int k = 0; k < 3; k++) { L.v[9][k] = (float)sunReal_[k]; L.v[10][k] = (float)hcLog[k]; }
	L.v[9][3] = (float)hcW;
	L.v[7][3] = regionalFog ? 1.0f : 0.0f;	// 1: the shaders read the global fog from the weather map (lighting.inc)
	L.v[0][3] = 0;
	L.v[1][3] = alpha_;
	L.v[2][3] = (float)kEnvColourW;
	L.v[3][3] = (float)kAmbientParamsW;
	L.v[4][3] = 1.0f;	// unexposed scene (x1): the exposure moves to Hdr::composite (Sky::exposure() stays the CPU estimate, diagnostics only)
	L.v[5][0] = kFogStartFrac * fogDist_;
	L.v[5][1] = kFogEndFrac * fogDist_;
	L.v[5][2] = 0.0f;	// retired (k16.2.83): was kWorldLightScale (the water light scale); no shader reads the slot, kept zero
	L.v[5][3] = fogRho;
	L.v[6][3] = fogOn ? 1.0f : 0.0f;
	L.sh[0][3] = 0;
	if (skyAmbient > 0) { ambientSH(l); }

	// Star field (SKY4_EXE): no rotation. SkyX scrolls the dome's star UV by uTime x 0.1 on both axes, uTime = 0.5 x H, H = SkyX+0x7c (kenshi-inspect
	// findings/sky.md): game hours played since launch, speed scaled, frozen on pause, not moved by a clock set, never wrapped, not saved. Here from 0 at
	// start; a clock step over half an hour in one frame counts as a set.
	const double hours = clock.day * 24.0 + clock.hours;
	if (lastHours_ >= 0 && hours > lastHours_ && hours - lastHours_ < 0.5) { skyHours_ += hours - lastHours_; }
	lastHours_ = hours;
	starScroll_ = 0.05 * skyHours_;
}

// Dynamic ambient (ours, CLIENT_RENDER_NEXT.md 3.1): the irradiance of the sky the player sees plus the ground's bounce, as 9 SH coefficients (lighting.inc
// light_irradiance), in place of the game's static cube mp_irradiance.dds. Sky: the Rayleigh sky model (no sun glare) x the weather's sky colour mult, floored like
// the exe's ambient samples. Ground: a Lambertian plane of the colour map's albedo lit by the sun and by this sky's own up-facing irradiance, converted into the
// cube's units (the shader multiplies the ambient by amb_mult x the daylight clamp; the sun term is pi N.L sun_col). At noon the mean equals the cube's, so the
// exposure chain stays balanced for it; other hours keep the sky's own level (dark at night), and the ground only moves hue and direction.
void Sky::ambientSH(const double l[3])
{
	constexpr int kDirs = 256;	// MINE: Fibonacci sphere samples; the sky model is smooth without the Mie lobe
	struct Coef { double c[9][3] = {}; };
	auto basis = [](const double d[3], double y[9])
	{
		y[0] = 0.282095;
		y[1] = 0.488603 * d[1]; y[2] = 0.488603 * d[2]; y[3] = 0.488603 * d[0];
		y[4] = 1.092548 * d[0] * d[1]; y[5] = 1.092548 * d[1] * d[2]; y[6] = 0.315392 * (3.0 * d[2] * d[2] - 1.0);
		y[7] = 1.092548 * d[0] * d[2]; y[8] = 0.546274 * (d[0] * d[0] - d[1] * d[1]);
	};
	// radiance -> irradiance polynomial coefficients: the cosine lobe's A_l (pi, 2 pi / 3, pi / 4) x the basis constant, so the shader evaluates bare monomials
	auto irradiance = [](const Coef& L, Coef& E)
	{
		static const double k[9] = { kPi * 0.282095, 2.0 * kPi / 3.0 * 0.488603, 2.0 * kPi / 3.0 * 0.488603, 2.0 * kPi / 3.0 * 0.488603,
			kPi / 4.0 * 1.092548, kPi / 4.0 * 1.092548, kPi / 4.0 * 0.315392, kPi / 4.0 * 1.092548, kPi / 4.0 * 0.546274 };
		for (int i = 0; i < 9; i++) { for (int c = 0; c < 3; c++) { E.c[i][c] = L.c[i][c] * k[i]; } }
	};
	auto eval = [](const Coef& E, const double n[3], double out[3])
	{
		for (int c = 0; c < 3; c++)
		{
			out[c] = E.c[0][c] + E.c[1][c] * n[1] + E.c[2][c] * n[2] + E.c[3][c] * n[0] + E.c[4][c] * n[0] * n[1] + E.c[5][c] * n[1] * n[2]
				+ E.c[6][c] * (3.0 * n[2] * n[2] - 1.0) + E.c[7][c] * n[0] * n[2] + E.c[8][c] * (n[0] * n[0] - n[1] * n[1]);
		}
	};

	// the sky's upper hemisphere
	static double dirs[kDirs][3];
	static bool made = false;
	if (!made)
	{
		const double golden = kPi * (3.0 - std::sqrt(5.0));
		for (int i = 0; i < kDirs; i++)
		{
			const double y = 1.0 - (i + 0.5) * 2.0 / kDirs, r = std::sqrt(1.0 - y * y), a = golden * i;
			dirs[i][0] = r * std::cos(a); dirs[i][1] = y; dirs[i][2] = r * std::sin(a);
		}
		made = true;
	}
	const double w = 4.0 * kPi / kDirs;
	auto skyOf = [&](const double sun[3], const double mult[3], Coef& sky)
	{
		for (const double* d : dirs)
		{
			if (d[1] < 0) { continue; }
			double rad[3], y[9];
			atm_sky(d, sun, kColourSamples, rad, false);
			basis(d, y);
			for (int c = 0; c < 3; c++)
			{
				rad[c] = std::fmax(rad[c] * mult[c], kAmbientBase[c]);
				for (int i = 0; i < 9; i++) { sky.c[i][c] += rad[c] * y[i] * w; }
			}
		}
	};
	// White balance: the Rayleigh model's raw hue is far bluer than the game's cube, so the sky is taken relative to our own noon sky (no weather tint), whose mean
	// becomes the cube's mean colour: at noon the average ambient is the game's, and only the direction (sky above, ground below), the hour and the weather move it.
	double noon[3];
	sky_sun_direction(12.0, k_.sunrise, k_.sunset, k_.latitude, noon);
	const double nl = std::sqrt(noon[0] * noon[0] + noon[1] * noon[1] + noon[2] * noon[2]);
	for (double& v : noon) { v /= nl; }
	const double one[3] = { 1, 1, 1 };
	Coef ref, refE, sky, skyE;
	skyOf(noon, one, ref);
	irradiance(ref, refE);
	const double mult[3] = { weather.skyColourMult[0], weather.skyColourMult[1], weather.skyColourMult[2] };
	skyOf(l, mult, sky);
	static const double kCubeMean[3] = { 1.1328, 1.2420, 1.4963 };	// the six lighting.inc kIrr faces' mean
	for (int c = 0; c < 3; c++)
	{
		const double wb = kCubeMean[c] / std::fmax(refE.c[0][c], 1e-12);
		for (auto& row : sky.c) { row[c] *= wb; }
	}
	irradiance(sky, skyE);
	// Moonlight (post.moonlight, ours; user 2026-11-07): the sky model has no moon, so night is lamps only. Where the sky's mean irradiance falls below this
	// fraction of the cube's, a uniform upper-hemisphere radiance makes up the difference: 1 matches the original at night (its fixed cube), day is untouched.
	if (moonlight > 0)
	{
		Coef dome, domeE;
		for (const double* d : dirs)
		{
			if (d[1] < 0) { continue; }
			double y[9];
			basis(d, y);
			for (int i = 0; i < 9; i++) { for (int c = 0; c < 3; c++) { dome.c[i][c] += y[i] * w; } }
		}
		irradiance(dome, domeE);
		bool lifted = false;
		for (int c = 0; c < 3; c++)
		{
			const double gap = moonlight * kCubeMean[c] - skyE.c[0][c];
			if (gap <= 0) { continue; }
			for (int i = 0; i < 9; i++) { sky.c[i][c] += dome.c[i][c] * gap / domeE.c[0][c]; }
			lifted = true;
		}
		if (lifted) { irradiance(sky, skyE); }
	}
	// The white balance has put the sky in the cube's units already (noon's mean is the cube's): no level scale here, so the ambient
	// follows the hour. Rescaling every hour to the cube's level lifted midnight's faint red horizon scatter to noon brightness (user
	// 2026-11-07: the Hub glowing red at 00:09).

	// the ground's lower hemisphere: albedo / pi x (sun + sky) irradiance on a level plane, in cube units (the shader's ambient factor divided out of the sun)
	Coef total = sky;
	if (hasGround)
	{
		constexpr double kBounce = 0.5;	// ours (user 2026-11-05: the full sand bounce tinted every shade orange)
		const double up[3] = { 0, 1, 0 };
		double skyUp[3];
		eval(skyE, up, skyUp);
		const double day = std::clamp(5.0 * light_.v[0][1] + 0.2, 0.1, 1.0);
		double rad[3];
		for (int c = 0; c < 3; c++)
		{
			const double sunIrr = kPi * std::fmax(light_.v[0][1], 0.0) * light_.v[1][c] * light_.v[3][3] / (day * std::fmax((double)light_.v[7][c], 1e-3));
			rad[c] = kBounce * groundAlbedo[c] / kPi * (sunIrr + std::fmax(skyUp[c], 0.0));
		}
		for (const double* d : dirs)
		{
			if (d[1] >= 0) { continue; }
			double y[9];
			basis(d, y);
			for (int c = 0; c < 3; c++) { for (int i = 0; i < 9; i++) { total.c[i][c] += rad[c] * y[i] * w; } }
		}
	}
	Coef E;
	irradiance(total, E);
	const double scale = luma(skyE.c[0]) / std::fmax(luma(E.c[0]), 1e-12);	// the ground bounce shifts hue and direction, not the sky's level
	for (int i = 0; i < 9; i++) { for (int c = 0; c < 3; c++) { light_.sh[i][c] = (float)(E.c[i][c] * scale); } }
	light_.sh[0][3] = std::clamp(skyAmbient, 0.0f, 1.0f);
}

SkyFrame Sky::frame() const
{
	SkyFrame s;
	s.light = light_;
	for (int k = 0; k < 3; k++)
	{
		s.sunDir[k] = sunReal_[k];
		s.sunColour[k] = sunColour_[k];
		s.skyColourMult[k] = weather.skyColourMult[k];
	}
	s.starScroll = starScroll_;
	s.hours = clock.hours;
	s.day = clock.day;
	s.cloudsDensity = weather.cloudsDensity;
	s.clouds = cloudsOn;
	s.windAcc[0] = windAcc_[0];
	s.windAcc[1] = windAcc_[1];
	s.minLum = minLum_;
	s.maxLum = maxLum_;
	s.heatHaze = weather.heatHaze;
	return s;
}

void Sky::render(const SkyFrame& s, const double f[3], const double r[3], const double u[3], float fovY, float aspect, int viewHeight) const
{
	sky_params_t p = {};
	const float th = std::tan(fovY * 0.5f);
	for (int k = 0; k < 3; k++)
	{
		p.cam_f[k] = (float)f[k];
		p.cam_r[k] = (float)r[k] * th * aspect;
		p.cam_u[k] = (float)u[k] * th;
		p.sun_real[k] = (float)s.sunDir[k];
	}
	p.star_axis[3] = (float)std::fmod(s.starScroll, 1.0);	// the star texture wraps
	const Atm& a = atm();
	p.atm0[0] = (float)kInnerRadius; p.atm0[1] = (float)kOuterRadius; p.atm0[2] = (float)(kInnerRadius + kHeight); p.atm0[3] = (float)kScaleDepth;
	p.atm1[0] = (float)a.kr4pi; p.atm1[1] = (float)a.km4pi; p.atm1[2] = (float)a.kresun; p.atm1[3] = (float)a.kmesun;
	for (int k = 0; k < 3; k++) { p.atm2[k] = (float)a.invWl[k]; p.sky_mult[k] = s.skyColourMult[k]; }
	p.atm2[3] = (float)kAtmExposure;
	p.atm3[0] = (float)kG; p.atm3[1] = (float)a.scale;
	p.night[0] = hasStars_ ? 1.0f : 0.0f;
	// moons: angular radius = atan(size x tan(0.01 deg) x mesh radius); spin about +Y = floor(total game minutes) x spin / 1440
	const double tanT = std::tan(0.01 * kPi / 180.0);
	auto unit = [](const double v[3], float out[4], double radius)
	{
		const double l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
		out[0] = (float)(v[0] / l); out[1] = (float)(v[1] / l); out[2] = (float)(v[2] / l); out[3] = (float)radius;
	};
	const double radius[2] = { std::atan(kMoon1Size * tanT * kMoonMeshRadius), std::atan(kMoon2Size * tanT * kMoonMeshRadius) };
	unit(kMoon1, p.moon_a, radius[0]);
	unit(kMoon2, p.moon_b, radius[1]);
	// +1e-3: a clock set to a whole minute (the scene's time=01:11, the sim's float hour) lands within a few 1e-5 of the boundary, and the floor
	// flipped a minute (moon 2 spins 0.0033 rad per minute) with the last bit of the hour (kenshi-k16.2.328.1)
	const double minutes = std::floor((s.hours + s.day * 24.0) * 60.0 + 1e-3);
	p.moon_c[0] = (float)std::fmod(minutes * (kMoon1Spin / 1440.0), 2.0 * kPi);
	p.moon_c[1] = (float)std::fmod(minutes * (kMoon2Spin / 1440.0), 2.0 * kPi);
	// texture LOD: the lit hemisphere spans half the equirect width, over the disc's diameter in pixels
	const double pxPerTan = 0.5 * viewHeight / th;
	for (int i = 0; i < 2; i++) { p.moon_c[2 + i] = (float)std::fmax(0.0, std::log2(kMoonTexWidth[i] * 0.5 / (2.0 * std::tan(radius[i]) * pxPerTan))); }
	// Clouds: the exe's two per-frame formulas for the layer fields (C35), d = density: the density offset and the darkness (live: uDensityOffset,
	// uDarkness; kenshi-inspect findings/sky_dawn_exposure.md).
	const double d = s.cloudsDensity;
	p.cloud0[0] = (float)(d * 1.4 - 0.8);
	p.cloud0[1] = (float)std::pow(saturate(d - 0.5), 0.3);
	p.cloud0[3] = s.clouds && hasClouds_ ? 1.0f : 0.0f;
	p.cloud1[0] = (float)std::fmod(s.windAcc[0] * kCloudWind, 1.0); p.cloud1[1] = (float)std::fmod(s.windAcc[1] * kCloudWind, 1.0);
	for (int k = 0; k < 3; k++) { p.cloud_sun[k] = s.sunColour[k]; }

	drawstats::pipeline(pip_);
	sg_bindings b = {};
	b.vertex_buffers[0] = vbuf_;
	b.views[VIEW_star_tex] = hasStars_ ? view_[0] : blackView_;
	b.views[VIEW_cloud_tile_tex] = hasClouds_ ? view_[1] : blackView_;
	b.views[VIEW_cloud_tex] = hasClouds_ ? view_[4] : blackView_;
	b.views[VIEW_cloud_nrm_tex] = hasClouds_ ? view_[5] : blackView_;
	b.views[VIEW_moon1_tex] = view_[2].id != SG_INVALID_ID ? view_[2] : blackView_;
	b.views[VIEW_moon2_tex] = view_[3].id != SG_INVALID_ID ? view_[3] : blackView_;
	b.samplers[SMP_sky_smp] = smp_;
	drawstats::bindings(&b);
	sg_apply_uniforms(UB_sky_params, SG_RANGE(p));
	sg_apply_uniforms(UB_light_params, { &s.light, sizeof s.light });
	drawstats::draw(0, 3, 1);
}

std::string Sky::describe() const
{
	char buf[500];
	const float (*v)[4] = light_.v;
	snprintf(buf, sizeof buf, "sun dir %.3f %.3f %.3f alpha %.2f exposure %.2f (scene %.2f, min %.2f max %.2f) sun rgb %.2f %.2f %.2f zenith %.3f %.3f %.3f nadir %.3f %.3f %.3f fog %.2f %.2f %.2f wet %.2f dust %.2f/%.2f/%.2f weather %s",
		sunReal_[0], sunReal_[1], sunReal_[2], alpha_, exposure_, sceneLum_, minLum_, maxLum_, v[1][0], v[1][1], v[1][2], v[2][0], v[2][1], v[2][2], v[3][0], v[3][1], v[3][2], v[4][0], v[4][1], v[4][2], v[8][0], v[8][1], v[8][2], v[8][3], weather.name.c_str());
	return buf;
}
