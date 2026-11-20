// debug.route_preview: see routepreview.hpp.
#include "dev/routepreview.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "app/config.hpp"
#include "app/eventlog.hpp"
#include "knav/route.hpp"
#include "world/clientnav.hpp"

namespace dev::routepreview
{
namespace
{
const cfg::Bool kRoutePreview{ cfg::Group{ "Route preview" }, "debug.route_preview", false, "Debug (kenshi-k16.2.331.2): with one character selected, the move_to route the cursor's ground point would get, planned live by the sim's own planner (knav::planRoute) on the client's nav tiles, drawn magenta. A WARN line (events file, stderr) when the cursor moved under 3 u but the route length jumped, and one when a click's server route differs from the preview for that goal" };

constexpr double kCursorStill = 3.0;	// u: a cursor move this small should not change the route much
constexpr double kJumpRatioDefault = 2.0, kJumpAbsDefault = 20.0;	// u: the jump that is logged (x2 or more AND more than 20 u, either way)
constexpr double kClickRel = 0.25, kClickAbs = 20.0, kClickEnd = 5.0;	// the click cross-check's thresholds
constexpr double kWarnGap = 0.5;	// s between jump warnings

// KCLIENT_ROUTE_PREVIEW_JUMP=RATIO,ABS lowers the jump thresholds (a test of the log lines on routes that do not jump by themselves).
void jumpLimits(double& ratio, double& abs)
{
	static double r = kJumpRatioDefault, a = kJumpAbsDefault;
	static bool read = false;
	if (!read) { read = true; if (const char* e = getenv("KCLIENT_ROUTE_PREVIEW_JUMP")) { sscanf(e, "%lf,%lf", &r, &a); } }
	ratio = r;
	abs = a;
}

using Pts = std::vector<std::array<float, 3>>;

struct Plan
{
	bool valid = false;	// planned (even a failed plan has the inputs set)
	knav::RoutePlan plan;
	double cursor[3] = {}, from[3] = {};
	double tick = 0;
	uint64_t id = 0;
};

Plan g_last;	// the newest hover plan
Plan g_click;	// the plan for the last click's goal
uint32_t g_clickSeq = 0;
bool g_clickOpen = false;
Pts g_draw;
bool g_approach = false;
std::string g_status;
std::chrono::steady_clock::time_point g_lastWarn;

double length(const Pts& p)
{
	double sum = 0;
	for (size_t i = 1; i < p.size(); i++) { sum += std::sqrt(std::pow(p[i][0] - p[i - 1][0], 2) + std::pow(p[i][1] - p[i - 1][1], 2) + std::pow(p[i][2] - p[i - 1][2], 2)); }
	return sum;
}

double dist(const double a[3], const double b[3]) { return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2])); }

std::string fmt(const Pts& p)
{
	std::string s;
	char b[96];
	for (const auto& q : p) { snprintf(b, sizeof b, "%s(%.1f,%.1f,%.1f)", s.empty() ? "" : " ", q[0], q[1], q[2]); s += b; }
	return s.empty() ? "(none)" : s;
}

std::string flags(const knav::RoutePlan& r)
{
	std::string s = r.found ? "found" : std::string("failed:") + std::string(r.check);
	if (r.approach) { s += " approach"; }
	if (r.reseated) { s += " reseated"; }
	return s;
}

const char* held(const ClientNav& nav, const double at[3])
{
	switch (nav.walkable(at)) { case ClientNav::Reach::kYes: return "held, walkable"; case ClientNav::Reach::kNo: return "held, no mesh there"; default: return "not held (tile not streamed)"; }
}

Plan plan(const ClientNav& nav, const Subject& s, const double goal[3], double tick)
{
	Plan out;
	const knav::NavMesh* mesh = nav.mesh();
	const knav::NavQuery* query = nav.query();
	if (!mesh || !query) { return out; }
	knav::RouteRequest rq;
	rq.from = { s.from[0], s.from[1], s.from[2] };
	rq.toX = goal[0];
	rq.toZ = goal[2];
	rq.planned = false;
	rq.radius = s.radius;
	rq.water = s.water;
	rq.clickGoal = true;
	out.plan = knav::planRoute(*mesh, *query, nav.routeSets(), rq);
	out.valid = true;
	for (int k = 0; k < 3; k++) { out.cursor[k] = goal[k]; out.from[k] = s.from[k]; }
	out.tick = tick;
	out.id = s.id;
	return out;
}

Pts pts(const Plan& p) { Pts r; for (const knav::Point& q : p.plan.points) { r.push_back(q); } return r; }

void warn(const char* kind, double tick, uint64_t id, const std::string& text)
{
	evlog::add(kind, tick, id, "%s", text.c_str());
	fprintf(stderr, "WARN %s: %s\n", kind, text.c_str());
}
}	// namespace

bool on() { return kRoutePreview.get(); }
const Pts& points() { return g_draw; }
bool approach() { return g_approach; }
std::string status() { return on() ? g_status : std::string(); }

