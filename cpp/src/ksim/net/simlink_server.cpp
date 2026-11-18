// The TCP half of the SIMLINK server: one single-threaded loop that accepts a client, reads
// order frames, ticks the simulation in real time and writes snapshots. POSIX sockets only.

#include "ksim/net/simlink.hpp"
#include "kutil/crashlog.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <fstream>
#include <memory>
#include <optional>
#include <set>

#ifndef MSG_NOSIGNAL	// macOS: no per-call flag (SO_NOSIGPIPE is set on the socket instead)
#define MSG_NOSIGNAL 0
#endif

namespace ksim::simlink {

namespace {

using Clock = std::chrono::steady_clock;

constexpr std::size_t kMaxOutbound = 64u << 20; // a client that stops reading is dropped
constexpr double kMenuGraceSeconds = 30.0; // DIALOGUE.md 13.3: the session grace (MULTIPLAYER 2c default) before a lone menu ends
constexpr int kFlushGraceMs = 2000; // after ERROR: time the client gets to read it before the close
constexpr double kBehindLimit = 1.0; // wall seconds of lag after which pacing resyncs

[[noreturn]] void fail(const std::string& what)
{
	throw std::runtime_error("simlink: " + what + ": " + std::strerror(errno));
}

struct Client
{
	int fd = -1;
	std::vector<std::uint8_t> in;
	std::vector<std::uint8_t> out;
	std::size_t outSent = 0;
	std::unique_ptr<Session> session;
	bool closeAfterFlush = false;
	bool initialSent = false;
	Clock::time_point lastFrame;    // last complete frame of any type (HELLO or PING included)
	Clock::time_point frameStarted; // when the oldest unfinished frame's first byte arrived
	Clock::time_point rateWindow;
	Clock::time_point closeAt;      // set with closeAfterFlush
};

} // namespace

SimLinkServer::SimLinkServer(Simulation& sim, ServerContext& context, ServerOptions options)
	: sim_(sim)
	, ctx_(context)
	, options_(std::move(options))
{
}

SimLinkServer::~SimLinkServer()
{
	if (listenFd_ >= 0)
		::close(listenFd_);
}

int SimLinkServer::listen()
{
	listenFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
	if (listenFd_ < 0)
		fail("socket");
	const int one = 1;
	::setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	sockaddr_in address{};
	address.sin_family = AF_INET;
	address.sin_port = htons(static_cast<std::uint16_t>(options_.port));
	if (::inet_pton(AF_INET, options_.host.c_str(), &address.sin_addr) != 1)
		throw std::runtime_error("simlink: not an IPv4 address: " + options_.host);
	if (::bind(listenFd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
		fail("bind " + options_.host + ":" + std::to_string(options_.port));
	if (::listen(listenFd_, 1) != 0)
		fail("listen");
	socklen_t length = sizeof(address);
	::getsockname(listenFd_, reinterpret_cast<sockaddr*>(&address), &length);
	const int port = ntohs(address.sin_port);
	if (options_.onListening)
		options_.onListening(port);
	return port;
}

void SimLinkServer::run(const std::atomic<bool>& stop)
{
	if (listenFd_ < 0)
		throw std::logic_error("SimLinkServer::run before listen");
	std::ofstream record;
	if (!options_.recordPath.empty())
	{
		record.open(options_.recordPath, std::ios::binary | std::ios::trunc);
		if (!record)
			throw std::runtime_error("simlink: cannot write " + options_.recordPath);
	}

	Client client;
	const auto drop = [&]()
	{
		if (client.fd >= 0)
			::close(client.fd);
		// A gone client leaves nothing running away: its driven characters stop (SIMLINK.md
		// section 12). One client is served at a time, so every driven character is its.
		if (client.session && sim_.combat() != nullptr)
		{
			for (const CombatArena::Member& m : sim_.combat()->members())
			{
				if (!m.fighter.driving())
					continue;
				Command stopDrive = Command::fighterDrive(state::Id{m.id}, 0, m.fighter.driveHeading, 0, 0);
				stopDrive.tick = sim_.clockTick() + 1;
				sim_.enqueue(stopDrive);
				ctx_.orderLog.push_back(OrderLogEntry{stopDrive.tick, 0, 20, true, stopDrive,
					"drive id=" + std::to_string(m.id) + " drive=0 (client gone)"});
			}
		}
		client = Client{};
	};
	const auto queue = [&](const std::vector<std::uint8_t>& bytes)
	{
		client.out.insert(client.out.end(), bytes.begin(), bytes.end());
		if (record.is_open())
			record.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
	};
	const auto flush = [&]() -> bool
	{
		while (client.outSent < client.out.size())
		{
			const ssize_t n = ::send(client.fd, client.out.data() + client.outSent, client.out.size() - client.outSent,
				MSG_NOSIGNAL | MSG_DONTWAIT);
			if (n > 0)
			{
				client.outSent += static_cast<std::size_t>(n);
				continue;
			}
			if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
				break;
			return false; // reset or broken pipe
		}
		if (client.outSent == client.out.size())
		{
			client.out.clear();
			client.outSent = 0;
		}
		return client.out.size() - client.outSent <= kMaxOutbound;
	};

	// End this client (and only this client): ERROR if we can, then close once it is flushed.
	// Whatever is already in `out` is whole frames, so the ERROR lands on a frame boundary.
	const auto end = [&](const std::string& text, bool sendError)
	{
		if (client.fd < 0 || client.closeAfterFlush)
			return;
		if (sendError && client.session)
		{
			try
			{
				std::vector<std::uint8_t> bytes;
				BinaryWriter writer(bytes);
				client.session->refuse(writer, text);
				queue(bytes);
			} catch (...) {
			}
		}
		client.closeAfterFlush = true;
		client.closeAt = Clock::now() + std::chrono::milliseconds(kFlushGraceMs);
	};
	const auto fault = [&](const char* where)
	{
		std::string what = "unknown exception";
		try
		{
			throw;
		} catch (const std::exception& e) {
			what = e.what();
		} catch (...) {
		}
		std::fprintf(stderr, "simlink: client fault in %s: %s\n", where, what.c_str());
		end(std::string("server fault: ") + where, true);
	};

	// Tick pacing: the next tick is due at `due`; speed changes the interval, never the tick.
	auto due = Clock::now();
	// Pause policy (MULTIPLAYER 6.2, single-server default): the sim idles while no ready session is connected. `idle` is separate from
	// the admin's `paused`, so a joining session never unpauses what an admin paused, and the speed is never touched.
	const auto updateIdle = [&]()
	{
		const bool ready = client.fd >= 0 && client.session && client.session->ready() && !client.closeAfterFlush;
		ctx_.idle = !options_.runWithoutClients && !ready;
	};
	updateIdle();
	// DIALOGUE.md 13.3: a menu with no ready session to answer it ends after the session grace, by a logged command (reason 5 no_client),
	// so no NPC waits for a player who never returns. A reconnect inside the grace gets the menus again (Session::sendTick initial).
	std::optional<Clock::time_point> noClientSince;
	std::set<std::pair<std::uint64_t, std::uint32_t>> graceEnded;
	const auto checkGrace = [&]()
	{
		const bool ready = client.fd >= 0 && client.session && client.session->ready() && !client.closeAfterFlush;
		if (ready)
		{
			noClientSince.reset();
			graceEnded.clear();
			return;
		}
		if (!noClientSince)
			noClientSince = Clock::now();
		if (std::chrono::duration<double>(Clock::now() - *noClientSince).count() < kMenuGraceSeconds)
			return;
		for (const Simulation::ReplyMenuEvent& menu : sim_.openMenus())
		{
			if (!graceEnded.insert({menu.speaker, menu.menuSeq}).second)
				continue;
			Command endIt = Command::endConversation(state::Id{menu.target}, state::Id{menu.speaker}, 5);
			endIt.tick = sim_.clockTick() + 1;
			sim_.enqueue(endIt);
			ctx_.orderLog.push_back(OrderLogEntry{endIt.tick, 0, 89, true, endIt,
				"end_talk id=" + std::to_string(menu.target) + " speaker=" + std::to_string(menu.speaker) + " reason=5 (no client)"});
		}
	};
	while (!stop.load())
	{
		drainCalls();
		updateIdle();
		checkGrace();
		const auto now = Clock::now();
		const double interval = Simulation::kTickSeconds / static_cast<double>(ctx_.speed);
		// KSIM_SNAPSHOT_PER_TICK=1 restores one snapshot per tick at any speed (the before / after profile of SIM_SCHEDULING 5O).
		static const bool perTick = std::getenv("KSIM_SNAPSHOT_PER_TICK") != nullptr;
		// One snapshot per `perSnapshot` ticks (SIMLINK.md "Cadence", SIM_SCHEDULING 5O): floor(speed), min 1, so the client's view
		// (delay 2 x speed ticks) always has two snapshots of history. At 1x that is one per tick. The loop sleeps until the whole
		// batch is due, so the ticks run back to back and one snapshot follows; the batch's events go out in tick order, each
		// stamped with its own tick (stampEvents).
		const int perSnapshot = perTick ? 1 : std::max(1, static_cast<int>(ctx_.speed));
		int timeoutMs = 50;
		if (!ctx_.pausedNow())
		{
			const double wait = std::chrono::duration<double>(due - now).count() + (perSnapshot - 1) * interval;
			timeoutMs = wait <= 0.0 ? 0 : static_cast<int>(std::min(50.0, wait * 1000.0 + 0.999));
		}

		pollfd fds[2];
		fds[0] = {listenFd_, static_cast<short>(client.fd < 0 ? POLLIN : 0), 0};
		fds[1] = {client.fd, static_cast<short>(POLLIN | (client.outSent < client.out.size() ? POLLOUT : 0)), 0};
		const int ready = ::poll(fds, client.fd >= 0 ? 2 : 1, timeoutMs);
		if (ready < 0 && errno != EINTR)
			fail("poll");

		if (client.fd < 0 && (fds[0].revents & POLLIN))
		{
			sockaddr_in peer{};
			socklen_t peerLen = sizeof(peer);
			const int fd = ::accept(listenFd_, reinterpret_cast<sockaddr*>(&peer), &peerLen);
			if (fd >= 0)
			{
				const int one = 1;
				::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
#ifdef SO_NOSIGPIPE	// macOS has no MSG_NOSIGNAL
				::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
				client.fd = fd;
				client.lastFrame = client.rateWindow = Clock::now();
				try
				{
					client.session = std::make_unique<Session>(sim_, ctx_);
					client.session->setOrderRateLimit(options_.orderRate);
					// The admin session (SIMLINK.md SAVES) and the debug view's peer check: for now the loopback client.
					const bool loopback = peerLen >= sizeof(sa_family_t) && peer.sin_family == AF_INET
						&& (ntohl(peer.sin_addr.s_addr) >> 24) == 127;
					client.session->setAdmin(loopback);
					client.session->setDebugPeer(loopback);
					std::vector<std::uint8_t> bytes;
					BinaryWriter writer(bytes);
					client.session->begin(writer);
					queue(bytes);
				} catch (...) {
					fault("accept");
				}
			}
		}

		if (client.fd >= 0 && (fds[1].revents & (POLLIN | POLLHUP | POLLERR)))
		{
			std::uint8_t buffer[65536];
			const ssize_t n = ::recv(client.fd, buffer, sizeof(buffer), MSG_DONTWAIT);
			if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
			{
				drop();
			} else if (n > 0 && !client.closeAfterFlush) {
				if (client.in.empty())
					client.frameStarted = Clock::now();
				client.in.insert(client.in.end(), buffer, buffer + n);
				std::size_t at = 0;
				while (!client.closeAfterFlush && client.in.size() - at >= 6)
				{
					std::uint32_t len;
					std::memcpy(&len, &client.in[at], 4);
					if (len < 2 || len > kMaxClientFrame)
					{
						end("bad frame length " + std::to_string(len) + " (client frames are at most "
							+ std::to_string(kMaxClientFrame) + " bytes)", true);
						break;
					}
					if (client.in.size() - at - 4 < len)
						break;
					std::uint16_t type;
					std::memcpy(&type, &client.in[at + 4], 2);
					const std::uint8_t* payload = &client.in[at + 6];
					const std::size_t size = len - 2;
					at += 4 + len;
					client.lastFrame = client.frameStarted = Clock::now();
					try
					{
						std::vector<std::uint8_t> bytes;
						BinaryWriter writer(bytes);
						const bool wasReady = client.session->ready();
						bool alive = client.session->onFrame(type, payload, size, writer);
						if (alive && !wasReady && client.session->ready())
						{
							// Both hellos are in: the full snapshot follows, and the sim resumes (CLOCK in it says running).
							updateIdle();
							client.session->sendTick(writer, true);
							client.initialSent = true;
						}
						queue(bytes);
						if (!alive)
						{
							client.closeAfterFlush = true;
							client.closeAt = Clock::now() + std::chrono::milliseconds(kFlushGraceMs);
						}
					} catch (...) {
						fault("frame");
					}
				}
				if (client.closeAfterFlush)
					client.in.clear();
				else
					client.in.erase(client.in.begin(), client.in.begin() + static_cast<std::ptrdiff_t>(at));
			}
		}

		// Timeouts. A silent socket must not hold the slot (SIMLINK.md section 11).
		if (client.fd >= 0 && !client.closeAfterFlush)
		{
			const auto t = Clock::now();
			const auto since = [&](Clock::time_point from) { return std::chrono::duration_cast<std::chrono::milliseconds>(t - from).count(); };
			if (!client.session->ready() && since(client.lastFrame) > options_.helloTimeoutMs)
				end("HELLO timeout", true);
			else if (!client.in.empty() && since(client.frameStarted) > options_.frameTimeoutMs)
				end("frame timeout (a started frame did not finish)", true);
			else if (client.session->ready() && since(client.lastFrame) > options_.pingTimeoutMs)
				end("keepalive timeout (no frame; clients send PING every 5 s)", true);
			if (since(client.rateWindow) >= 1000)
			{
				client.rateWindow = t;
				client.session->resetRateWindow();
			}
		}

		// Ticks that are due. Paused: none. A long stall resyncs instead of catching up. The pause is checked again before every tick of
		// the catch-up batch: a reply menu opening in one of them must stop the world at once (DIALOGUE.md 13.2), not after the batch.
		updateIdle();
		if (!ctx_.pausedNow())
		{
			const auto t = Clock::now();
			if (std::chrono::duration<double>(t - due).count() > kBehindLimit)
				due = t;
			const auto sending = [&] { return client.fd >= 0 && client.initialSent && client.session->ready() && !client.closeAfterFlush; };
			int pending = 0;
			const auto sendSnapshot = [&]
			{
				pending = 0;
				if (!sending())
					return;
				try
				{
					std::vector<std::uint8_t> bytes;
					BinaryWriter writer(bytes);
					client.session->sendTick(writer, false);
					queue(bytes);
				} catch (...) {
					fault("snapshot");
				}
			};
			int budget = 8;
			while (!ctx_.pausedNow() && due <= t && budget-- > 0)
			{
				sim_.tick();
				kutil::crashlog_tick(sim_.clockTick()); // a crash report names the tick a replay names (kutil/crashlog.hpp)
				if (ctx_.scenario.active)
					ctx_.scenario.sample(sim_, sim_.clockTick()); // task CE: the playtest run's counters; observation only
				due += std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(interval));
				if (sending())
					client.session->stampEvents();
				if (++pending >= perSnapshot)
					sendSnapshot();
			}
			if (pending > 0)
				sendSnapshot();
		} else {
			due = Clock::now();
			if (client.fd >= 0 && client.initialSent && client.session->ready() && !client.closeAfterFlush)
			{
				try
				{
					std::vector<std::uint8_t> bytes;
					BinaryWriter writer(bytes);
					client.session->sendDebugOnly(writer); // the debug view still answers while paused
					queue(bytes);
				} catch (...) {
					fault("debug");
				}
			}
		}

		if (client.fd >= 0)
		{
			if (!flush())
				drop();
			else if (client.closeAfterFlush && (client.outSent == client.out.size() || Clock::now() > client.closeAt))
				drop();
		}
	}
	drop();
	drainCalls();
}

void SimLinkServer::call(const std::function<void()>& fn)
{
	std::promise<void> done;
	std::future<void> finished = done.get_future();
	{
		const std::lock_guard<std::mutex> lock(callMutex_);
		calls_.emplace_back(&fn, std::move(done));
	}
	finished.get(); // rethrows what fn threw
}

void SimLinkServer::drainCalls()
{
	for (;;)
	{
		std::pair<const std::function<void()>*, std::promise<void>> next;
		{
			const std::lock_guard<std::mutex> lock(callMutex_);
			if (calls_.empty())
				return;
			next = std::move(calls_.front());
			calls_.pop_front();
		}
		try
		{
			(*next.first)();
			next.second.set_value();
		} catch (...) {
			next.second.set_exception(std::current_exception());
		}
	}
}

} // namespace ksim::simlink
