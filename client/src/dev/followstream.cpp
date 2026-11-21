// Side-by-side follow mode (docs/FOLLOW_STREAM.md, kenshi-cym.40.2.5). See followstream.hpp. Dev only.
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdarg>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "bridge/simview.hpp"
#include "dev/devhooks.hpp"
#include "dev/driftview.hpp"
#include "dev/followparse.hpp"
#include "dev/origstore.hpp"

#ifndef _WIN32
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace dev
{
namespace
{
	using Clock = std::chrono::steady_clock;
	double nowSec() { return std::chrono::duration<double>(Clock::now().time_since_epoch()).count(); }

	struct Cam { double pivot[3] = {}, yaw = 0, pitch = 0, dist = 150; int floor = 0; };

	struct S
	{
		bool enabled = false, importGiven = false, reportGiven = false;
		std::string address, importSave, importReport;
		int port = 0;
		FollowHost host;
		bool inited = false;
		bool logFrames = false;	// KCLIENT_FOLLOW_LOG=1: one camera line per frame

		int fd = -1;
		bool connecting = false, connected = false;
		double retryAt = 0;
		follow::LineReader reader;
		std::string outbuf;
		int connections = 0;

		bool beginDone = false, readySent = false, everReady = false;
		bool lockOn = true, lockToasted = false;
		std::set<std::string> malformedLogged;
		uint64_t dropped = 0;

		// camera
		bool haveTarget = false, camPlaced = false;
		Cam target, cur;
		double lastCamAt = 0, camLogAt = 0;
		int appliedFloor = -1;
		uint64_t camRecords = 0;

		// speed
		int pendPaused = -1;	// 0 / 1: the pause order sent and not yet seen in the sim's clock
		double pendSpeed = 0;	// > 0: the speed order sent and not yet seen
		double lastFrameAt = 0, reconPauseAt = -10;

		// orders sent, awaiting the sim's event 9 / 10 (matched by clientSeq)
		struct PendingOrder { uint32_t seq; std::string text; double since; };
		std::vector<PendingOrder> pendingOrders;

		// selection
		std::vector<uint64_t> wantSelect;
		bool selectPending = false;
		uint64_t squadShowId = 0;	// the selected character whose squad tab the HUD still has to show (the squad list may arrive after the select)
		double selectSince = 0;

		// the clock beat
		double lastClockSent = 0, lastClockRecv = 0;
		bool clockMissLogged = false, clockSeen = false;
		double lastClockT = 0;
	};
	S& st() { static S s; return s; }

	bool simUp(SimView& sim) { return sim.active() && sim.live(); }

	void logLine(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
	void logLine(const char* fmt, ...)
	{
		char b[512];
		va_list ap;
		va_start(ap, fmt);
		vsnprintf(b, sizeof b, fmt, ap);
		va_end(ap);
		printf("follow: %s\n", b);
		fflush(stdout);
	}

#ifndef _WIN32
	void closeFd(S& s)
	{
		if (s.fd >= 0) { ::close(s.fd); }
		s.fd = -1;
		s.connecting = s.connected = false;
		s.reader.clear();
		s.outbuf.clear();
	}

	void sendLine(S& s, const std::string& line)
	{
		if (!s.connected) { return; }
		s.outbuf += line + "\n";
	}

	void flush(S& s)
	{
		while (s.connected && !s.outbuf.empty())
		{
			const ssize_t n = ::send(s.fd, s.outbuf.data(), s.outbuf.size(), MSG_NOSIGNAL);
			if (n > 0) { s.outbuf.erase(0, (size_t)n); continue; }
			if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) { return; }
			logLine("send failed (%s): dropping the connection", strerror(errno));
			closeFd(s);
			s.retryAt = nowSec() + 2.0;
			return;
		}
	}

	void startConnect(S& s)
	{
		s.fd = ::socket(AF_INET, SOCK_STREAM, 0);
		if (s.fd < 0) { s.retryAt = nowSec() + 2.0; return; }
		fcntl(s.fd, F_SETFL, fcntl(s.fd, F_GETFL, 0) | O_NONBLOCK);
		sockaddr_in a{};
		a.sin_family = AF_INET;
		a.sin_port = htons((uint16_t)s.port);
		inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
		const int r = ::connect(s.fd, (sockaddr*)&a, sizeof a);
		if (r == 0) { s.connected = true; }
		else if (errno == EINPROGRESS) { s.connecting = true; }
		else { closeFd(s); s.retryAt = nowSec() + 2.0; }
	}
#else
	void closeFd(S&) {}
	void sendLine(S&, const std::string&) {}
	void flush(S&) {}
	void startConnect(S& s) { s.retryAt = nowSec() + 60.0; }
#endif

	// ---- applying the records -------------------------------------------------------------------------------------------------------------------

	void applyCam(S& s, const follow::Record& r)
	{
		// pos is the pivot; rot its orientation (Ogre w,x,y,z); the view looks along R*(+Z): as place_session_camera does for CAMERA_START
		const double w = r.rot[0], x = r.rot[1], y = r.rot[2], z = r.rot[3];
		const double fx = 2 * (x * z + w * y), fy = 2 * (y * z - w * x), fz = 1 - 2 * (x * x + y * y);
		Cam c;
		std::copy(r.pos, r.pos + 3, c.pivot);
		c.yaw = std::atan2(fx, -fz);
		c.pitch = std::asin(std::clamp(fy, -1.0, 1.0));
		c.dist = std::clamp(r.zoom, 10.0, 2000.0);
		c.floor = r.floor;
		s.target = c;
		s.haveTarget = true;
		s.camRecords++;
		const double t = nowSec();
		if (t - s.camLogAt >= 1.0 || s.camRecords <= 3)
		{
			s.camLogAt = t;
			logLine("cam record #%llu pivot %.3f %.3f %.3f yaw %.2f pitch %.2f dist %.1f floor %d", (unsigned long long)s.camRecords, c.pivot[0], c.pivot[1], c.pivot[2], c.yaw * 180 / M_PI, c.pitch * 180 / M_PI, c.dist, c.floor);
		}
	}

	void applySpeed(S& s, double v)
	{
		SimView& sim = s.host.sim();
		const auto& c = sim.clock();
		const bool paused = c.pauseBits & 1;
		if (s.pendPaused >= 0 && (s.pendPaused != 0) == paused) { s.pendPaused = -1; }
		if (s.pendSpeed > 0 && std::fabs(c.speed - s.pendSpeed) < 1e-3) { s.pendSpeed = 0; }
		const bool curPaused = s.pendPaused >= 0 ? s.pendPaused != 0 : paused;
		const double curSpeed = s.pendSpeed > 0 ? s.pendSpeed : c.speed;
		if (v <= 0)
		{
			if (!curPaused) { sim.pause(true); s.pendPaused = 1; logLine("speed 0: pause sent"); }
			else { logLine("speed 0: already paused"); }
			return;
		}
		if (curPaused) { sim.pause(false); s.pendPaused = 0; logLine("speed %.2f: unpause sent", v); }
		if (std::fabs(curSpeed - v) > 1e-3) { sim.setSpeed((float)v); s.pendSpeed = v; logLine("speed %.2f: set_speed sent (was %.2f)", v, curSpeed); }
		else if (!curPaused) { logLine("speed %.2f: already there", v); }
	}

	void trySelect(S& s)
	{
		if (!s.selectPending) { return; }
		SimView& sim = s.host.sim();
		std::vector<uint64_t> ok;
		for (const uint64_t id : s.wantSelect) { if (sim.isPlayer(id)) { ok.push_back(id); } }
		const bool all = ok.size() == s.wantSelect.size();
		if (!all && nowSec() - s.selectSince < 5.0) { return; }	// the characters may still be streaming in
		for (size_t i = 0; i < ok.size(); i++) { sim.select(ok[i], i != 0); }
		if (!all) { logLine("select: %zu of %zu ids are not player characters here, left out", s.wantSelect.size() - ok.size(), s.wantSelect.size()); }
		logLine("select: %zu ids applied", ok.size());
		s.squadShowId = ok.empty() ? 0 : ok[0];
		s.selectPending = false;
	}

	void applyOrder(S& s, const follow::Record& r)
	{
		SimView& sim = s.host.sim();
		std::string ids;
		for (const uint64_t id : r.ids) { ids += (ids.empty() ? "" : ",") + std::to_string(id); }
		if (r.kind == follow::Kind::kUnmapped)
		{
			logLine("order unmapped ids=%s what=\"%s\" (not applied)", ids.c_str(), r.what.c_str());
			return;
		}
		// the s6 table: the original's task type per kind; a src the table lacks is logged, not applied
		const char* kindName = r.kind == follow::Kind::kMove ? "move" : r.kind == follow::Kind::kAttack ? "attack" : "interact";
		bool inTable = r.src < 0;
		if (r.kind == follow::Kind::kMove) { inTable = inTable || r.src == 29; }
		else if (r.kind == follow::Kind::kAttack) { inTable = inTable || r.src == 5; }
		else
		{
			const std::map<int, int> byAction = { { 5, 26 }, { 1, 72 }, { 2, 73 }, { 3, 76 } };	// action -> task type
			const auto it = byAction.find(r.action);
			if (it == byAction.end()) { logLine("order interact ids=%s: action %d not in the table (not applied)", ids.c_str(), r.action); return; }
			inTable = inTable || r.src == it->second;
		}
		if (!inTable) { logLine("order %s ids=%s: src %d is not in the table for this kind (not applied)", kindName, ids.c_str(), r.src); return; }
		for (const uint64_t id : r.ids)
		{
			if (!sim.isPlayer(id)) { logLine("order %s ids=%s: id %llu is not a player character here (not applied)", kindName, ids.c_str(), (unsigned long long)id); return; }
		}
		const std::vector<uint64_t> keep = sim.selected();
		for (size_t i = 0; i < r.ids.size(); i++) { sim.select(r.ids[i], i != 0); }
		bool sent = false;
		if (r.kind == follow::Kind::kMove)
		{
			SimView::Click click;
			click.what = "follow";
			sent = sim.moveTo(r.pos, r.queue, true, &click);
		}
		else if (r.kind == follow::Kind::kAttack) { sent = sim.attack(r.target); }
		else { sent = sim.interact(r.target, (uint8_t)r.action, r.queue, r.action == 5 ? 1 : 0); }
		logLine("order %s ids=%s%s -> %s%s (src %d)", kindName, ids.c_str(), r.kind == follow::Kind::kMove ? (" to " + std::to_string(r.pos[0]) + "," + std::to_string(r.pos[1]) + "," + std::to_string(r.pos[2])).c_str() : "",
			sent ? "sent" : "NOT sent", r.queue ? " queued" : "", r.src);
		if (sent)
		{
			std::string what = std::string(kindName) + " ids=" + ids;
			s.pendingOrders.push_back({ sim.lastSentSeq(), what, nowSec() });
		}
		for (size_t i = 0; i < keep.size(); i++) { sim.select(keep[i], i != 0); }
	}

	void handleRecord(S& s, const follow::Record& r)
	{
		switch (r.type)
		{
			case follow::Type::kHello: logLine("agent hello v=%d", r.v); return;
			default: break;
		}
		if (!s.readySent) { s.dropped++; if (s.dropped == 1) { logLine("records before ready are dropped (first: type %d)", (int)r.type); } return; }
		switch (r.type)
		{
			case follow::Type::kAlign:
			{
				follow::origStore().setAlign(r.t);
				const auto& c = s.host.sim().clock();
				drift::onAlign(r.t, ((double)c.day - 1.0) * 24.0 + c.hour);
				logLine("align t=%.6f h", r.t);
				break;
			}
			case follow::Type::kCam: applyCam(s, r); break;
			case follow::Type::kSpeed: applySpeed(s, r.speed); break;
			case follow::Type::kSelect:
				if (r.ids.empty())	// a real empty selection (s4): the sim view holds it empty while the stream drives (kenshi-cym.40.2.9)
				{
					s.selectPending = false;
					s.host.sim().selectNone();
					logLine("select: empty set applied (selection now %zu)", s.host.sim().selected().size());
					break;
				}
				s.wantSelect = r.ids; s.selectPending = true; s.selectSince = nowSec(); trySelect(s);
				break;
			case follow::Type::kOrder: applyOrder(s, r); break;
			case follow::Type::kOrig:
			{
				follow::OrigSample o;
				o.t = r.t; std::copy(r.pos, r.pos + 3, o.pos); o.yaw = r.yaw; o.state = (uint8_t)r.state;
				follow::origStore().put(r.target, o);
				break;
			}
			case follow::Type::kClock:
				s.lastClockRecv = nowSec(); s.clockSeen = true; s.clockMissLogged = false; s.lastClockT = r.t;
				break;
			default: break;
		}
	}

	void handleLine(S& s, const std::string& line)
	{
		follow::Record r;
		std::string why;
		const follow::Status st = follow::parse(line, r, &why);
		if (st == follow::Status::kUnknown) { return; }
		if (st == follow::Status::kMalformed)
		{
			const std::string verb = line.substr(0, line.find(' '));
			if (s.malformedLogged.insert(verb).second) { logLine("malformed %s record skipped (%s): %.120s", verb.c_str(), why.c_str(), line.c_str()); }
			return;
		}
		handleRecord(s, r);
	}

	void readSocket(S& s)
	{
#ifndef _WIN32
		char buf[65536];
		for (int guard = 0; guard < 8 && s.connected; guard++)
		{
			const ssize_t n = ::recv(s.fd, buf, sizeof buf, 0);
			if (n == 0) { logLine("agent closed the stream"); closeFd(s); s.retryAt = nowSec() + 2.0; return; }
			if (n < 0)
			{
				if (errno == EAGAIN || errno == EWOULDBLOCK) { return; }
				logLine("recv failed (%s)", strerror(errno));
				closeFd(s); s.retryAt = nowSec() + 2.0; return;
			}
			const size_t before = s.reader.dropped();
			std::vector<std::string> lines;
			s.reader.feed(buf, (size_t)n, lines);
			if (s.reader.dropped() != before) { logLine("a line over %zu bytes dropped (%zu so far)", follow::kMaxLine, s.reader.dropped()); }
			for (const std::string& line : lines) { handleLine(s, line); }
		}
#endif
	}

	void easeCamera(S& s, double dt)
	{
		if (!s.haveTarget || !followCamera()) { return; }
		FollowHost& h = s.host;
		if (!s.camPlaced)
		{
			h.camGet(s.cur.yaw, s.cur.pitch, s.cur.dist, s.cur.pivot);
			s.camPlaced = true;
		}
		const double a = 1.0 - std::exp(-dt * 10.0);	// eases in about a third of a second
		Cam& c = s.cur;
		for (int k = 0; k < 3; k++) { c.pivot[k] += (s.target.pivot[k] - c.pivot[k]) * a; }
		double dy = std::fmod(s.target.yaw - c.yaw, 2 * M_PI);
		if (dy > M_PI) { dy -= 2 * M_PI; } else if (dy < -M_PI) { dy += 2 * M_PI; }
		c.yaw += dy * a;
		c.pitch += (s.target.pitch - c.pitch) * a;
		c.dist += (s.target.dist - c.dist) * a;
		h.camSet(c.yaw, c.pitch, c.dist, c.pivot);
		if (s.target.floor != s.appliedFloor) { s.appliedFloor = s.target.floor; h.setFloor(s.target.floor); logLine("floor %d", s.target.floor); }
		if (s.logFrames)
		{
			printf("follow-cam: pivot %.3f %.3f %.3f yaw %.3f pitch %.3f dist %.2f | target %.3f %.3f %.3f yaw %.3f pitch %.3f dist %.2f\n", c.pivot[0], c.pivot[1], c.pivot[2], c.yaw * 180 / M_PI, c.pitch * 180 / M_PI, c.dist,
				s.target.pivot[0], s.target.pivot[1], s.target.pivot[2], s.target.yaw * 180 / M_PI, s.target.pitch * 180 / M_PI, s.target.dist);
			SimView& sim = h.sim();
			std::string sel;
			for (const uint64_t id : sim.selected())
			{
				double at[3] = {};
				float hull[3];
				const bool ok = sim.placed(id, at, hull);
				char b[160];
				snprintf(b, sizeof b, " %llu@%.2f,%.2f,%.2f%s", (unsigned long long)id, at[0], at[1], at[2], ok ? "" : "?");
				sel += b;
			}
			printf("follow-sim: wall %.3f tick %llu paused %d speed %.2f selected%s\n", nowSec(), (unsigned long long)sim.clock().tick, sim.clock().pauseBits & 1, sim.clock().speed, sel.empty() ? " none" : sel.c_str());
		}
	}

	bool simReady(S& s)
	{
		SimView& sim = s.host.sim();
		if (!simUp(sim) || sim.decoder().lastCompleteTick == 0 || !sim.clock().valid) { return false; }
		if (!s.importSave.empty() && sim.decoder().cameraStartSerial == 0) { return false; }
		return true;
	}
}

// ---- the hooks ------------------------------------------------------------------------------------------------------------------------------

bool followArg(int argc, char** argv, int& i)
{
	S& s = st();
	const std::string a = argv[i];
	const bool more = i + 1 < argc;
	if (a == "--follow-stream" && more) { s.address = argv[++i]; s.enabled = true; return true; }
	if (a == "--import-save" && more) { s.importSave = argv[++i]; s.importGiven = true; return true; }
	if (a == "--import-report" && more) { s.importReport = argv[++i]; s.reportGiven = true; return true; }
	return false;
}

void followValidate(bool debug)
{
	S& s = st();
	if (!s.enabled && !s.importGiven && !s.reportGiven) { return; }
	if (!debug) { fprintf(stderr, "kclient: --follow-stream, --import-save and --import-report need --debug (dev tools, kenshi-cym.40.2.5)\n"); exit(2); }
	if (s.reportGiven && !s.importGiven) { fprintf(stderr, "kclient: --import-report needs --import-save\n"); exit(2); }
	if (s.enabled)
	{
		const size_t c = s.address.rfind(':');
		char* end = nullptr;
		const long p = c == std::string::npos ? 0 : strtol(s.address.c_str() + c + 1, &end, 10);
		if (c == std::string::npos || s.address.substr(0, c) != "127.0.0.1" || *end || p < 1 || p > 65535)
		{
			fprintf(stderr, "kclient: --follow-stream wants 127.0.0.1:PORT, not '%s'\n", s.address.c_str());
			exit(2);
		}
		s.port = (int)p;
	}
	s.logFrames = std::getenv("KCLIENT_FOLLOW_LOG") != nullptr;
}

bool followEnabled() { return st().enabled; }

void followServerArgs(std::vector<std::string>& args)
{
	const S& s = st();
	if (s.importGiven) { args.push_back("--import-save"); args.push_back(s.importSave); }
	if (s.reportGiven) { args.push_back("--import-report"); args.push_back(s.importReport); }
	if (s.enabled) { args.push_back("--paused"); }
}

void followInit(const FollowHost& host)
{
	S& s = st();
	s.host = host;
	s.inited = true;
}

bool mirrorLocked()
{
	const S& s = st();
	return s.enabled && s.connected && s.readySent && s.lockOn;
}

bool followCamera() { return mirrorLocked(); }

void followLockToggle()
{
	S& s = st();
	s.lockOn = !s.lockOn;
	if (!s.lockOn && s.inited) { s.host.sim().releaseEmpty(); }	// released: the one-of-ours-always-selected rule is back
	if (s.inited && s.host.toast) { s.host.toast(s.lockOn ? "Follow mode: your camera and orders are locked to the original's." : "Follow mode: lock released, you can move the camera and give orders."); }
	logLine("mirror lock %s", s.lockOn ? "on" : "off");
}

bool followLockOn() { return st().lockOn; }
bool followActive() { return st().enabled; }

void followPoll()
{
	S& s = st();
	if (!s.enabled || !s.inited) { return; }
	SimView& sim = s.host.sim();
	const double now = nowSec();
	const double dt = s.lastFrameAt > 0 ? std::min(0.25, now - s.lastFrameAt) : 0.0;
	s.lastFrameAt = now;
#ifndef _WIN32
	if (s.connecting)
	{
		pollfd p{ s.fd, POLLOUT, 0 };
		if (::poll(&p, 1, 0) > 0)
		{
			int err = 0;
			socklen_t len = sizeof err;
			getsockopt(s.fd, SOL_SOCKET, SO_ERROR, &err, &len);
			if (err) { closeFd(s); s.retryAt = now + 2.0; }
			else { s.connecting = false; s.connected = true; }
		}
	}
	else if (!s.connected && now >= s.retryAt) { startConnect(s); }
#endif
	if (s.squadShowId && s.host.showSquadOf && s.host.showSquadOf(s.squadShowId))
	{
		logLine("panel: squad tab of selected %llu shown", (unsigned long long)s.squadShowId);
		s.squadShowId = 0;
	}
	for (size_t i = 0; i < s.pendingOrders.size();)
	{
		const S::PendingOrder& o = s.pendingOrders[i];
		const std::string r = sim.orderResult(o.seq);
		if (!r.empty()) { logLine("order %s clientSeq %u: %s", o.text.c_str(), o.seq, r.rfind("applied", 0) == 0 ? ("acked (" + r + ")").c_str() : r.c_str()); }
		else if (now - o.since > 20.0) { logLine("order %s clientSeq %u: no answer from the sim in 20 s", o.text.c_str(), o.seq); }
		else { i++; continue; }
		s.pendingOrders.erase(s.pendingOrders.begin() + (long)i);
	}
	static bool wasConnected = false;
	if (s.connected && !wasConnected)
	{
		// a new connection: hello at once; after a drop the agent sends the full state again once `ready` goes out
		s.connections++;
		s.readySent = false;
		s.lockToasted = false;
		s.malformedLogged.clear();
		follow::origStore().clear();
		drift::reset();
		s.clockSeen = false;
		sendLine(s, "hello v=1 client=" + follow::quote(s.host.clientHash.empty() ? "unknown" : s.host.clientHash));
		logLine("connected to 127.0.0.1:%d (connection %d), hello sent", s.port, s.connections);
	}
	if (!s.connected && wasConnected) { logLine("stream dropped: reconnecting every 2 s"); s.readySent = false; }
	wasConnected = s.connected;

	// the imported world: the title goes once the server has the world in
	if (!s.beginDone && simReady(s)) { s.beginDone = true; s.host.beginSession(); logLine("imported world is in"); }

	if (s.connected)
	{
		// ready = on the imported world AND paused (s3). On a reconnect the sim may be running: pause it first (order 40), send ready once it shows paused
		// (kenshi-cym.40.2.9; the agent's speed record after ready unpauses it again).
		if (!s.readySent && s.beginDone && s.host.worldOn() && s.everReady && !(sim.clock().pauseBits & 1))
		{
			if (now - s.reconPauseAt >= 1.0) { s.reconPauseAt = now; sim.pause(true); logLine("reconnect: sim is running, pause sent before ready"); }
		}
		if (!s.readySent && s.beginDone && s.host.worldOn() && (sim.clock().pauseBits & 1))
		{
			s.readySent = s.everReady = true;
			sendLine(s, "ready");
			s.lastClockSent = now;
			logLine("ready sent (sim %s)", (sim.clock().pauseBits & 1) ? "paused" : "running");
			if (!s.lockToasted && s.host.toast) { s.lockToasted = true; s.host.toast("Following the original: your camera and orders are locked (F8 panel: Follow mirror lock)."); }
		}
		readSocket(s);
		trySelect(s);
		if (s.readySent && now - s.lastClockSent >= 1.0)
		{
			s.lastClockSent = now;
			const auto& c = sim.clock();
			char b[64];
			snprintf(b, sizeof b, "clock t=%.6f", ((double)c.day - 1.0) * 24.0 + c.hour);
			sendLine(s, b);
		}
		if (s.readySent && s.clockSeen && now - s.lastClockRecv > 3.0 && !s.clockMissLogged)
		{
			s.clockMissLogged = true;
			logLine("no clock beat from the agent for %.1f s", now - s.lastClockRecv);
		}
		drift::frame(sim, s.readySent, now, [&s](const std::string& l) { sendLine(s, l); });
		easeCamera(s, dt);
		flush(s);
	}
}

void followQuit()
{
	S& s = st();
	if (!s.enabled) { return; }
	if (s.connected) { sendLine(s, "bye"); flush(s); logLine("bye sent"); }
	closeFd(s);
}
}