void update(const ClientNav& nav, const Subject* subject, const double* hit, double tick)
{
	if (!on()) { g_draw.clear(); g_approach = false; g_status.clear(); g_last = Plan{}; g_click = Plan{}; g_clickOpen = false; return; }
	if (!subject || !hit || !nav.ready())
	{
		g_draw.clear();
		g_approach = false;
		g_status = !nav.ready() ? "route preview: no mesh yet" : !subject ? "route preview: select one character" : "route preview: no ground under the cursor";
		g_last = Plan{};
		return;
	}
	if (g_last.valid && g_last.id == subject->id && dist(g_last.cursor, hit) < 0.01) { return; }	// one plan per cursor change (and at most one per frame: this runs once a frame)
	Plan now = plan(nav, *subject, hit, tick);
	if (!now.valid) { return; }
	g_draw = pts(now);
	g_approach = now.plan.found && now.plan.approach;
	char b[200];
	if (now.plan.found) { snprintf(b, sizeof b, "route preview: %.1f u, %zu points%s", length(g_draw), g_draw.size(), g_approach ? ", approach point" : ""); g_status = b; }
	else { g_status = "route preview: no route (" + std::string(now.plan.check) + ")"; g_draw.clear(); }
	static const bool trace = getenv("KCLIENT_ROUTE_PREVIEW_TRACE") != nullptr;	// one stdout line per plan, for scripted runs
	if (trace) { printf("route preview: tick %.1f cursor (%.1f,%.1f,%.1f) from (%.1f,%.1f,%.1f): %s, %.1f u\n", tick, now.cursor[0], now.cursor[1], now.cursor[2], now.from[0], now.from[1], now.from[2], flags(now.plan).c_str(), length(g_draw)); }
	if (g_last.valid && g_last.id == subject->id && g_last.plan.found && now.plan.found && !(g_last.plan.approach && now.plan.approach) && dist(g_last.cursor, now.cursor) < kCursorStill && dist(g_last.from, now.from) < kCursorStill)
	{
		const Pts a = pts(g_last), c = pts(now);
		const double la = length(a), lc = length(c), hi = std::fmax(la, lc), lo = std::fmin(la, lc);
		double ratio, absolute;
		jumpLimits(ratio, absolute);
		const bool jump = hi >= ratio * std::fmax(lo, 1e-3) && hi - lo > absolute;
		const auto nowT = std::chrono::steady_clock::now();
		if (jump && std::chrono::duration<double>(nowT - g_lastWarn).count() >= kWarnGap)
		{
			g_lastWarn = nowT;
			char head[512];
			snprintf(head, sizeof head, "route length jump %.1f u -> %.1f u for a cursor move of %.2f u: tick %.1f, cursor (%.1f,%.1f,%.1f) -> (%.1f,%.1f,%.1f); ",
				la, lc, dist(g_last.cursor, now.cursor), tick, g_last.cursor[0], g_last.cursor[1], g_last.cursor[2], now.cursor[0], now.cursor[1], now.cursor[2]);
			std::string t = head;
			t += "snapped start " + (a.empty() ? std::string("-") : fmt({ a.front() })) + " -> " + (c.empty() ? std::string("-") : fmt({ c.front() }));
			t += ", snapped goal " + fmt({ a.back() }) + " -> " + fmt({ c.back() });
			t += "; old route [" + fmt(a) + "] " + flags(g_last.plan) + "; new route [" + fmt(c) + "] " + flags(now.plan);
			warn("route_jump", tick, subject->id, t);
		}
	}
	g_last = std::move(now);
}

void onOrder(const ClientNav& nav, const Subject& subject, const double goal[3], uint32_t seq, double tick)
{
	if (!on() || !nav.ready()) { return; }
	g_click = plan(nav, subject, goal, tick);
	g_clickSeq = seq;
	g_clickOpen = g_click.valid;
}

void onServerRoute(const ClientNav& nav, uint32_t seq, const Pts& points, uint8_t routeFlags, const double now[3], double tick)
{
	if (!on() || !g_clickOpen || seq != g_clickSeq) { return; }
	g_clickOpen = false;
	const Pts mine = pts(g_click);
	Pts server;
	server.push_back({ (float)now[0], (float)now[1], (float)now[2] });
	for (const auto& p : points) { server.push_back(p); }
	const double lm = length(mine), ls = length(server);
	const double endGap = !mine.empty() && !points.empty() ? std::sqrt(std::pow(mine.back()[0] - points.back()[0], 2) + std::pow(mine.back()[1] - points.back()[1], 2) + std::pow(mine.back()[2] - points.back()[2], 2)) : 0.0;
	const bool failed = !g_click.plan.found;
	const bool lenDiff = !failed && std::fabs(lm - ls) > kClickRel * std::fmax(lm, 1e-3) && std::fabs(lm - ls) > kClickAbs;
	const bool endDiff = !failed && endGap > kClickEnd;
	if (getenv("KCLIENT_ROUTE_PREVIEW_TRACE")) { printf("route preview: click seq %u: preview %.1f u vs server %.1f u, ends %.1f u apart: %s\n", seq, lm, ls, endGap, failed || lenDiff || endDiff ? "MISMATCH" : "silent"); }
	if (!failed && !lenDiff && !endDiff) { return; }
	char head[512];
	snprintf(head, sizeof head, "click route mismatch seq %u: preview %.1f u (%zu points) vs server %.1f u (%zu waypoints), ends %.1f u apart; tick %.1f; goal (%.1f,%.1f,%.1f); ",
		seq, lm, mine.size(), ls, points.size(), endGap, tick, g_click.cursor[0], g_click.cursor[1], g_click.cursor[2]);
	std::string t = head;
	char pos[256];
	snprintf(pos, sizeof pos, "start wire at click (%.1f,%.1f,%.1f) vs the character at the server route's arrival (%.1f,%.1f,%.1f); ", g_click.from[0], g_click.from[1], g_click.from[2], now[0], now[1], now[2]);
	t += pos;
	t += "preview [" + fmt(mine) + "] " + flags(g_click.plan) + "; server [" + fmt(points) + "]" + ((routeFlags & 2) ? " approach" : "");
	t += std::string("; tiles at the start: ") + held(nav, g_click.from) + ", at the goal: " + held(nav, g_click.cursor);
	t += "; likely cause when the planner is shared: the client's mesh (only streamed tiles, layer 0 per tile key, door paint only via tile patches) or the start / body input";
	warn("route_mismatch", tick, g_click.id, t);
}
}	// namespace dev::routepreview
