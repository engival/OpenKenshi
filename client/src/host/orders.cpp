#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <map>
#include <string>
#include <vector>

#include "sokol_app.h"

#include "app/config.hpp"
#include "platform/windowgate.hpp"
#include "audio/events.hpp"
#include "host/app.hpp"
#include "host/orders.hpp"
#include "host/pick.hpp"
#include "input/pick.hpp"
#include "ui/ui.hpp"

namespace host
{

// Two right clicks on the same own character within this time: the second is the direct drive (kenshi-k16.2.238.1), as the cast row's double click.
constexpr double kDoubleClick = 0.4;

const cfg::Bool kForeignWalkTo{ cfg::Group{ "Orders" }, "orders.foreign_walk_to", true, "A right click without ALT on someone else's container (shop counter, cabinet, locked safe) shows the ALT hint and walks there, as the original (user 2026-11-14). Off: the hint only. ALT steals / picks the lock either way" };

#ifdef KCLIENT_DEV_TOOLS
uint64_t g_forcePickThing = 0;	// script `mouse force ID`: the next clicks pick thing ID wherever the pointer is (a test of the order dispatch past the pick's geometry)
#else
constexpr uint64_t g_forcePickThing = 0;
#endif

// The per-click record (kenshi-k16.2.259.3, user 2026-11-17: "the EXACT camera pose, x/y click location, and click projection results"): each click at its own
// frame, with what the pick saw, so a bad click is diagnosable from the F12 shot a second later. The last kClickHistory, newest first, go in the F12 sidecar
// ("clicks:" block) and, one per click, in the run log ("click record:"); `click replay KEY=VALUE...` (the record's first line) repeats the pick.
struct ClickRec
{
	uint64_t n = 0, frame = 0;
	bool right = false, shift = false;
	double mx = 0, my = 0, aspect = 0, near = 0;
	std::string mode;
	int w = 0, h = 0, bar = 0;
	double eye[3] = {}, yawDeg = 0, pitchDeg = 0, dist = 0, focus[3] = {}, from[3] = {}, dir[3] = {}, terrain[3] = {};
	bool hasTerrain = false;
	uint64_t followId = 0;
	Pick pick;
	std::vector<PickSkip> skipped;
	std::string building, ghosts;
	int floor = -1;
	float xray[4] = {};
	uint32_t seq = 0;	// a ground click's move_to clientSeq (0: none sent)
};
constexpr size_t kClickHistory = 6;
static std::deque<ClickRec> g_clicks;
static uint64_t g_clickCount = 0;
static const char* const kPickKinds[] = { "none", "ground", "character", "item", "door", "container", "producer", "bed" };

static std::string click_head(const ClickRec& c)
{
	char b[1400];
	snprintf(b, sizeof b,
		"n=%llu frame=%llu btn=%s shift=%d at=%.1f,%.1f size=%dx%d bar=%d mode=%s eye=%.3f,%.3f,%.3f yaw=%.4f pitch=%.4f dist=%.2f focus=%.3f,%.3f,%.3f follow=%llu fovy=45 aspect=%.5f near=%.2f "
		"from=%.3f,%.3f,%.3f dir=%.6f,%.6f,%.6f pick=%s id=%llu hit=%.3f,%.3f,%.3f building=%d terrain=%.3f,%.3f,%.3f xray=%.2f,%.2f,%.2f,%.2f seq=%u",
		(unsigned long long)c.n, (unsigned long long)c.frame, c.right ? "right" : "left", c.shift ? 1 : 0, c.mx, c.my, c.w, c.h, c.bar, c.mode.c_str(), c.eye[0], c.eye[1], c.eye[2], c.yawDeg, c.pitchDeg,
		c.dist, c.focus[0], c.focus[1], c.focus[2], (unsigned long long)c.followId, c.aspect, c.near, c.from[0], c.from[1], c.from[2], c.dir[0], c.dir[1], c.dir[2], kPickKinds[(int)c.pick.kind],
		(unsigned long long)c.pick.id, c.pick.hit[0], c.pick.hit[1], c.pick.hit[2], c.pick.building ? 1 : 0, c.terrain[0], c.terrain[1], c.terrain[2], c.xray[0], c.xray[1], c.xray[2], c.xray[3], c.seq);
	return b;
}

// The record as text: the replayable first line, then what the pick walked past, the buildings ghosted, and (a move) where the sim sent the order.
static std::string click_text(const ClickRec& c)
{
	char b[512];
	std::string r = "click record: " + click_head(c) + "\n";
	if (!c.building.empty()) { snprintf(b, sizeof b, "  hit building \"%s\" floor %d\n", c.building.c_str(), c.floor); r += b; }
	if (!c.hasTerrain) { r += "  terrain: no ray hit\n"; }
	for (const PickSkip& s : c.skipped)
	{
		snprintf(b, sizeof b, "  skipped \"%s\" at %.1f %.1f %.1f: %s\n", s.name.c_str(), s.at[0], s.at[1], s.at[2], s.why.c_str());
		r += b;
	}
	if (c.skipped.empty()) { r += "  skipped: none\n"; }
	r += "  ghosts: " + c.ghosts + "\n";
	if (c.seq)
	{
		for (const uint64_t id : app.sim.selected())
		{
			const simlink::Route* rt = app.sim.routeOf(id);
			if (rt && rt->clientSeq == c.seq && !rt->points.empty())
			{
				snprintf(b, sizeof b, "  route of %llu for seq %u ends %.1f %.1f %.1f (%zu waypoints, flags %d)\n", (unsigned long long)id, c.seq, rt->points.back()[0], rt->points.back()[1], rt->points.back()[2], rt->points.size(), (int)rt->flags);
				r += b;
			}
		}
	}
	return r;
}

static ClickRec* click_begin(double mx, double my, bool right, bool shift, const Pick& p, std::vector<PickSkip>&& skipped)
{
	constexpr double kDeg = 180 / 3.14159265358979;
	ClickRec c;
	c.n = ++g_clickCount;
	c.frame = (uint64_t)app.frames;
	c.right = right;
	c.shift = shift;
	c.mx = mx;
	c.my = my;
	c.mode = camera_mode_name();
	c.w = view::width();
	c.h = view::height();
	c.bar = app.post.letterboxBar();
	c.aspect = windowgate::aspect();
	c.near = camera_near();
	for (int k = 0; k < 3; k++) { c.eye[k] = app.pos[k]; c.focus[k] = app.focus[k]; }
	c.yawDeg = app.yaw * kDeg;
	c.pitchDeg = app.pitch * kDeg;
	c.dist = app.follow ? app.followDist : app.rtsDist;
	c.followId = app.follow ? app.followId : 0;
	mouse_ray(mx, my, c.dir);
	for (int k = 0; k < 3; k++) { c.from[k] = app.pos[k] + c.dir[k] * c.near; }
	c.hasTerrain = ray_ground(c.from, c.dir, c.terrain);
	c.pick = p;
	c.skipped = std::move(skipped);
	for (int k = 0; k < 4; k++) { c.xray[k] = app.buildings.xray[k]; }
	c.ghosts = app.buildings.ghostState();
	g_clicks.push_front(std::move(c));
	while (g_clicks.size() > kClickHistory) { g_clicks.pop_back(); }
	return &g_clicks.front();
}

// The F12 sidecar's "clicks:" block, newest first.
std::string clicks_report()
{
	std::string r = "clicks: " + std::to_string(g_clicks.size()) + " recorded (newest first; each first line replays with the script op `click replay <the key=value words>`)\n";
	for (const ClickRec& c : g_clicks) { r += click_text(c); }
	return r;
}

// Script `click replay KEY=VALUE...`: the pick of a recorded click, from its own camera, pointer and viewport; prints the new pick next to the recorded one.
bool click_replay(const std::vector<std::string>& words)
{
	std::map<std::string, std::string> kv;
	for (const std::string& w : words) { if (const size_t q = w.find('='); q != std::string::npos) { kv[w.substr(0, q)] = w.substr(q + 1); } }
	const auto nums = [&](const char* key, double* out, int n)
	{
		const auto it = kv.find(key);
		if (it == kv.end()) { return false; }
		const char* s = it->second.c_str();
		for (int i = 0; i < n; i++)
		{
			char* end;
			out[i] = strtod(s, &end);
			if (end == s) { return false; }
			s = *end == ',' ? end + 1 : end;
		}
		return true;
	};
	double eye[3], at[2], yaw, pitch, hit[3], xr[4];
	if (!nums("eye", eye, 3) || !nums("at", at, 2) || !nums("yaw", &yaw, 1) || !nums("pitch", &pitch, 1)) { return false; }
	constexpr double kRad = 3.14159265358979 / 180;
	const bool wantHit = nums("hit", hit, 3);
	const bool wantXray = nums("xray", xr, 4);
	double sv[3] = { app.pos[0], app.pos[1], app.pos[2] };
	const double svYaw = app.yaw, svPitch = app.pitch;
	float svX[4];
	for (int k = 0; k < 3; k++) { app.pos[k] = eye[k]; }
	app.yaw = yaw * kRad;
	app.pitch = pitch * kRad;
	for (int k = 0; k < 4; k++) { svX[k] = app.buildings.xray[k]; if (wantXray) { app.buildings.xray[k] = (float)xr[k]; } }
	if (kv.count("size") && kv["size"] != std::to_string(view::width()) + "x" + std::to_string(view::height())) { printf("click replay: the record's size %s is not this run's %dx%d (pass --size)\n", kv["size"].c_str(), view::width(), view::height()); }
	std::vector<PickSkip> sk;
	const Pick p = pick_screen(at[0], at[1], &sk);
	for (int k = 0; k < 3; k++) { app.pos[k] = sv[k]; }
	app.yaw = svYaw;
	app.pitch = svPitch;
	for (int k = 0; k < 4; k++) { app.buildings.xray[k] = svX[k]; }
	const std::string rec = kv.count("pick") ? kv["pick"] : "?";
	const bool same = rec == kPickKinds[(int)p.kind] && (!wantHit || std::hypot(std::hypot(hit[0] - p.hit[0], hit[1] - p.hit[1]), hit[2] - p.hit[2]) < 0.5);
	printf("click replay: at %.1f,%.1f from eye %.1f %.1f %.1f yaw %.2f pitch %.2f: pick %s id %llu hit %.3f %.3f %.3f building %d; recorded %s%s: %s\n", at[0], at[1], eye[0], eye[1], eye[2], yaw, pitch,
		kPickKinds[(int)p.kind], (unsigned long long)p.id, p.hit[0], p.hit[1], p.hit[2], p.building ? 1 : 0, rec.c_str(), wantHit ? (" hit " + kv["hit"]).c_str() : "", same ? "MATCH" : "DIFFERS");
	for (const PickSkip& s : sk) { printf("click replay skipped \"%s\" at %.1f %.1f %.1f: %s\n", s.name.c_str(), s.at[0], s.at[1], s.at[2], s.why.c_str()); }
	return true;
}

void sim_click(double mx, double my, bool right, bool shift)
{
	std::vector<PickSkip> clickSkipped;
	Pick p = pick_screen(mx, my, &clickSkipped);
	if (g_forcePickThing)
	{
		if (const simlink::Interactable* ft = app.sim.thing(g_forcePickThing)) { p = Pick{}; p.kind = ft->kind == simlink::Interactable::kContainer ? Pick::Container : Pick::Item; p.id = g_forcePickThing; }
	}
	ClickRec* const rec = click_begin(mx, my, right, shift, p, std::move(clickSkipped));	// kenshi-k16.2.259.3: the click's own record; printed when this click is done
	struct ClickPrint { ClickRec* r; ~ClickPrint() { fputs(click_text(*r).c_str(), stdout); } } clickPrint{ rec };
	const uint64_t id = p.kind == Pick::Character ? p.id : 0;
	if (!right && p.kind == Pick::Container && label_at(mx, my) == p.id)
	{
		if (container_locked(app.sim, p.id))
		{
			const bool sent = app.sim.interact(p.id, 3, shift);
			printf("sim click (%.0f, %.0f): label: lockpick %llu: %s\n", mx, my, (unsigned long long)p.id, sent ? "sent" : "not sent");
			order_sent(sent);
			return;
		}
		const bool sent = open_container(p.id, shift);
		printf("sim click (%.0f, %.0f): label: open container %llu: %s\n", mx, my, (unsigned long long)p.id, sent ? "sent" : "not sent");
		order_sent(sent);
		return;
	}
	if (!right && p.kind == Pick::Item && label_at(mx, my) == p.id)
	{
		const bool sent = app.sim.interact(p.id, 7, shift);
		printf("sim click (%.0f, %.0f): label: pick up %llu%s: %s\n", mx, my, (unsigned long long)p.id, shift ? " (queued)" : "", sent ? "sent" : "not sent");
		order_sent(sent);
		return;
	}
	if (!right && p.kind == Pick::Producer)	// LMB: the generic machine window, from anywhere, the selection kept (user 2026-11-02, as the original)
	{
		app.machine = p.id;
		app.panelProducer = p.id;	// the panel follows the last clicked thing (k16.19.8.2): the producer
		printf("sim click (%.0f, %.0f): show machine %llu\n", mx, my, (unsigned long long)p.id);
		return;
	}
	if (!right && p.kind == Pick::Container)	// LMB: its window alone, from anywhere, the selection kept (user 2026-11-02, as the original)
	{
		if (container_locked(app.sim, p.id))
		{
			const bool sent = app.sim.interact(p.id, 3, shift);
			printf("sim click (%.0f, %.0f): lockpick %llu: %s\n", mx, my, (unsigned long long)p.id, sent ? "sent" : "not sent");
			order_sent(sent);
			return;
		}
		if (!cont_find(p.id)) { App::OpenCont oc; oc.id = p.id; oc.view = true; cont_add(oc); }	// already open: the click leaves it be
		printf("sim click (%.0f, %.0f): show container %llu\n", mx, my, (unsigned long long)p.id);
		return;
	}
	if (!right)
	{
		if (id && !app.sim.isPlayer(id))
		{
			app.inspect = id;
			app.panelProducer = 0;	// a character click moves the panel back to the character; the machine window stays (k16.19.8.2)
			printf("sim click (%.0f, %.0f): inspect %llu\n", mx, my, (unsigned long long)id);
			return;
		}
		if (id) { app.inspect = 0; }
		app.panelProducer = 0;
		app.sim.select(id, shift);
		printf("sim click (%.0f, %.0f): character %llu, %zu selected\n", mx, my, (unsigned long long)id, app.sim.selected().size());
		return;
	}
	// A double right click on one of your own characters enters direct drive on it as select + V, instead of a second order (k16.2.238.1,
	// user: "double left click already focused on a single character to follow. double right to direct drive?"). On the character being driven
	// it leaves. The first click keeps its normal order (below); an NPC or the ground never drives, so their two clicks order as they always did.
	if (id && app.sim.isPlayer(id) && !locked_cage(app.sim, id))
	{
		const double now = ui::now();
		const bool twice = app.lastRmbChar == id && now - app.lastRmbCharAt < kDoubleClick;
		app.lastRmbChar = twice ? 0 : id;	// a third click starts a new pair
		app.lastRmbCharAt = twice ? -1 : now;
		if (twice)
		{
			if (app.follow && app.followId == id)
			{
				printf("sim click (%.0f, %.0f): double right click, character %llu: leave the drive\n", mx, my, (unsigned long long)id);
				leave_follow();
			}
			else
			{
				app.inspect = 0;	// as a left click's select: the panel goes with the character
				app.panelProducer = 0;
				app.sim.select(id, false);
				set_follow_id(id);
				printf("sim click (%.0f, %.0f): double right click, character %llu: direct drive on\n", mx, my, (unsigned long long)id);
			}
			return;
		}
	}
	else { app.lastRmbChar = 0; app.lastRmbCharAt = -1; }
	bool sent = false;
	if (id && locked_cage(app.sim, id))	// a caged prisoner has no default order: the menus (opened on the press) are the choices (kenshi-k16.2.105.1)
	{
		printf("sim click (%.0f, %.0f): caged character %llu: no default order\n", mx, my, (unsigned long long)id);
		return;
	}
	if (id)
	{
		const CharAct a = char_act(app.sim, app.chars, id, ctrl_held());
		// Shift: the original's jobs (order 76): attack -> 4 (Fight enemies), follow -> 31 (Stay close), first aid -> 25 (Medic); the rest queue
		const uint16_t job = !shift ? 0 : a.action == kAttack ? 4 : a.action == kFollow ? 31 : a.action == 10 ? 25 : 0;
		if (job) { sent = app.sim.jobAdd(job, job == 31 ? id : 0); }
		else { sent = a.action == kAttack ? app.sim.attack(id) : a.action == kFollow ? app.sim.follow(id) : a.action == 5 ? open_body(id, shift) : a.action > 0 && app.sim.interact(id, (uint8_t)a.action, shift && a.action != 14); }	// stealth KO: queue 1 is bad_value
		printf("sim click (%.0f, %.0f): character %llu %s (%d)%s: %s\n", mx, my, (unsigned long long)id, a.cursor, a.action, job ? " as a job" : "", sent ? "sent" : "not sent");
	}
	else if (p.kind == Pick::Item)
	{
		sent = app.sim.interact(p.id, 7, shift);
		printf("sim click (%.0f, %.0f): pick up %llu%s: %s\n", mx, my, (unsigned long long)p.id, shift ? " (queued)" : "", sent ? "sent" : "not sent");
	}
	else if (p.kind == Pick::Bed)	// sleep (order 60 action 9); event 10 occupied / not_allowed come back as refusals
	{
		sent = app.sim.interact(p.id, 9, shift);
		printf("sim click (%.0f, %.0f): sleep in bed %llu%s: %s\n", mx, my, (unsigned long long)p.id, shift ? " (queued)" : "", sent ? "sent" : "not sent");
	}
	else if (p.kind == Pick::Producer)
	{
		app.panelProducer = p.id;	// the last clicked thing is the producer here too (k16.19.8.2): a mine order shows its panel
		sent = use_producer(p.id, shift);
		printf("sim click (%.0f, %.0f): use producer %llu%s: %s\n", mx, my, (unsigned long long)p.id, shift ? " (queued)" : "", sent ? "sent" : "not sent");
	}
	else if (p.kind == Pick::Container)
	{
		const simlink::Interactable* t = app.sim.thing(p.id);
		const bool locked = container_locked(app.sim, p.id);
		if (t && app.sim.foreign(p.id) && !app.keys[SAPP_KEYCODE_LEFT_ALT] && !app.keys[SAPP_KEYCODE_RIGHT_ALT])
		{
			// a foreign container's RMB without ALT (CURSORS_LIVE B4b storage / shop counter, B6 locked): the original's hint, and (orders.foreign_walk_to) the move
			// order to it at the same instant; no loot, no lockpick (ALT does those; user 2026-11-14, ask kenshi-k16.2.240.2: "Match it")
			ui::message(locked ? "Hold down the ALT key to pick the lock." : "Hold down the ALT key to steal items");	// the HUD message box (kenshi-rwh.161: the original posts both on the RMB press edge; the lock text ends in a full stop, the steal one does not)
			if (kForeignWalkTo.get())
			{
				const double at[3] = { t->use[0], t->use[1], t->use[2] };
				sent = app.sim.moveTo(at, shift);
			}
			printf("sim click (%.0f, %.0f): foreign %s %llu%s, no ALT: hint%s: %s\n", mx, my, locked ? "locked container" : "container", (unsigned long long)p.id, shift ? " (queued)" : "",
				kForeignWalkTo.get() ? " + walk, no steal/lockpick" : " only", sent ? "sent" : "not sent");
		}
		else if (locked)
		{
			sent = app.sim.interact(p.id, 3, shift);
			printf("sim click (%.0f, %.0f): lockpick %llu%s: %s\n", mx, my, (unsigned long long)p.id, shift ? " (queued)" : "", sent ? "sent" : "not sent");
		}
		else
		{
			sent = open_container(p.id, shift);
			printf("sim click (%.0f, %.0f): open container %llu%s: %s\n", mx, my, (unsigned long long)p.id, shift ? " (queued)" : "", sent ? "sent" : "not sent");
		}
	}
	else if (const simlink::Interactable* d = p.kind == Pick::Door ? app.sim.thing(p.id) : nullptr)
	{
		// locked: the click does nothing; the hold menu has Pick Lock (order 60 action 3, on the server since kenshi-h2v.15.1), unlock and bash. Broken: nothing to do
		const uint8_t action = d->state == simlink::Interactable::kOpen ? 2 : d->state == simlink::Interactable::kClosed ? 1 : 0;
		sent = action && app.sim.interact(p.id, action, shift);
		printf("sim click (%.0f, %.0f): door %llu %s%s: %s\n", mx, my, (unsigned long long)p.id, action == 2 ? "close" : action == 1 ? "open" : d->state == simlink::Interactable::kBroken ? "broken" : "locked", shift ? " (queued)" : "", sent ? "sent" : "not sent");
	}
	else if (p.kind == Pick::Ground)
	{
		// kenshi-k16.2.231.1: the shot's route sidecar records which building and floor a ground click meant (a floor/deck hit is a building shape)
		SimView::Click click;
		click.what = "click";
		click.building = p.building;
		double dir[3];
		mouse_ray(mx, my, dir);
		const double far = std::hypot(p.hit[0] - app.pos[0], p.hit[1] - app.pos[1], p.hit[2] - app.pos[2]) + 1.0;
		if (const int b = app.buildings.rayBuilding(app.pos, dir, far, p.hit); b >= 0)
		{
			click.buildingName = app.buildings.nameOfInstance(b);
			click.floor = app.buildings.floorAt(b, p.hit);
			rec->building = click.buildingName;
			rec->floor = click.floor;
		}
		sent = app.sim.moveTo(p.hit, shift, true, &click);
		printf("sim click (%.0f, %.0f): move_to %.0f %.0f: %s\n", mx, my, p.hit[0], p.hit[2], sent ? "sent" : "not sent");
		if (sent)
		{
			memcpy(app.orderAt, p.hit, sizeof p.hit);
			app.orderAge = 0;
			app.orderRejected = false;
			rec->seq = app.sim.lastSentSeq();
			app.orderSeq = app.sim.lastSentSeq();	// the marker moves to this order's route end when the sim answers (k16.2.259)
			app.orderRouted = false;
		}
	}
	// The order click (Right_Click, the order dispatcher 1407f9e20): once per issued order, after the whole selection got it, never one per
	// character. A context menu row's action does not come through here, so a move from a row plays no click (the original's menu-row flag).
	if (right && sent) { post_event(audio::kRightClick, "Right_Click"); }
	order_sent(sent);
}

// The sim's yaw (0 = +z, towards +x) of the camera's forward, and back
double heading_of(double camYaw) { return M_PI - camYaw; }

// The follow camera's default close view (k16.2.237 part 5): the distance the follow camera starts at (the wheel's 10..2000 zoom moves it after).
constexpr double kFollowDist = 60.0, kFollowPitch = -15 * M_PI / 180.0;	// units, rad
// The follow camera on any drawn character (a spectator's NPC): the selection is not needed, and the drive keys stay the player's (update_follow).
// Every entry starts at the default close camera behind the character, whatever the RTS zoom was, and a leave + re-enter the same (k16.2.237 part 5,
// user: "v - reengaging direct drive, it didn't restore to a 'default close' camera").
bool set_follow_id(uint64_t id)
{
	double pos[3], yaw;
	const simlink::Route* route;
	if (!id || !app.sim.pose(id, pos, yaw, route)) { return false; }
	const double fromRts = app.rts ? app.rtsDist : 0.0;	// the zoom it was at, for the scripted log: it does not carry over
	app.follow = true;
	app.followId = id;
	app.buildings.engage();
	app.followBase = M_PI - yaw;
	app.followOffset = 0;
	app.yaw = app.followBase;
	app.pitch = kFollowPitch;
	app.followDist = kFollowDist;
	app.followPulled = 1e9;
	app.followHold = 0;
	app.driveSent = 0; app.strafeSent = 0;
	if (app.scripted()) { printf("follow %llu: camera mode follow dist %.1f pitch %.1f (from rts dist %.1f)\n", (unsigned long long)id, app.followDist, app.pitch * 180.0 / M_PI, fromRts); }
	return true;
}

// The collision boom (kenshi-k16.2.289.1; user 2026-11-15: a follow camera never hides the character; shared with the RTS camera's look-up,
// kenshi-k16.2.353.1): a sphere kBoomRadius swept from the pivot back along the view to the wanted distance; solid world (terrain, walls, large building
// shapes, foliage) shortens the boom to just before the contact, at once. Thin props (CollisionPick::thin) are not in the way: the occluder cone dithers
// them (the frame's cone). It eases back out at `outRate` u/s once it has held kBoomHold s since the last blockage, so a post passed never makes it
// jitter. `pulled`/`hold` are the caller's state (1e9: free). minLen is the shortest the boom is cut to; sweep false skips the world test (the boom only
// eases out). Every constant is MINE.
struct Boom { double len, allowed; std::string blocker; };
Boom boom_step(const double pivot[3], const double back[3], double wanted, double minLen, double outRate, bool sweep, double dt, double& pulled, double& hold)
{
	constexpr double kBoomRadius = 3.0, kBoomMargin = 0.5, kBoomHold = 0.6, kGroundClear = 2.0, kStep = 3.0;	// u, u, s, u, u
	double allowed = wanted;
	std::string blocker;
	float bbox[6] = {};
	if (sweep)
	{
		if (double t; app.collide.sweep(pivot, back, allowed + kBoomMargin, kBoomRadius, t, bbox, app.scripted() ? &blocker : nullptr)) { allowed = std::max(t - kBoomMargin, minLen); }
		else { blocker.clear(); }
		if (char box[96]; app.scripted() && !blocker.empty()) { snprintf(box, sizeof box, " box %.0f %.0f %.0f - %.0f %.0f %.0f", bbox[0], bbox[1], bbox[2], bbox[3], bbox[4], bbox[5]); blocker += box; }
		for (double d = kStep; d < allowed + kStep; d += kStep)	// the terrain: the first step under the ground, then the last one over it
		{
			const double l = std::min(d, allowed);
			if (pivot[1] + back[1] * l < app.hm->height(pivot[0] + back[0] * l, pivot[2] + back[2] * l) + kGroundClear)
			{
				double lo = d - kStep, hi = l;
				for (int i = 0; i < 6; i++)
				{
					const double m = 0.5 * (lo + hi);
					(pivot[1] + back[1] * m < app.hm->height(pivot[0] + back[0] * m, pivot[2] + back[2] * m) + kGroundClear ? hi : lo) = m;
				}
				allowed = std::max(lo, minLen);
				break;
			}
			if (l >= allowed) { break; }
		}
	}
	if (allowed < std::min(pulled, wanted)) { pulled = allowed; hold = kBoomHold; }	// snap in
	else if ((hold -= dt) <= 0) { pulled = std::min(pulled + outRate * dt, allowed < wanted ? allowed : 1e9); }	// ease out, slowly
	if (pulled >= wanted) { pulled = 1e9; }
	return { std::min(wanted, pulled), allowed, blocker };
}

// Follow camera: behind the character at followDist, its yaw smoothed towards the character's facing, or towards the route ahead so the corners
// don't snap it; while driving the camera's yaw is the heading. Drive orders on change, steering at most at the tick rate.
void update_follow(double dt)
{
	constexpr double kTurnRate = 2.0, kSmooth = 2.5, kLookAhead = 15, kEye = 14, kMinHeadingStep = 0.01;	// MINE: rad/s, 1/s, units, units, rad
	double pos[3], yaw;
	const simlink::Route* route;
	if (!app.sim.pose(app.followId, pos, yaw, route)) { set_follow(false); return; }
	app.driveAge += dt;
	constexpr double kZoomRate = 1.5;	// MINE: e-folds/s of the distance
	const double zoom = (held("camera_zoom_out") ? 1 : 0) - (held("camera_zoom_in") ? 1 : 0);
	// kenshi-k16.2.237.7 (user k16.2.237 part 6: "any zoom is the 'disable cruise control'"): Home / End in the follow of a player character leave it like the wheel,
	// to the RTS camera on the same character, and zoom it this frame. A spectator's follow keeps its zoom.
	if (zoom != 0 && app.sim.isPlayer(app.followId)) { leave_follow(); update_rts(dt); return; }
	app.followDist = std::clamp(app.followDist * std::exp(zoom * kZoomRate * dt), 10.0, 2000.0);	// the scroll wheel's clamp
	if (app.sim.isPlayer(app.followId))
	{
		if (held("camera_back")) { app.autorun = false; }
		const uint8_t drive = held("camera_forward") || app.autorun ? 1 : held("camera_back") ? 2 : 0;
		// A / D steer; stopped, they turn in place (drive 0 with the heading: the sim turns only a character in its drive hold)
		// kenshi-k16.2.237.4: with mouse look held (the right button), A / D strafe (order 20's strafe, facing = the camera heading) and do not turn
		const double ad = (held("camera_right") ? 1 : 0) - (held("camera_left") ? 1 : 0);
		// SIMLINK sign as is (A = -1, D = +1). The sim's lateral is mirrored on screen until kenshi-k16.8.38 lands; the prediction's knav::Walker follows it.
		const int strafe = app.look ? (int)ad : 0;
		const double turn = app.look ? 0.0 : ad;
		app.yaw += turn * kTurnRate * dt;
		// kenshi-k16.2.237.1 (MINE): a backpedal keeps the body's facing and walks along -heading (SIMLINK "Direct drive"), so the heading sent for S is the body's
		// own yaw, not the camera's: A / D with S turn the camera while the body backs away straight (a turning heading walked it sideways across its facing,
		// a forward / lateral mix no clip fits: the S+D of user 2026-11-14)
		const double heading = drive == 2 && !strafe ? yaw : heading_of(app.yaw);
		if (drive != app.driveSent || strafe != app.strafeSent) { send_drive(drive, heading, strafe); }
		else if ((drive || turn || strafe) && app.driveAge >= 1 / SimView::kTickRate && std::abs(wrap_angle(heading - app.headingSent)) > kMinHeadingStep) { send_drive(drive, heading, strafe); }
		if (drive || strafe) { app.sim.aim(app.followId, wrap_angle(heading)); }
		if (drive || turn || strafe)
		{
			app.followBase = app.yaw;
			app.followOffset = 0;
		}
	}
	double rawLook = yaw;
	if (!driving() && !(app.sim.isPlayer(app.followId) && (held("camera_right") || held("camera_left"))))
	{
		double look = yaw;
		if (route && !route->points.empty())
		{
			const auto* p = &route->points.back();
			// kenshi-k16.2.237.7: the wire's route keeps the waypoints already passed (it is the whole path from the order's start, n stays 5 as the body
			// walks it), so a passed waypoint 15 u behind the body counted as the look-ahead and pointed the camera backwards. The look-ahead starts at
			// the end of the route segment nearest the body.
			size_t first = 0;
			double bestD = 1e30;
			for (size_t i = 0; i + 1 < route->points.size(); i++)
			{
				const auto &u = route->points[i], &v = route->points[i + 1];
				const double ex = v[0] - u[0], ez = v[2] - u[2], l2 = ex * ex + ez * ez;
				const double t = l2 > 0 ? std::clamp(((pos[0] - u[0]) * ex + (pos[2] - u[2]) * ez) / l2, 0.0, 1.0) : 0.0;
				const double d = std::hypot(pos[0] - (u[0] + t * ex), pos[2] - (u[2] + t * ez));
				if (d < bestD - 1e-6) { bestD = d; first = i + 1; }
			}
			for (size_t i = first; i < route->points.size(); i++) { const auto& q = route->points[i]; if (std::hypot(q[0] - pos[0], q[2] - pos[2]) >= kLookAhead) { p = &q; break; } }
			if (std::hypot((*p)[0] - pos[0], (*p)[2] - pos[2]) > 1) { look = std::atan2((*p)[0] - pos[0], (*p)[2] - pos[2]); }
		}
		// kenshi-k16.2.237.5 (user 2026-11-14: on a right-click walk down a hill the camera swung to the character's front): the route's look-ahead can lie
		// behind the drawn body (a switchback, a corner the body has not turned into yet, the drawn facing trailing the sim's), and the camera followed it
		// round to the front. The camera stays behind the drawn facing: a route heading more than kMaxRouteTurn off it is not followed.
		constexpr double kMaxRouteTurn = 60 * M_PI / 180;	// MINE
		rawLook = look;
		if (std::abs(wrap_angle(look - yaw)) > kMaxRouteTurn) { look = yaw; }
		// a body that turns sharply (a corner, a switchback) drags the camera round faster the further it is off, so it is never long in front
		const double dev = wrap_angle(M_PI - look - app.followBase);
		app.followBase += dev * std::min(1.0, (kSmooth + 4.0 * std::abs(dev)) * dt);
		app.yaw = app.followBase + app.followOffset;
	}
	{ static const bool followLog = std::getenv("KCLIENT_FOLLOWCAM_LOG") != nullptr; if (followLog && app.scripted()) { printf("followcam: frame %llu cam_heading %.1f drawn_yaw %.1f diff %.1f route_look %.1f route %s drive %u\n", (unsigned long long)app.frames, heading_of(app.yaw) * 180.0 / M_PI, yaw * 180.0 / M_PI, wrap_angle(heading_of(app.yaw) - yaw) * 180.0 / M_PI, rawLook * 180.0 / M_PI, route && !route->points.empty() ? "yes" : "no", app.driveSent); } }	// kenshi-k16.2.237.5: the camera's behind-heading against the drawn facing, per frame
	if (app.scripted() && std::getenv("KDLOOK"))	// kenshi-k16.2.237.9: per frame mouse delta, sent / predicted / server heading, drawn yaw, camera yaw, eye
	{
		const simlink::Dynamic& d = app.sim.decoder().chars.at(app.followId).cur;
		printf("dlook: %llu dx %.1f sent %.4f pred %.4f server %.4f serveryaw %.4f drawn %.4f cam %.4f pos %.2f %.2f eye %.2f %.2f %.2f\n", (unsigned long long)app.frames, app.lookDx, app.headingSent, app.sim.localHeading(), (double)d.heading, (double)d.yaw, yaw, heading_of(app.yaw), pos[0], pos[2], app.pos[0], app.pos[1], app.pos[2]);
		app.lookDx = 0;
	}
	if (app.scripted() && app.frames % 10 == 0 && app.sim.predicting())
	{
		const simlink::Dynamic& d = app.sim.decoder().chars.at(app.followId).cur;
		printf("predict: shown %.1f %.1f  server %.1f %.1f (tick %llu, drive %u) y shown %.2f server %.2f\n", pos[0], pos[2], d.pos[0], d.pos[2], (unsigned long long)d.tick, d.drive, pos[1], (double)d.pos[1]);
	}
	constexpr double kBoomMin = 4.0;	// u
	double f[3];
	forward_vec(app.yaw, app.pitch, f);
	const double pivot[3] = { pos[0], pos[1] + kEye, pos[2] };
	const double back[3] = { -f[0], -f[1], -f[2] };
	const Boom b = boom_step(pivot, back, app.followDist, kBoomMin, 30.0, true, dt, app.followPulled, app.followHold);
	for (int k = 0; k < 3; k++) { app.pos[k] = pivot[k] + back[k] * b.len; }
	app.followBoom = b.len;
	if (app.scripted()) { printf("boom: len %.2f wanted %.2f allowed %.2f hold %.2f cam %.1f %.1f %.1f %s\n", b.len, app.followDist, b.allowed, std::max(app.followHold, 0.0), app.pos[0], app.pos[1], app.pos[2], b.blocker.c_str()); }
}

// Leaving direct drive (V, Esc, or a double right click on the driven character; k16.2.238.1): back to the floating camera, auto-centred on the
// character (the camera stays where the drive left it, as V's own toggle does).
void leave_follow()
{
	const uint64_t was = app.followId;
	set_rts(was);	// drops the follow (and a running drive order) itself
	if (app.scripted()) { printf("follow %llu: left, camera mode rts center %llu\n", (unsigned long long)was, (unsigned long long)was); }
}

// Floating camera (kinspect findings/camera_rts.md): WASD pans the focus at 500 d / 600 u/s and ends the auto-center, Q/E orbit, , / . tilt,
// Home / End zoom (MINE rate). The focus floats on the ground (MINE smoothing), leashed within kTether of the nearest selected character (else squad
// member); the camera keeps min(0.2 d, 20) + 10 over the ground by lifting the focus. Auto-center: the focus is the character + kCenterLift, no lag.
void update_rts(double dt)
{
	constexpr double kOrbitRate = 54 * M_PI / 180.0, kTiltRate = 12 * M_PI / 180.0, kZoomRate = 1.5, kSmooth = 8, kTether = 2250, kCenterLift = 10.8;	// rad/s, rad/s, e-folds/s (MINE), 1/s (MINE), u, u
	constexpr double kMinPitch = -66.9 * M_PI / 180.0, kMaxPitch = 80.0 * M_PI / 180.0;	// the original stops at +11.5; past it the collision boom shortens the orbit (kenshi-k16.2.353.1)
	const double panX = (held("camera_right") ? 1 : 0) - (held("camera_left") ? 1 : 0), panZ = (held("camera_forward") ? 1 : 0) - (held("camera_back") ? 1 : 0);
	double at[3];
	float hull[3];
	if (panX || panZ || (app.centerId && !app.sim.placed(app.centerId, at, hull))) { app.centerId = 0; }
	app.yaw += ((held("camera_rotate_right") ? 1 : 0) - (held("camera_rotate_left") ? 1 : 0)) * kOrbitRate * dt;
	app.pitch = std::clamp(app.pitch + ((held("camera_tilt+") ? 1 : 0) - (held("camera_tilt-") ? 1 : 0)) * kTiltRate * dt, kMinPitch, kMaxPitch);
	const double zoom = (held("camera_zoom_out") ? 1 : 0) - (held("camera_zoom_in") ? 1 : 0);
	app.rtsDist = std::clamp(app.rtsDist * std::exp(zoom * kZoomRate * dt), 10.0, 2000.0);
	if (app.centerId)
	{
		app.focusOn = "character";
		std::copy(at, at + 3, app.focus);
		app.focus[1] += kCenterLift;
	}
	else
	{
		const double speed = 500 * app.rtsDist / 600 * dt * (held("debug.fly_fast") ? kFastFactor : 1);
		const double sy = std::sin(app.yaw), cy = std::cos(app.yaw);	// the view's horizontal forward is (sy, -cy), its right (cy, sy)
		app.focus[0] += (sy * panZ + cy * panX) * speed;
		app.focus[2] += (-cy * panZ + sy * panX) * speed;
		double best = 1e30, near[3] = {};
		for (uint64_t id : app.sim.selected().empty() ? app.bar : app.sim.selected())
		{
			if (!app.sim.placed(id, at, hull)) { continue; }
			const double d = std::hypot(at[0] - app.focus[0], at[2] - app.focus[2]);
			if (d < best) { best = d; std::copy(at, at + 3, near); }
		}
		if (best > kTether && best < 1e30)
		{
			app.focus[0] = near[0] + (app.focus[0] - near[0]) * kTether / best;
			app.focus[2] = near[2] + (app.focus[2] - near[2]) * kTether / best;
		}
		const double lim = kHalfWorld - 1;
		app.focus[0] = std::clamp(app.focus[0], -lim, lim);
		app.focus[2] = std::clamp(app.focus[2], -lim, lim);
		app.focus[1] += (ground_under(app.focus[0], app.focus[2], &app.focusOn) - app.focus[1]) * std::min(1.0, kSmooth * dt);
	}
	double f[3];
	forward_vec(app.yaw, app.pitch, f);
	// Looking up (user 2026-11-18: look at the sky): the follow camera's collision boom (boom_step) shortens the orbit as the ground (indoors the
	// floor, walls, ceiling) comes between the focus and the eye, and eases back out to rtsDist, the player's zoom, as the view tilts down. At
	// or below the horizon the orbit is the original's, untouched.
	const double back[3] = { -f[0], -f[1], -f[2] };
	const Boom b = boom_step(app.focus, back, app.rtsDist, 0.0, std::max(30.0, 1.5 * app.rtsDist), f[1] > 0, dt, app.rtsPulled, app.rtsHold);
	// The boom's length blends in over pitch 0..11.5 deg (smoothstep), so tilting up from the horizon never jumps the orbit (kenshi-k16.2.353.2);
	// below 11.5 the ground lift below keeps the eye out of the ground, as the original did.
	const double wt = std::clamp(app.pitch / (11.5 * M_PI / 180.0), 0.0, 1.0), w = wt * wt * (3 - 2 * wt);
	const double len = app.rtsDist + (b.len - app.rtsDist) * w;
	app.rtsBoom = len;
	for (int k = 0; k < 3; k++) { app.pos[k] = app.focus[k] + back[k] * len; }
	const double lift = app.hm->height(app.pos[0], app.pos[2]) + std::min(0.2 * len, 20.0) + 10 - app.pos[1];
	if (lift > 0)
	{
		if (w < 1 || b.len >= app.rtsDist) { app.focus[1] += lift; }	// the original's: the focus rides up with the eye; a shortened boom lifts the eye alone, or the focus would climb
		app.pos[1] += lift;
	}
	if (app.scripted() && app.rtsLog) { printf("rtsboom: pitch %.3f len %.2f dist %.1f allowed %.2f eye %.1f %.1f %.1f ground %.1f\n", app.pitch * 180.0 / M_PI, len, app.rtsDist, b.allowed, app.pos[0], app.pos[1], app.pos[2], app.hm->height(app.pos[0], app.pos[2])); }
}

}
