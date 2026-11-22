#include <algorithm>
#include "kutil/executable.hpp"
#include "render/lightbake.hpp"
#include "app/realfps.hpp"
#include "render/drawstats.hpp"
#include "render/framestats.hpp"
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <thread>
#include <future>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <unistd.h>
#include <string>
#include <vector>

#include "render/interiorclip.hpp"
#include "sokol_app.h"
#include "sokol_gfx.h"
#include "sokol_debugtext.h"
#include "sokol_gl.h"
#include "sokol_glue.h"
#include "sokol_log.h"
#include "world/buildings.hpp"
#include "world/foliage.hpp"
#include "world/grass.hpp"
#include "render/gputimer.hpp"
#include "app/cpustage.hpp"
#ifdef KCLIENT_DEV_TOOLS
#include "app/threadsplit.hpp"	// the logic / render split on the stages line (kenshi-k16.2.205.12.84)
#endif
#include "dev/packetdigest.hpp"	// KCLIENT_PACKET_DIGEST / KCLIENT_PACKET_POISON (kenshi-k16.2.205.12.93)
#include "knobs/ablate.hpp"	// KCLIENT_ABL removal switches (upper-bound measurements, kenshi-k16.2.205.7)
#include "assets/sidorder.hpp"
#include "render/fogvolume.hpp"
#include "render/sunshafts.hpp"
#include "render/dust.hpp"
#include "render/blooddecals.hpp"
#include "render/arrows.hpp"
#include "render/particles.hpp"
#include "world/particleworld.hpp"
#include "frame/beamfield.hpp"
#include "characters/physics.hpp"
#include "bridge/ragdolldrive.hpp"
#include "characters/ragdoll.hpp"
#include "render/hdr.hpp"
#include "render/idbuffer.hpp"
#include "render/post.hpp"
#include "render/camera.hpp"
#include "characters/portraits.hpp"
#include "world/doorpose.hpp"
#include "dev/debughost.hpp"
#include "dev/devhooks.hpp"
#ifdef KCLIENT_DEV_TOOLS
#include "dev/pickmask.hpp"
#include "dev/driftview.hpp"
#include "dev/routepreview.hpp"
#endif
#include "knobs/lightterms.hpp"
#include "net/aitask.hpp"
#include "app/githash.hpp"
#include "frame/loadscreen.hpp"
#include "platform/localserver.hpp"
#include "assets/dds.hpp"
#include "assets/ogremesh.hpp"
#include "world/clientnav.hpp"
#include "kworld/towns.hpp"
#include "render/navoverlay.hpp"
#include "app/config.hpp"
#include "platform/windowgeom.hpp"
#include "platform/process.hpp"	// plat::unsetEnv
#include "platform/windowgate.hpp"
#include "render/readback.hpp"
#include "render/renderqueue.hpp"
#include "input/inputqueue.hpp"
#include "app/sappcmd.hpp"
#include "app/logicthread.hpp"
#include "frame/lastframe.hpp"
#include "frame/renderframe.hpp"
#include "app/opencont.hpp"
#include "render/weff.hpp"
#include "net/progressfeed.hpp"
#include "ui/ui.hpp"
#include "ui/gfx.hpp"
#include "ui/hud.hpp"
#include "ui/options.hpp"
#include "ui/gear.hpp"
#include "ui/itemwindows.hpp"
#include "ui/stats.hpp"
#include "ui/keys.hpp"
#include "ui/overview.hpp"
#include "ui/squads.hpp"
#include "ui/research.hpp"
#include "ui/crafting.hpp"
#include "ui/faction.hpp"
#include "ui/progressbars.hpp"
#include "ui/namelabels.hpp"
#include "ui/prompt.hpp"
#include "ui/icons.hpp"
#include "ui/creator.hpp"
#include "ui/menus.hpp"
#include "ui/savepanel.hpp"
#include "ui/newgame.hpp"
#include "ui/advanced.hpp"
#include "ui/tutorials.hpp"
#ifdef KCLIENT_DEV_TOOLS
#include "ui/dev/lightspanel.hpp"
#endif
#include "ui/layout.hpp"
#include "ui/prospecting.hpp"
#include "ui/build.hpp"
#include "ui/style.hpp"
#include "app/serverrows.hpp"
#include "ui/trade.hpp"
#include "ui/buildingwindow.hpp"
#include "ui/bubbles.hpp"
#include "ui/dialogue.hpp"
#include "kworld/heightmap.hpp"
#include "kworld/coords.hpp"
#include "assets/materials.hpp"
#include "assets/mods.hpp"
#include "assets/resources.hpp"
#include "render/shadow.hpp"
#include "render/sky.hpp"
#include "kutil/crashlog.hpp"
#include "app/eventlog.hpp"
#include "world/squads.hpp"
#ifdef KCLIENT_DEBUG_VIEW
#include "debug/debugview.hpp"
#include "ui/dev/debugtree.hpp"
#include "ksim/debugview/enums.hpp"
#endif
#include "characters/characters.hpp"
#include "characters/creator.hpp"
#include "characters/creatorscene.hpp"
#include "kworld/gamedata.hpp"
#include "render/terrain.hpp"
#include "render/water.hpp"
#include "render/reflection.hpp"
#include "render/speccube.hpp"
#include "render/weathermap.hpp"
#include "world/weatherinstances.hpp"
#include "audio/audiohook.hpp"
#include "audio/events.hpp"
#include "bridge/simview.hpp"
#include "frame/trails.hpp"
#include "input/collisionpick.hpp"
#include "input/controlhints.hpp"	// the control-hints types and the resolver stub (kenshi-k16.2.294.2)
#include "input/pick.hpp"	// Pick and the target predicates the hover / click / menu read (kenshi-k16.2.294.2)
#include "input/cursors.hpp"
#include "render/drawlist_gpu.hpp"
#include "render/overlaylist.hpp"
#include "world/worldbuildings.hpp"
#include "world/worldmap.hpp"
#include "host/app.hpp"
#include "host/pick.hpp"
#include "host/orders.hpp"

#include "host/hud.hpp"

