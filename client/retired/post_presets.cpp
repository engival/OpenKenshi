// Retired 2026-11-10 (kenshi-k16.2.82): the post presets and the display-curve options, all built on the old sRGB-era look (AgX / Khronos Neutral + the
// fitted contrast). The original's display (tone clip, exposure key 0.55, no output encode) is the client's one base now (INTENT 2026-11-10, the user's
// words on the item); a future preset is a set of modifications layered on top of it, not a row that switches the display transform. The retired shader
// curves (AgX, Neutral, srgb_encode) are in retired/tone_curves.glsl. A cfg file still setting post.preset / post.tone / post.contrast gets one warning
// per key at load (kclient main.cpp Post::retireOptions) and the key is ignored.
// Kept for reference only; not built.

// post.preset rows, in table column order; "custom" (keys edited after a pick) has no row.
const char* const kPresets[] = { "original", "subtle", "enhanced", "cinematic", "stylized" };
constexpr int kPresetCount = std::size(kPresets);

const cfg::Text kPreset{ "post.preset", "subtle", { "original", "subtle", "enhanced", "cinematic", "stylized", "custom" },
	"Graphics preset (our post effects, not the original's): original = all off, subtle = the default. Picking one sets every post.* option; "
	"changing one of them afterwards makes it custom. In the file, set custom when editing post.* options by hand: a named preset rewrites its options at start" };
const cfg::Text kTone{ "post.tone", "agx", { "agx", "neutral", "clip" },
	"Display transform on the linear pipeline: agx = filmic, neutral = Khronos PBR Neutral, clip = the original's hard clip without a curve (the --vanilla preset)" };
const cfg::Float kContrast{ "post.contrast", 0, 0, 1,
	"Contrast look: a per-channel S-curve around middle grey, the punch the old display-referred pipeline had; 1 = the fitted vanilla-esque curve, 0 = Neutral's flat curve" };

// Every post.* option and its value per preset (canonical text: on/off, %g numbers).
struct Row
{
	const cfg::Option* opt;
	const char* value[kPresetCount];
};
const Row kTable[] = {
	{ &kDither, { "off", "on", "on", "on", "on" } },
	{ &kShoulder, { "0", "0", "0.6", "0.8", "0.5" } },	// subtle: the contrast look owns the curve; a second roll-off would undo its highlight fit
	{ &kTone, { "neutral", "neutral", "agx", "agx", "agx" } },	// AgX is base-curve only (flat, desaturated); subtle stays on Neutral until a look is tuned
	{ &kContrast, { "0", "1", "0.55", "0.7", "0" } },
	{ &kBlackPoint, { "0", "0.01", "0.015", "0.04", "0" } },
	{ &kVibrance, { "0", "0.1", "0.12", "0.15", "0.3" } },
	{ &kWarmth, { "0", "0", "0.1", "0.15", "0" } },
	{ &kAa, { "off", "smaa", "smaa", "smaa", "smaa" } },
	{ &kSharpen, { "0", "0.2", "0.4", "0.3", "0.3" } },
	{ &kAo, { "0", "0.6", "1", "1", "0.8" } },
	{ &kIndirect, { "0", "0", "0.5", "0.6", "0" } },
	{ &kSkyAmbient, { "0", "1", "1", "1", "1" } },
	{ &kBloom, { "0", "0.05", "0.12", "0.18", "0.08" } },
	{ &kClarity, { "0", "0.1", "0.25", "0.2", "0" } },
	{ &kVignette, { "0", "0", "0.15", "0.35", "0" } },
	{ &kDof, { "off", "off", "off", "on", "off" } },
	{ &kOutline, { "0", "0", "0", "0", "0.8" } },
	{ &kPosterize, { "0", "0", "0", "0", "8" } },
};

// A new pick of post.preset writes its row (pinned keys keep their command-line value); a key edited away from the row under an unchanged pick makes the
// preset custom. Runs only when an option changed, and writes only differing keys, so a steady state writes nothing.
void Post::syncPreset()
{
	const uint64_t gen = cfg::generation();
	if (gen == gen_) { return; }
	gen_ = gen;
	const std::string preset = kPreset.get();
	const bool picked = preset != preset_;	// the first call counts as a pick: the file's preset wins at startup
	preset_ = preset;
	const auto col = std::find(std::begin(kPresets), std::end(kPresets), preset) - std::begin(kPresets);
	if (col == kPresetCount) { return; }
	const std::vector<cfg::Info> opts = cfg::list();
	auto info = [&](const std::string& key) { return *std::find_if(opts.begin(), opts.end(), [&](const cfg::Info& o) { return o.key == key; }); };
	const bool pinned = info(kPreset.key()).pinned;	// --set post.preset=X: its row is pinned too, never saved
	bool custom = false;
	for (const Row& r : kTable)
	{
		const cfg::Info i = info(r.opt->key());
		if (i.pinned || i.value == r.value[col]) { continue; }
		if (!picked) { custom = true; }
		else if (pinned) { cfg::set(i.key + "=" + r.value[col]); }
		else { cfg::setUser(i.key, r.value[col]); }
	}
	if (custom) { kPreset.set("custom"); }
}

// main.cpp, the exposure key (the post setup; the whole display transform is the clip now, so the key has one value):
//   hf.tone = Post::tone();
//   hf.key = hf.tone == 3 ? 0.55f : 0.55f * 0.5f;	// clip: the game's EXPOSURE_KEY as is; the curves (Neutral, AgX) take half (their encode lifts the mid tones)
