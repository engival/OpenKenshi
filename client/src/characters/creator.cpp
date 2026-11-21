#include "characters/creator.hpp"

#include <algorithm>
#include <map>
#include <random>

// The posture sliders are Spec fields (the posture clips), not shape values
static float Characters::Spec::* postureField(const std::string& slider)
{
	return slider == "Posture" ? &Characters::Spec::posture : slider == "Shoulder set" ? &Characters::Spec::shoulderSet : slider == "Neck position" ? &Characters::Spec::neckPosition : nullptr;
}
// The original's default appearance (140072c50) holds Posture at a fixed 20, not the human range's mid 35 (findings/charedit.md (2)): RESET and a
// race / sex change show that body.
constexpr float kDefaultPosture = 20;

bool Creator::open(Characters& chars, const double pos[3], float yaw, const std::string& race, bool female, std::string& err)
{
	spec_ = Characters::Spec();
	for (int k = 0; k < 3; k++) { spec_.pos[k] = pos[k]; }
	spec_.yaw = yaw;
	spec_.loco = true;
	spec_.move = 0;	// standing: the idle chooser plays spec.stance (the editor's Idle stance)
	chars_ = &chars;
	if (!setRace(race, female, err) || !chars.add(spec_, err)) { chars_ = nullptr; return false; }
	index_ = chars.size() - 1;
	return true;
}

bool Creator::apply(std::string& err)
{
	if (!chars_) { err = "creator not open"; return false; }
	Characters::Spec shown = spec_;
	if (!clothes) { shown.wear.clear(); shown.weapons.clear(); }
	return chars_->replace(index_, shown, err);
}

bool Creator::setRace(const std::string& race, bool female, std::string& err)
{
	if (!chars_) { err = "creator not open"; return false; }
	Characters::RaceChoices c = chars_->choices(race);
	if (!c.ok) { err = "creator: unknown race " + race; return false; }
	choices_ = std::move(c);
	spec_.race = race;
	spec_.female = female;
	spec_.head = 0;
	spec_.hair.clear();
	spec_.beard.clear();
	if (!choices_.hairColours.empty()) { spec_.hairColour = choices_.hairColours[0]; }
	// RESET / a new race or sex: the original's default appearance. Every <Range> at its xml mid (the parser's round(min + (max-min)/2); so Arm bulk
	// 108, not 100: 70 + 75/2 = 107.5), except Posture at the default appearance's fixed 20, and the shape map clears so value() answers the mids.
	spec_.shape.clear();
	for (const Characters::EditorRange& r : ranges())
	{
		if (r.name == "Posture") { spec_.posture = kDefaultPosture; }
		else if (const auto f = postureField(r.name)) { spec_.*f = r.mid; }
	}
	return true;
}

// The RESET button after the original (ResetButton -> 1405ef870 -> 14007a490(defaults, appearance, 0); kenshi-rwh.233): every Range at its mid (Posture 20),
// Skin Tone 0 (palette[0]), the default head, the normal idle stance. Hair, beard, hair colour and Age stay.
bool Creator::reset(std::string& err)
{
	if (!chars_) { err = "creator not open"; return false; }
	const std::string hair = spec_.hair, beard = spec_.beard, hairColour = spec_.hairColour;
	float age = 0;
	bool hasAge = false;
	for (const auto& [name, v] : spec_.shape) { if (name == "Age") { age = v; hasAge = true; } }
	if (!setRace(spec_.race, spec_.female, err)) { return false; }
	spec_.hair = hair;
	spec_.beard = beard;
	spec_.hairColour = hairColour;
	if (hasAge) { setValue("Age", age); }
	spec_.head = 0;
	spec_.skin = 0;
	spec_.stance = Characters::Spec().stance;
	return true;
}

// The editor's Skin Tone slider top in tenths: (swatch count - 1) * 10 (140077090; kinspect rwh47_skin_tone_scale_static.md).
int Creator::skinMax() const
{
	const int stops = choices_.skinStops[spec_.female ? 1 : 0];
	return stops > 0 ? 10 * (stops - 1) : 0;
}

bool Creator::defaultLook(const std::string& race, bool female, std::string& err)
{
	if (!chars_) { err = "creator not open"; return false; }
	// a fresh Spec (no wear, weapons or hair of the member shown before it), the preview where it stands, standing on its own idle
	const double pos[3] = { spec_.pos[0], spec_.pos[1], spec_.pos[2] };
	const float yaw = spec_.yaw;
	spec_ = Characters::Spec();
	for (int k = 0; k < 3; k++) { spec_.pos[k] = pos[k]; }
	spec_.yaw = yaw;
	spec_.loco = true;
	spec_.move = 0;
	return setRace(race, female, err);
}