namespace host
{

const cfg::Int kFloaters{ cfg::Group{ "HUD and labels" }, "ui.floaters", 1, 0, 2, "Damage floaters over hit characters: 0 off, 1 simple, 2 full (the original's option, default simple)" };
// Over-head progress bars (SIMLINK TASK_PROGRESS 39, order 98): whose tasks show a bar. off none, original the bars the original game shows (the player's
// own tasks only), squad every task of your characters (producing, building, repairing, dismantling, processing, first aid too), everyone those plus the
// NPCs in view. The UI's Options row for it is k16.3.15.
const cfg::Text kProgressBars{ cfg::Group{ "HUD and labels" }, "ui.progress_bars", "original", { "off", "original", "squad", "everyone" },
	"Progress bars over working characters: off none, original the original's own bars (lockpicking, cutting, ...), squad every task of your characters, everyone those plus the NPCs in view" };

// Order 98's level for ui.progress_bars: off 0, original 1, squad 2, everyone 3 (SIMLINK order 98 / TASK_PROGRESS "Who gets rows").
uint8_t progress_level()
{
	const std::string& v = kProgressBars.get();
	return (uint8_t)(v == "off" ? 0 : v == "squad" ? 2 : v == "everyone" ? 3 : 1);
}

// A TASK_PROGRESS kind's caption (SIMLINK 39 "kind"). 1-8 are the original's own strings (kinspect findings/ui_captures/progbar.md part A, the static
// callers' table); 9-16 are OURS: the original has no bar for a producer, construction, repair, dismantle, processing or first aid (progbar.md:
// "Construction, repair and dismantle do not use this bar"). nullptr: no bar (0, 15 crafting is never sent, anything newer).
static const char* progress_caption(uint8_t kind)
{
	switch (kind)
	{
	case 1: return "Lockpicking";		// the original's, with the chance suffix below
	case 2: return "Unlocking";			// the original's
	case 3: return "Locking";			// the original's
	case 4: return "Prospecting";		// the original's
	case 5: return "Adding materials";	// the original's
	case 6: return "Training";			// the original's
	case 7: return "Cutting";			// the original's, with the hardness suffix below
	case 8: return "Restraining";		// the original's
	case 9: return "Operating";			// OURS (the producer's own progress bar; the original shows none: has progress bar when used is 0 on the mines)
	case 10: return "Producing";		// OURS
	case 11: return "Building";			// OURS
	case 12: return "Repairing";		// OURS
	case 13: return "Dismantling";		// OURS
	case 14: return "Processing";		// OURS
	case 16: return "First aid";		// OURS
	default: return nullptr;
	}
}

// Where a character's progress bar and floating texts hang: the plumb line under the drawn body's own centre, not under its root (kenshi-h2v.16.7.1,
// user 2026-11-13: "the bar drifts side to side as the camera rotates"). The root is where the sim stands the character; a pose (the lockpicking crouch,
// a lean, a kneel) draws the body off it, up to ~1.5 u at the pelvis, and that offset seen from a rotating camera swings the body across the bar. The
// body's centre is the mean x / z of the head, pelvis and both feet, taken from this frame's pose (the hands swing and are left out); the height stays
// the placed (feet) one. A character not drawn, or without those bones, keeps the placed position. UNSURE: the original's own point is kenshi-rwh.115's
// (not answered yet); this is the drawn body's centre, which stays centred at any yaw by construction.
static void body_anchor(uint64_t id, double pos[3])
{
	const int index = app.sim.charIndex(id);
	if (index < 0) { return; }
	double sx = 0, sz = 0;
	int n = 0;
	for (const char* name : { "Bip01 Head", "Bip01 Pelvis", "Bip01 L Foot", "Bip01 R Foot" })
	{
		double b[3];
		if (!app.chars.boneWorld((size_t)index, name, b)) { return; }
		sx += b[0]; sz += b[2]; n++;
	}
	pos[0] = sx / n;
	pos[2] = sz / n;
}

// TASK_PROGRESS bars (SIMLINK 39, captions the client's) and the event floating texts (kinspect floating_text.md: 15 units over the character, rising), every frame
void progress_frame()
{
	std::vector<ui::WorldProgress> bars;
	std::vector<ui::WorldFloatText> texts;
	const double now = app.sim.tick();
	if (app.sim.active() && now >= 0)
	{
		const simlink::SimLinkDecoder& d = app.sim.decoder();
		const bool barsOn = progress_level() != 0;	// off draws no over-head bars at all (the sim sends no rows then either)
		for (const simlink::SimLinkDecoder::TaskProgress& t : d.tasks)
		{
			const char* caption = progress_caption(t.kind);
			double pos[3];
			float hull[3];
			ui::WorldProgress b;
			if (!barsOn || !caption || !app.sim.placed(t.ch, pos, hull)) { continue; }
				body_anchor(t.ch, pos);
				if (!world_to_screen(pos, b.x, b.y)) { continue; }
			b.id = t.ch;
			b.text = caption;
			char v[32];
			if (t.kind == 1 && t.value > 0) { snprintf(v, sizeof v, " (Chance %.1f%%)", (int)(t.value * 1000) * 0.1); b.text += v; }
			if (t.kind == 7 && t.value > 0) { snprintf(v, sizeof v, ": %g", t.value); b.text += v; }
			b.progress = (ProgressFeed{ t.progress, t.rate, d.tasksTick }).at(now);
			bars.push_back(std::move(b));
		}
		uint64_t n = 0;
		for (const SimView::Popup& p : app.sim.popups())
		{
			// the exe's speed index: 1 for the lock roll, the damage and "Failed!" (4.0/s: age += 4 x dt, life 12 / 4 = 3 s) and 3 for the
			// landed KO's "*Thunk*" (10.0/s, life 1.2 s; s54_static.md 1b, the label keeps its create-time speed until success sets vf+0x30);
			// the label rides its character, `age` units above the + 15 anchor
			const double kSpeed = p.kind == 50 && p.result == 0 ? 10.0 : 4.0;
			ui::WorldFloatText f;
			f.age = (float)((now - p.tick) / SimView::kTickRate * kSpeed);
			if (f.age >= 12) { continue; }
			double feet[3];
			float hull[3];
			const bool placedNow = app.sim.placed(p.ch, feet, hull);
				if (placedNow) { body_anchor(p.ch, feet); }
				const double* base = placedNow ? feet : p.pos;
			if (p.kind == 1)
			{
				// receiveHit's floater: gated by the option, within 4000 of the camera
				const double dx = base[0] - app.pos[0], dz = base[2] - app.pos[2];
				if (kFloaters.get() == 0 || dx * dx + dz * dz > 4000.0 * 4000.0) { continue; }
				// N = (int) the final damage over all four groups (kinspect: receiveHit l.740, after armour, toughness and damageScale, as event 1);
				// Full: cut (+ pierce when > 1) and blunt + the armour stun part
				const float cut = p.damage[2] > 1 ? p.damage[0] + p.damage[2] : p.damage[0], blunt = p.damage[1] + p.damage[3];
				f.font = kFloaters.get() == 1 ? 1 : 0;	// Simple Medium, Full Small; picks Small (the default)
				// "(-M)": what armour stopped, before toughness (M = (int) the sum; each Full group's own (int)), shown only when > 0
				const auto stopped = [](float v) { return (int)v > 0 ? " (-" + std::to_string((int)v) + ")" : std::string(); };
				if (kFloaters.get() == 1) { f.text = std::to_string((int)(p.damage[0] + p.damage[1] + p.damage[2] + p.damage[3])) + stopped(p.absorbed[0] + p.absorbed[1]); }
				else { f.text = app.sim.sidOf(p.part) + ": " + std::to_string((int)cut) + stopped(p.absorbed[0]) + (blunt >= 1 ? " + " + std::to_string((int)blunt) + stopped(p.absorbed[1]) : ""); }	// the part's sid, not yet the game's limb label
				f.colour = app.sim.isPlayer(p.ch) ? ui::Color{ 1.0f, 0.31f, 0.31f, 0.9f } : app.sim.isPlayer(p.other) ? ui::Color{ 0.11f, 0.60f, 0.11f, 0.9f } : ui::Color{ 0.9f, 0.9f, 0.9f, 0.9f };
			}
			else if (p.kind == 20)	// lock_roll (event 20): the original's strings
			{
				f.text = p.result ? "Pick success!" : "Pick failed";
				f.colour = p.result ? ui::Color{ 0.4f, 0.8f, 0.4f, 0.9f } : ui::Color{ 0.7f, 0.4f, 0.4f, 0.7f };
			}
			else if (p.kind == 50)	// stealth_ko (event 50): "*Thunk*" green on a landed KO, "Failed!" red on a failed roll
			{
				// The exe's label (s54_static.md 1b): created as "Failed!" with font index 1 = Kenshi_FloaterFont_Medium, on success recoloured
				// and setText'd to "*Thunk*". Result 1 (the roll failed) and 2 (aborted, the victim was aware) both stay "Failed!": the exe's
				// abort path is unread (STEALTH_KO.md D2) and has no event of its own. Its " (No XP)" suffix is its local XP rule, not on the
				// wire: left out.
				f.text = p.result == 0 ? "*Thunk*" : "Failed!";
				f.font = 1;	// Kenshi_FloaterFont_Medium (the exe's), not the Small default
				f.colour = p.result == 0 ? ui::Color{ 0.4f, 0.8f, 0.4f, 0.9f } : ui::Color{ 0.7f, 0.4f, 0.4f, 0.7f };
			}
			else { continue; }	// no other kind is a popup (SimView::answers pushes 1, 20 and 50)
			const double at[3] = { base[0], base[1] + 15 + f.age, base[2] };
			if (!world_to_screen(at, f.x, f.y)) { continue; }
			f.id = ++n;
			texts.push_back(std::move(f));
		}
	}
	app.progressBars->update(std::move(bars));
	app.progressBars->floatTexts(std::move(texts));
}

double bearing(double dx, double dz)	// compass degrees of a world offset, north = -z
{
	const double d = std::atan2(dx, -dz) * 180.0 / 3.14159265358979;
	return d < 0 ? d + 360.0 : d;
}

// The move marker's walk target (user 2026-11-15, kenshi-k16.2.259: the marker and the route end at the target the sim uses, no client-side snap): the
// last waypoint of the selected characters' route that carried the last move_to click's clientSeq — where the sim walks to, an approach point when the
// click was unreachable (route flag 2) — never a point of our own. False: no such route yet (its answer may still come, or never), the click point stands.
bool order_route_end(double out[3])
{
	if (!app.orderSeq) { return false; }
	for (const uint64_t id : app.sim.selected())
	{
		const simlink::Route* rt = app.sim.routeOf(id);
		if (rt && rt->clientSeq == app.orderSeq && !rt->points.empty())
		{
			for (int k = 0; k < 3; k++) { out[k] = rt->points.back()[k]; }
			return true;
		}
	}
	return false;
}

// kenshi-k16.2.414.2: a sim (navmesh) point drawn on the ground solver's drawn ground (SimView::groundAt), the same lookup as the body's feet; y moves, x and z stay. Nothing found: the point stands.
static void on_drawn_ground(double p[3])
{
	double y;
	if (app.sim.groundAt(p[0], p[2], p[1], y) >= 1) { p[1] = y; }
}

// Sim markers in the overlay (no depth test): a ring under each selected character, its route as a line along the ground, a fading ring at the last
// move_to click. Lines hug the terrain, except between points standing above it (a floor, a deck: MINE 2 u), which keep the sim's y (raised to the
// terrain where it would dip under); lifted a little. A segment with an end behind the camera is dropped.
// A character's tag point on screen: over the head (MINE: lift x the hull height, 0.85 = 17 u for a human; 3 u for one down), within kLabelRange and
// on screen. mvp: build_mvp(mvp, 1, 1000).
static bool head_screen(uint64_t id, const simlink::Character& ch, const float mvp[16], float& sx, float& sy, double& dist, float lift = 0.85f)
{
	double at[3];
	float hull[3];
	if (!app.sim.placed(id, at, hull)) { return false; }
	const bool down = ch.cur.flags & (simlink::kKO | simlink::kDead | simlink::kProne);
	const float p[3] = { (float)(at[0] - app.pos[0]), (float)(at[1] + (down ? 3.0 : hull[1] * lift) - app.pos[1]), (float)(at[2] - app.pos[2]) };
	dist = std::hypot(p[0], p[1], p[2]);
	if (dist > kLabelRange) { return false; }
	float c[4];
	for (int i = 0; i < 4; i++) { c[i] = mvp[i] * p[0] + mvp[4 + i] * p[1] + mvp[8 + i] * p[2] + mvp[12 + i]; }
	if (c[3] < 1.0f) { return false; }
	sx = (c[0] / c[3] * 0.5f + 0.5f) * view::widthf();
	sy = (0.5f - c[1] / c[3] * 0.5f) * view::heightf();
	return sx >= 0 && sx <= view::widthf() && sy >= 0 && sy <= view::heightf();
}

// The UI style chosen at start (ui.style takes effect at ui::init, so a config change needs a restart): for a host-drawn overlay that has the
// game's own art as a second look, as the selection box does (kenshi-rwh.134).
bool original_style()
{
	static const bool original = []
	{
		for (const cfg::Info& i : cfg::list()) { if (i.key == "ui.style") { return i.value == "original"; } }
		return false;
	}();
	return original;
}

// The world's labels over the scene (draw_sim_markers' last part): the name tags over the heads, the item labels, the padlock's pick chance, and with
// the UI the selection box and the debug portrait strip. With the UI they are UI draws: record_ui calls this in logic between gfx::begin and end on
// the packet's labels list (render hook 8's host half, kenshi-k16.2.205.12.39), so the font bakes they cause go with the UI's own; without it,
// draw_sim_markers calls it render side (draw_panel / draw_text). mvp: the packet's camera at 1 .. 1000 (build_mvp's).
void world_labels(const float mvp[16])
{
	auto tag = [](float x, float y, float w, float h, const std::string& text, ui::TagState st, ui::Color ink = {})
	{
		if (app.uiOn) { ui::drawTag({ x, y, w, h }, text, st, nullptr, ink); return; }
		const float s = text_scale(), g = st == ui::TagState::kNormal ? 0.85f : 1.0f, b = st == ui::TagState::kNormal ? 0.3f : 0.6f;
		draw_panel(x, y, w, h);
		draw_text(x + (w - 8.0f * s * text.size()) * 0.5f, y + (h - 8.0f * s) * 0.5f, st == ui::TagState::kFriendly ? 0.6f : 1.0f, g, b, text.c_str());
	};
	struct Tag { float x, y, w; double dist; std::string name; bool ours; uint64_t id; };
	std::vector<Tag> tags;	// the name tags drawn
	if (kCharacterLabels.get() && labels_shown())
	{
		// over the head (head_screen), overlapping boxes pushed up, nearest placed first
		const float h = label_height();
		for (const auto& [id, ch] : app.sim.decoder().chars)
		{
			std::string name = app.sim.decoder().str(ch.look.name);
			float sx, sy;
			double dist;
			double feet[3];
			float hull[3];
			if (name.empty() || app.sim.speech().count(id) || !head_screen(id, ch, mvp, sx, sy, dist) || (app.sim.placed(id, feet, hull) && char_cut_hidden(id, feet))) { continue; }
			const float w = label_text_width(name) + kLabelPad * view::heightf() / 720.0f;
			tags.push_back({ sx - w * 0.5f, sy - h, w, dist, std::move(name), (ch.cur.flags & simlink::kPlayer) != 0, id });
		}
		std::sort(tags.begin(), tags.end(), [](const Tag& a, const Tag& b) { return a.dist < b.dist; });
		for (size_t i = 0; i < tags.size(); i++)
		{
			for (bool moved = true; moved;)
			{
				moved = false;
				for (size_t k = 0; k < i; k++)
				{
					const Tag& o = tags[k];
					if (tags[i].x < o.x + o.w && o.x < tags[i].x + tags[i].w && tags[i].y < o.y + h && o.y < tags[i].y + h) { tags[i].y = o.y - h - 1; moved = true; }
				}
			}
		}
		for (const Tag& t : tags) { tag(t.x, t.y, t.w, h, t.name, t.ours ? ui::TagState::kFriendly : ui::TagState::kNormal); }
	}
	// item text: ItemStandard (picker.md); the grade inks (ItemGrade0..5 by level, ItemArtifact) wait for the item's spec; someone else's: a red frame
	// (kOwned; the original's ALT view, user 2026-11-05), hovered the stealing pair (red text)
	const ui::Color itemInk = app.uiOn ? ui::style().colour("ItemStandard", {}) : ui::Color{};
	for (const ItemLabel& l : item_labels(labels_shown(), mvp))
	{
		const bool theirs = app.sim.foreign(l.id);
		const ui::TagState state = l.id != app.hoverThing ? (theirs ? ui::TagState::kOwned : ui::TagState::kNormal) : theirs ? ui::TagState::kStealing : ui::TagState::kHover;
		tag(l.x, l.y, l.w, l.h, l.text, state, itemInk);
	}
	if (hoverCage && hoverCage == cageHover.cage && cageHover.chance >= 0 && !app.look && !ui::popupsOpen())	// the padlock's pick chance, above the cursor (kenshi-0qr shot 9)
	{
		char buf[16];
		snprintf(buf, sizeof buf, "%d%%", (int)(std::floor(cageHover.chance * 1000.0f) * 0.1f));
		const float h = label_height(), w = label_text_width(buf) + kLabelPad * view::heightf() / 720.0f;
		tag((float)app.mouseX - w * 0.5f, (float)app.mouseY - h - 26.0f * view::heightf() / 720.0f, w, h, buf, ui::TagState::kNormal, ui::Color{ 0.35f, 1.0f, 0.35f, 1.0f });
	}
#ifdef KCLIENT_DEV_TOOLS
	dev::pickmask::draw();
#endif
	if (app.uiOn && app.boxDown && app.boxDrag)	// the selection box: ours (a 1 px light outline over a faint fill), or under ui.style=original the
	{											// original's Kenshi_SelectionPanel, a thin near-black outline with no fill (kenshi-rwh.134)
		const float x0 = (float)std::min(app.boxX0, app.boxX1), y0 = (float)std::min(app.boxY0, app.boxY1);
		const ui::Rect r{ x0, y0, (float)std::max(app.boxX0, app.boxX1) - x0, (float)std::max(app.boxY0, app.boxY1) - y0 };
		const ui::Skin* sk = original_style() ? ui::res::skin("Kenshi_SelectionPanel") : nullptr;
		if (sk) { sk->draw(r, 1.0f, ui::State::kNormal, false); }	// scale 1: the original's box is fixed pixels at any resolution
		else
		{
			ui::gfx::fill(r, { 1, 1, 1, 0.08f });
			ui::gfx::frame(r, 1.0f, { 0.92f, 0.92f, 0.92f, 0.9f });
		}
	}
	if (app.uiOn && app.showPortraits) { ui::gfx::textured(app.portraitsTex, { 0, 120, 1024, 256 }, 0, 0.0625f, 0.25f, 0, { 1, 1, 1, 1 }); }
}

// The sim markers (render sitting 11, kenshi-k16.2.205.12.57): selection rings, routes, ground items and containers without a mesh, the move pin and,
// without the UI, the labels. Logic, through ovl:: into the packet's markers list (record_overlay binds it); with the UI the labels are record_ui's
// list, which world_overlay replays right after this one (renderframe.cpp), as the old draw did at its end.
void draw_sim_markers(const FramePacket& pk)
{
	const Camera cam = overlay_camera(pk);
	const double* eye = cam.pos;	// app.pos when the packet was filled
	float mvp[16];
	cam.mvp(mvp);
	auto screen = [&](double x, double y, double z, float& sx, float& sy)
	{
		const float p[3] = { (float)(x - eye[0]), (float)(std::max(y, app.hm->height(x, z)) + 0.5 - eye[1]), (float)(z - eye[2]) };
		float c[4];
		for (int i = 0; i < 4; i++) { c[i] = mvp[i] * p[0] + mvp[4 + i] * p[1] + mvp[8 + i] * p[2] + mvp[12 + i]; }
		if (c[3] < 1.0f) { return false; }
		sx = (c[0] / c[3] * 0.5f + 0.5f) * view::widthf();
		sy = (0.5f - c[1] / c[3] * 0.5f) * view::heightf();
		return true;
	};
	auto segment = [&](const double a[3], const double b[3])
	{
		// split long segments so the line follows the terrain
		auto raised = [&](const double q[3]) { return q[1] - app.hm->height(q[0], q[2]) > 2; };
		const bool floor = raised(a) || raised(b);	// else y = -inf: the terrain everywhere
		const int n = std::clamp((int)(std::hypot(b[0] - a[0], b[2] - a[2]) / 8.0), 1, 64);
		for (int i = 0; i < n; i++)
		{
			float ax, ay, bx, by;
			const double u = (double)i / n, v = (double)(i + 1) / n;
			const double ya = floor ? a[1] + (b[1] - a[1]) * u : -1e9, yb = floor ? a[1] + (b[1] - a[1]) * v : -1e9;
			if (screen(a[0] + (b[0] - a[0]) * u, ya, a[2] + (b[2] - a[2]) * u, ax, ay) && screen(a[0] + (b[0] - a[0]) * v, yb, a[2] + (b[2] - a[2]) * v, bx, by)) { ovl::v2f(ax, ay); ovl::v2f(bx, by); }
		}
	};
	auto ring = [&](const double c[3], double r)
	{
		constexpr int kSides = 24;
		for (int i = 0; i < kSides; i++)
		{
			const double a = 6.2831853 * i / kSides, b = 6.2831853 * (i + 1) / kSides;
			const double p[3] = { c[0] + r * std::cos(a), c[1], c[2] + r * std::sin(a) }, q[3] = { c[0] + r * std::cos(b), c[1], c[2] + r * std::sin(b) };
			segment(p, q);
		}
	};
	ovl::disable_texture();
	ovl::begin_lines();
	bool approach = false;
	for (const SimView::Marker& m : app.sim.markers())
	{
		if (kSelectionRing.get()) { ovl::c4f(0.3f, 1.0f, 0.4f, 0.9f); ring(m.pos, 4.0); }
		if (!kRouteLine.get()) { continue; }
		ovl::c4f(1.0f, 1.0f, 1.0f, 0.6f);
		double at[3] = { m.pos[0], m.pos[1], m.pos[2] };
		for (const auto& p : m.route->points)
		{
			double next[3] = { p[0], p[1], p[2] };
			on_drawn_ground(next);	// the route's waypoints are navmesh points: drawn where the body would stand there
			segment(at, next);
			std::copy(next, next + 3, at);
		}
		if (!m.route->points.empty() && (m.route->flags & 2))
		{
			// an approach point (the click was unreachable; the server walks as close as it can): marked where the walk ends, in place of the click
			ovl::c4f(1.0f, 0.7f, 0.2f, 0.9f);
			ring(at, 2.5);
			approach = true;
		}
	}
#ifdef KCLIENT_DEV_TOOLS
	if (app.opt.debug && dev::routepreview::on() && !dev::routepreview::points().empty())	// debug.route_preview: the sim planner's route to the cursor, magenta (the route marker is white)
	{
		ovl::c4f(1.0f, 0.2f, 0.9f, 0.95f);
		const auto& rp = dev::routepreview::points();
		for (size_t i = 1; i < rp.size(); i++)
		{
			double a[3] = { rp[i - 1][0], rp[i - 1][1], rp[i - 1][2] }, b[3] = { rp[i][0], rp[i][1], rp[i][2] };
			on_drawn_ground(a);
			on_drawn_ground(b);	// the planner's points are navmesh points (k16.2.412.4)
			segment(a, b);
		}
		if (dev::routepreview::approach()) { double e[3] = { rp.back()[0], rp.back()[1], rp.back()[2] }; on_drawn_ground(e); ring(e, 2.5); }
	}
	for (const dev::drift::Ghost& g : dev::drift::ghosts())	// the side-by-side drift view (dev/driftview.cpp): the original's character, a line to ours, by distance
	{
		if (g.colour == 0) { ovl::c4f(0.25f, 1.0f, 0.35f, 0.95f); } else if (g.colour == 1) { ovl::c4f(1.0f, 0.9f, 0.2f, 0.95f); } else { ovl::c4f(1.0f, 0.25f, 0.2f, 0.95f); }
		segment(g.ghost, g.ours);
		ring(g.ghost, 2.0);
		if (g.origState != g.ourState)	// the original's state differs from ours: a second ring (down), a cross (ko), both (dead), a pole (carried)
		{
			const double* c = g.ghost;
			const double a[3] = { c[0] - 3.0, c[1], c[2] - 3.0 }, b[3] = { c[0] + 3.0, c[1], c[2] + 3.0 }, e[3] = { c[0] - 3.0, c[1], c[2] + 3.0 }, f[3] = { c[0] + 3.0, c[1], c[2] - 3.0 };
			if (g.origState == 1 || g.origState == 3) { ring(c, 3.5); }
			if (g.origState == 2 || g.origState == 3) { segment(a, b); segment(e, f); }
			if (g.origState == 4) { const double top[3] = { c[0], c[1] + 8.0, c[2] }; segment(c, top); }
		}
	}
#endif
	// ground items: their record's mesh (WorldBuildings::setProps, BuildingSet::items); a small ring for one without (MINE), and the name label (the original's DropItemLabels) while ALT is held
	// (UNSURE: whether the original shows them only then) or the item is hovered, the hovered one highlighted
	// containers: their BUILDING mesh (WorldBuildings::setProps); a square of kContainerReach for one without, red once a theft event named it
	for (const auto& [id, th] : app.sim.things())
	{
		const bool hover = id == app.hoverThing;
		if (th.kind == simlink::Interactable::kItem && !app.buildings.hasMesh(id))
		{
			ovl::c4f(1.0f, hover ? 1.0f : 0.85f, hover ? 0.6f : 0.3f, 0.9f);
			const double c[3] = { th.pos[0], app.sim.thingY(id, th), th.pos[2] };
			ring(c, 1.5);
		}
		else if (th.kind == simlink::Interactable::kContainer && !app.buildings.hasMesh(id))
		{
			const double r = SimView::kContainerReach * 0.7;
			if (app.sim.foreign(id)) { ovl::c4f(1.0f, 0.3f, 0.2f, 0.9f); }
			else { ovl::c4f(0.6f, hover ? 1.0f : 0.8f, 1.0f, 0.9f); }
			const double y = app.sim.thingY(id, th);
			const double corner[4][3] = { { th.pos[0] - r, y, th.pos[2] - r }, { th.pos[0] + r, y, th.pos[2] - r }, { th.pos[0] + r, y, th.pos[2] + r }, { th.pos[0] - r, y, th.pos[2] + r } };
			for (int k = 0; k < 4; k++) { segment(corner[k], corner[(k + 1) % 4]); }
		}
	}
	ovl::count_lines();
	ovl::end();
	// the move marker (kinspect selection_marker.md): the game's position.mesh x8, upright on the click point, debug_marker's rgb(0.612, 0.694, 0.529)
	// flat (the live pixel is the constant, kenshi-rwh.5), popping in at full alpha and fading linearly over kPinFade (the live rate while playing:
	// ~1.33/s; real time here, so a paused game fades at play speed). Drawn over everything, as its x-ray pass shows the hidden part. MINE: a refusal red.
	constexpr double kPinFade = 0.75;
	if (const ogre::Mesh* pin = move_marker(); pin && app.orderAge < kPinFade)
	{
		double pinAt[3];	// the click point until the sim's route arrives, then the route's own end (k16.2.259: the marker shows the target the sim walks to)
		std::copy(app.orderAt, app.orderAt + 3, pinAt);
		if (order_route_end(pinAt)) { on_drawn_ground(pinAt); }
		auto project = [&](const double w[3], float& sx, float& sy)
		{
			const float q[3] = { (float)(w[0] - eye[0]), (float)(w[1] - eye[1]), (float)(w[2] - eye[2]) };
			float c[4];
			for (int i = 0; i < 4; i++) { c[i] = mvp[i] * q[0] + mvp[4 + i] * q[1] + mvp[8 + i] * q[2] + mvp[12 + i]; }
			if (c[3] < 1.0f) { return false; }
			sx = (0.5f + c[0] / c[3] * 0.5f) * view::widthf();
			sy = (0.5f - c[1] / c[3] * 0.5f) * view::heightf();
			return true;
		};
		const float rgb[3] = { app.orderRejected ? 0.9f : 0.612f, app.orderRejected ? 0.25f : 0.694f, app.orderRejected ? 0.15f : 0.529f };
		ovl::begin_triangles();
		for (const ogre::Submesh& sub : pin->submeshes)
		{
			const std::vector<float>& pos = pin->geometry(sub).pos;
			const std::vector<uint32_t> tris = sub.triangleList();
			for (size_t i = 0; i + 2 < tris.size(); i += 3)
			{
				double w[3][3];
				float sx[3], sy[3];
				bool on = true;
				for (int v = 0; v < 3; v++)
				{
					for (int k = 0; k < 3; k++) { w[v][k] = pinAt[k] + 8.0 * pos[tris[i + v] * 3 + k]; }
					on = on && project(w[v], sx[v], sy[v]);
				}
				const double e1[3] = { w[1][0] - w[0][0], w[1][1] - w[0][1], w[1][2] - w[0][2] }, e2[3] = { w[2][0] - w[0][0], w[2][1] - w[0][1], w[2][2] - w[0][2] };
				const double n[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
				const double v[3] = { w[0][0] - eye[0], w[0][1] - eye[1], w[0][2] - eye[2] };
				const double facing = (n[0] * v[0] + n[1] * v[1] + n[2] * v[2]) / (std::hypot(n[0], n[1], n[2]) * std::hypot(v[0], v[1], v[2]) + 1e-12);
				if (!on || facing >= 0) { continue; }	// front faces only: a back face behind one would double the alpha
				ovl::c4f(rgb[0], rgb[1], rgb[2], (float)(1.0 - app.orderAge / kPinFade));
				for (int k = 0; k < 3; k++) { ovl::v2f(sx[k], sy[k]); }
			}
		}
		ovl::count_triangles();
		ovl::end();
	}
	// labels: with the UI the packet's labels list (record_ui built it; world_overlay replays it after this list); without it the debug panel and font, here
	if (!app.uiOn) { world_labels(mvp); }
}

#if KCLIENT_PROFILE
// The profiling overlay's Sim block (SIMLINK.md "PROFILE", kenshi-k16.2.148): the KSIM_PROFILE server's answer to order 120 (live_profile), laid out
// like the GPU line above it. Tick mean with the p50 / p99 / max of the server's window and its share of the tick budget, the per-tick counts, a
// sparkline of the recent tick means (one step per answer, the last 40) and one row per docs/PROFILING.md section, sorted by mean. A refusal
// (not_allowed / unsupported) is one line and stops the queries (SimView::profileStatus).
static void sim_profile_lines(std::vector<HudLine>& out)
{
	const simlink::SimLinkDecoder::Profile* p = app.sim.profile();
	if (!p)
	{
		const std::string& st = app.sim.profileStatus();
		if (st.empty()) { return; }	// no answer yet: the query may still be in flight
		char text[256];
		snprintf(text, sizeof text, "sim profile: %s%s", st.c_str(),
			st == "unsupported" ? " (the server was built without KSIM_PROFILE)" : st == "not_allowed" ? " (an admin session is needed)" : "");
		out.push_back({ "simprofile", text, 1.0f, 0.6f, 0.5f });
		return;
	}
	if (app.simProfileSeen != app.sim.decoder().profileSerial)	// one sparkline step per answer
	{
		app.simProfileSeen = app.sim.decoder().profileSerial;
		app.simTickHistory[app.simTickHistoryNext] = p->tickMeanUs;
		app.simTickHistoryNext = (app.simTickHistoryNext + 1) % app.simTickHistory.size();
		app.simTickHistoryN = std::min(app.simTickHistoryN + 1, app.simTickHistory.size());
	}
	char line[256];
	const float budget = p->tickBudgetUs > 0 ? p->tickBudgetUs : 50000.0f;
	snprintf(line, sizeof line, "sim tick %.2f ms mean, p50 %.2f, p99 %.2f, max %.2f  (%.0f%% of %.0f)  window %u", p->tickMeanUs / 1000.0f,
		p->tickP50Us / 1000.0f, p->tickP99Us / 1000.0f, p->tickMaxUs / 1000.0f, 100.0f * p->tickMeanUs / budget, budget / 1000.0f, p->window);
	out.push_back({ "simprofile", line, 0.7f, 1.0f, 1.0f });
	snprintf(line, sizeof line, "sim counts  characters %.1f  squads %.1f  path queries %.1f/tick (%.1f us)", p->characters, p->squads, p->pathQueries, p->pathUs);
	out.push_back({ "simprofile", line, 0.7f, 1.0f, 1.0f });
	// The tick means, oldest first, scaled to their own maximum so a fast server still shows a shape; a step per answer (about 1 s).
	float peak = 1e-3f;
	for (size_t i = 0; i < app.simTickHistoryN; i++) { peak = std::max(peak, app.simTickHistory[(app.simTickHistoryNext + app.simTickHistory.size() - app.simTickHistoryN + i) % app.simTickHistory.size()]); }
	static const char* const kLevels = " .:-=+*#%@";	// 10 steps, sdtx's ASCII only
	std::string spark = "sim spark  ";
	for (size_t i = 0; i < app.simTickHistoryN; i++)
	{
		const float v = app.simTickHistory[(app.simTickHistoryNext + app.simTickHistory.size() - app.simTickHistoryN + i) % app.simTickHistory.size()];
		spark += kLevels[std::clamp((int)(v / peak * 9.99f), 0, 9)];
	}
	snprintf(line, sizeof line, "  (max %.2f ms, %zu answers)", peak / 1000.0f, app.simTickHistoryN);
	spark += line;
	out.push_back({ "simprofile", spark.c_str(), 0.7f, 1.0f, 0.7f });
	out.push_back({ "simprofile", "sim sections (us/tick, sorted by mean)", 0.7f, 1.0f, 0.7f });
	std::vector<const simlink::SimLinkDecoder::Profile::Row*> rows;
	rows.reserve(p->rows.size());
	for (const auto& r : p->rows) { rows.push_back(&r); }
	std::sort(rows.begin(), rows.end(), [](const auto* a, const auto* b) { return a->meanUs > b->meanUs; });
	for (const auto* r : rows)
	{
		snprintf(line, sizeof line, "  %-12s %8.1f us   max %8.1f", r->name.c_str(), r->meanUs, r->maxUs);
		out.push_back({ "simprofile", line, 0.8f, 0.9f, 0.9f });
	}
}
#endif

// The developer text block's lines (PLAYER_GUIDE 6.10), top to bottom; draw_hud draws them and the F12 sidecar writes each as "hud.<key>: text", so the
// two cannot drift (kenshi-k16.2.335.1). Built every --debug frame even while the block is hidden: the sim profile's sparkline steps per answer.
std::vector<HudLine> hud_lines()
{
	std::vector<HudLine> out;
	const double ground = app.hm->height(app.pos[0], app.pos[2]);
	const kworld::ZoneCell zone = kworld::zoneCellOf(app.pos[0], app.pos[2]);
	double heading = std::fmod(app.yaw * 180.0 / 3.14159265358979, 360.0);
	if (heading < 0) { heading += 360.0; }
	char line[256];
	// The real fps (kenshi-k16.2.437.3, app/realfps.hpp): frames that drew a new packet a second, not sokol's callback-interval EMA (a repeat of the
	// kept frame counts there); the repeats a second follow when there are any: "58 fps (+2 rep)".
	const realfps::Rate rf = realfps::last();
	const double shown = rf.unique > 0 ? rf.unique : app.fps;	// the first second: the EMA
	const double rep = rf.swaps - rf.unique;
	char fpsText[48];
	if (rf.unique > 0 && rep >= 0.5) { snprintf(fpsText, sizeof fpsText, "%.0f fps (+%.0f rep)", shown, rep); } else { snprintf(fpsText, sizeof fpsText, "%.0f fps", shown); }
	snprintf(line, sizeof line, "x %.0f  z %.0f  ground %.0f  alt %.0f  zone %d.%d  %s", app.pos[0], app.pos[2], ground, app.pos[1] - ground, zone.x, zone.z, fpsText);
	out.push_back({ "pos", line, 1.0f, 1.0f, 1.0f });
	// the exact camera, so another session can frame the same shot (kinspect knavshot: eye, pivot, dist, yaw, pitch; fovy 45) and the tracked character
	int at = snprintf(line, sizeof line, "heading %s %.1f  pitch %.1f  eye %.1f %.1f %.1f", compass(heading), heading, app.pitch * 180 / 3.14159265358979, app.pos[0], app.pos[1], app.pos[2]);
	if (app.rts) { at += snprintf(line + at, sizeof line - at, "  pivot %.1f %.1f %.1f dist %.1f on %s", app.focus[0], app.focus[1], app.focus[2], app.rtsDist, app.focusOn); }
	double feet[3];
	float hull[3];
	if (const uint64_t id = camera_subject() ? camera_subject() : app.sim.selected().empty() ? 0 : app.sim.selected().front(); id && app.sim.placed(id, feet, hull))
	{
		snprintf(line + at, sizeof line - at, "  char %.1f %.1f %.1f", feet[0], feet[1], feet[2]);
	}
	out.push_back({ "camera", line, 1.0f, 1.0f, 1.0f });
	const int n = app.towns.nearest(app.pos[0], app.pos[2]);
	if (n >= 0)
	{
		const Town& t = app.towns.towns[n];
		const double dx = t.x - app.pos[0], dz = t.z - app.pos[2], b = bearing(dx, dz);
		snprintf(line, sizeof line, "nearest: %s  approx %.1f km (10 u/m) %s %.0f deg", t.name.c_str(), std::hypot(dx, dz) / 10000.0, compass(b), b);
	}
	else { snprintf(line, sizeof line, "nearest: (towns %s)", app.townsReady ? "unavailable" : "loading"); }
	out.push_back({ "nearest", line, 0.8f, 1.0f, 0.8f });
	const WorldBuildings::Stats bs = app.buildings.stats();
	snprintf(line, sizeof line, "buildings %zu drawn, features %zu, %zu draws, %zu meshes, %zu loading  %.1f ms", bs.instancesDrawn - bs.featuresDrawn, bs.featuresDrawn, bs.groups, bs.meshesReady, bs.pending, app.frameMs);
	out.push_back({ "buildings", line, 0.9f, 0.9f, 0.7f });
	const GameClock& gc = app.sky.clock;
	const int minutes = (int)(gc.hours * 60.0);
	const double* sd = app.sky.sunDir();
	// cpu at the start of this line, not the end of the long foliage line: the PAUSED banner (top centre) covered that end, and the end of this one (kenshi-k16.2.205.1)
	char cpuText[64];
	if (logicthread::running()) { snprintf(cpuText, sizeof cpuText, "cpu %.1f ms (logic %.1f render %.1f)  ", framestats::stats().cpu.ms, app.logicMs, framestats::stats().renderMs); }	// two threads (RENDER_THREAD.md 7.1 rule 11)
	else if (app.foliageOn) { snprintf(cpuText, sizeof cpuText, "cpu %.1f ms (cull %.1f)  ", framestats::stats().cpu.ms, app.foliage.stats().cullMs); }
	else { snprintf(cpuText, sizeof cpuText, "cpu %.1f ms  ", framestats::stats().cpu.ms); }
	snprintf(line, sizeof line, "%stime %02d:%02d  day %d  %s  sun altitude %.0f deg", cpuText, minutes / 60, minutes % 60, gc.day + 1,
		gc.paused ? "paused" : gc.speed == 1 ? "1x" : (std::to_string((int)gc.speed) + "x").c_str(), std::asin(sd[1]) * 180.0 / 3.14159265358979);
	out.push_back({ "time", line, 1.0f, 0.95f, 0.6f });
	if (app.foliageOn)
	{
		const Foliage::Stats fs = app.foliage.stats();
		snprintf(line, sizeof line, "foliage x%.3g %zu drawn of %zu, %zu draws, %zu cells (%zu loading)", app.foliage.foliageRange, fs.instancesDrawn, fs.instancesLoaded, fs.draws, fs.cellsLoaded, fs.cellsPending);
		out.push_back({ "foliage", line, 0.7f, 1.0f, 0.7f });
	}
	if (app.opt.debug && !app.squads.dots().empty())	// the squad overlay is a debug view (--debug)
	{
		snprintf(line, sizeof line, app.squads.on ? "squads %zu shown  %s" : "squads %zu (%s to show)", app.squads.dots().size(), ui::keys::label("debug.squad_overlay").c_str());
		out.push_back({ "squads", line, 1.0f, 0.8f, 0.5f });
	}
	if (app.opt.debug) { for (const std::string& l : dev::heightProbeLines()) { out.push_back({ "hprobe", l, 1.0f, 0.95f, 0.7f }); } }	// the vertical height probe's chart
	const std::string helpHint = app.opt.debug ? "  " + ui::keys::label("debug.help") + " help" : "";
	snprintf(line, sizeof line, "weather %s  scene %s  shadows %s (%.1f ms)%s", app.sky.regionalFog ? ("auto: " + app.weatherMix).c_str() : app.sky.weather.name.c_str(), app.sceneName.empty() ? "-" : app.sceneName.c_str(), app.shadow.active() ? "on" : "off", framestats::stats().shadowMs, helpHint.c_str());
	out.push_back({ "weather", line, 0.8f, 0.9f, 1.0f });
#if KCLIENT_PROFILE
	if (framestats::stats().gpuOk)
	{
		char gpuLine[512];	// 19 sections (kenshi-k16.2.101.1) no longer fit the 256-byte overlay line
		int g = snprintf(gpuLine, sizeof gpuLine, "gpu %.1f ms:", framestats::stats().gpuTotal);
		for (int s = 0; s < GpuTimer::kSections && g < (int)sizeof gpuLine; s++)
		{
			g += snprintf(gpuLine + g, sizeof gpuLine - g, " %s %.1f", GpuTimer::name(s), framestats::stats().gpuMs[s]);
			if (g >= (int)sizeof gpuLine) { g = (int)sizeof gpuLine - 1; }
		}
		out.push_back({ "gpu", gpuLine, 0.7f, 0.9f, 1.0f });
	}
#endif
	if (const WorldBuildings::FloorStatus fl = app.buildings.floorStatus(); fl.valid)
	{
		const int m = snprintf(line, sizeof line, "building %s  floor %d/%d  R/F", fl.name.c_str(), fl.shown, fl.top);	// the ground floor is 0, as the original counts (user 2026-11-04)
		if (fl.following && fl.leadFloor >= 0) { snprintf(line + m, sizeof line - m, "  lead on floor %d", fl.leadFloor); }
		out.push_back({ "building", line, 1.0f, 0.85f, 0.6f });
	}
	if (std::string collision; dev::collisionLegend(collision)) { out.push_back({ "collision", collision, 0.75f, 1.0f, 0.75f }); }	// debug.collision's counts (dev/colloverlay.cpp)
	if (app.sim.active()) { out.push_back({ "sim", app.sim.status(), 0.6f, 1.0f, 0.9f }); }
#if KCLIENT_PROFILE
	sim_profile_lines(out);
#endif
	return out;
}
}