float Creator::value(const std::string& slider) const
{
	if (const auto f = postureField(slider)) { return spec_.*f; }
	for (const auto& [name, v] : spec_.shape) { if (name == slider) { return v; } }
	for (const Characters::EditorRange& r : ranges()) { if (r.name == slider) { return r.mid; } }
	return 0;
}

void Creator::setValue(const std::string& slider, float v)
{
	for (const Characters::EditorRange& r : ranges()) { if (r.name == slider) { v = std::clamp(v, std::min(r.min, r.max), std::max(r.min, r.max)); } }
	if (const auto f = postureField(slider)) { spec_.*f = v; return; }
	for (auto& [name, old] : spec_.shape) { if (name == slider) { old = v; return; } }
	spec_.shape.push_back({ slider, v });
}

// RANDOMISE after the original (findings/charedit.md, 1405ef930 / 14006fcf0): every editor Range gets round(mid + variation * (2u - 1)), u uniform in [0, 1),
// variation = round((max - min) * random_variation / 100), or (max - min) * 0.2 without one; Ranges of one random_group share their u. The head is uniform
// over the race's heads, hair and beard weighted by the race's `hairs` (the empty record is the none), skin tone a uniform integer over the ColourRange
// (0..skinMax(), tenths), the hair colour weighted by the race's `hair colors` (val0, 0 = 100; charedit8b_static.md). Posture, Age and the idle stance never change.
// The exe turns the picked COLOR_DATA into Hair Colour / Saturation / Brightness sliders (HSB x 100); the spec keeps the record. The face morph pass
// (14007b7c0, one side of each pair 0) is the signed Face ranges here, so it goes through the Range formula too.
void Creator::randomize(uint32_t seed, int part)
{
	std::mt19937 rng(seed);
	const auto unit = [&]() { return std::uniform_real_distribution<float>(0, 1)(rng); };
	// a weighted index over weights (the none first), -1 = none
	const auto weighted = [&](float noneWeight, const std::vector<float>& weights) -> int
	{
		float total = noneWeight;
		for (const float w : weights) { total += w; }
		if (total <= 0) { return -1; }
		float at = unit() * total;
		if (at < noneWeight) { return -1; }
		at -= noneWeight;
		for (size_t i = 0; i < weights.size(); i++) { if (at < weights[i]) { return (int)i; } at -= weights[i]; }
		return (int)weights.size() - 1;
	};
	const auto has = [&](int p) { return part == kAll || part == p; };
	std::map<int, float> groupU;
	for (const Characters::EditorRange& r : ranges())
	{
		if (r.name == "Posture" || r.name == "Age" || !has(r.category == "Face" ? kFace : kBody)) { continue; }
		float u;
		if (r.group < 0) { u = unit(); }
		else { u = groupU.try_emplace(r.group, unit()).first->second; }
		const float variation = std::round((r.max - r.min) * (r.variation > 0 ? r.variation / 100.0f : 0.2f));
		setValue(r.name, std::round(r.mid + variation * (2 * u - 1)));
	}
	if (has(kFace))
	{
		const int heads = choices_.heads[spec_.female ? 1 : 0];
		spec_.head = heads > 0 ? std::uniform_int_distribution<int>(0, heads - 1)(rng) : 0;
	}
	if (has(kHair))
	{
		const int hair = weighted(choices_.hairNone, choices_.hairWeights), beard = weighted(choices_.beardNone, choices_.beardWeights);
		spec_.hair = hair < 0 ? std::string() : choices_.hairs[(size_t)hair];
		spec_.beard = spec_.female || beard < 0 ? std::string() : choices_.beards[(size_t)beard];
		if (const int c = weighted(0, choices_.hairColourWeights); c >= 0) { spec_.hairColour = choices_.hairColours[(size_t)c]; }
	}
	// Skin Tone: a uniform integer slider position over the ColourRange, stored as the 0..1 position along the palette (charedit.md (2))
	if (has(kBody))
	{
		const int top = skinMax();
		spec_.skin = top > 0 ? (float)std::uniform_int_distribution<int>(0, top)(rng) / (float)top : 0.0f;
	}
}

std::string Creator::specString() const
{
	char buf[64];
	snprintf(buf, sizeof buf, ";head=%d;skin=%.3f", spec_.head, spec_.skin);
	std::string out = "race=" + spec_.race + (spec_.female ? ";female" : "") + buf;
	if (!spec_.hair.empty()) { out += ";hair=" + spec_.hair; }
	if (!spec_.beard.empty()) { out += ";beard=" + spec_.beard; }
	out += ";haircol=" + spec_.hairColour;
	out += ";stance=" + spec_.stance;
	snprintf(buf, sizeof buf, ";posture=%g;shoulders=%g;neck=%g", spec_.posture, spec_.shoulderSet, spec_.neckPosition);
	out += buf;
	for (const auto& [n, v] : spec_.shape) { snprintf(buf, sizeof buf, "%g", v); out += ";shape=" + n + ":" + buf; }
	return out;
}
