// kenshi-cym.42: ksim_server --import-save DIR --paused --import-report PATH, through runServer in a thread and a raw SIMLINK client. DIR is the quick-save
// fixture ksim_save_import_test --write-fixture wrote (argv[1]). Asserted: the server starts paused (a ready session gets its first snapshot and no
// second one), a speed order releases it (the tick advances), the report is JSON format 1 whose members are all in the sim (each one's CHAR_APPEARANCE
// arrived), the selected id is a member, and a missing save folder ends the server with exit code 1 and no fallback world.
#include "ksim/net/simlink.hpp"
#include "ksim/server_main.hpp"

#include "net/simlink.hpp" // client/src/net/simlink.hpp
#include "kworld/loadorder.hpp"
#include "test_net.hpp"
#include "test_util.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <set>
#include <sstream>
#include <streambuf>
#include <string>
#include <thread>
#include <vector>

#ifndef KENSHI_DATA
#error KENSHI_DATA must be defined
#endif

namespace
{
using Wall = std::chrono::steady_clock;
namespace fs = std::filesystem;

// The server's `listening 127.0.0.1:PORT` line, read from another thread.
class LineBuf : public std::streambuf
{
public:
	std::string waitFor(const std::string& prefix, int ms)
	{
		std::unique_lock<std::mutex> lock(mutex_);
		cv_.wait_for(lock, std::chrono::milliseconds(ms), [&] { return text_.find(prefix) != std::string::npos && text_.find('\n', text_.find(prefix)) != std::string::npos; });
		const std::size_t at = text_.find(prefix);
		return at == std::string::npos ? "" : text_.substr(at, text_.find('\n', at) - at);
	}

protected:
	int_type overflow(int_type c) override
	{
		if (c != traits_type::eof())
		{
			std::lock_guard<std::mutex> lock(mutex_);
			text_ += static_cast<char>(c);
			cv_.notify_all();
		}
		return c;
	}

private:
	std::mutex mutex_;
	std::condition_variable cv_;
	std::string text_;
};

struct Frames
{
	int fd = -1;
	std::vector<std::uint8_t> raw;
	std::uint64_t snapshots = 0;
	std::uint64_t lastTick = 0;
	std::set<std::uint64_t> appeared; // CHAR_APPEARANCE ids

	void send(const std::vector<std::uint8_t>& bytes)
	{
		test::require(test_net::send(fd, bytes.data(), bytes.size(), test_net::kMsgNoSignal) == static_cast<long>(bytes.size()), "client send");
	}
	void pump(int ms)
	{
		const auto end = Wall::now() + std::chrono::milliseconds(ms);
		while (Wall::now() < end)
		{
			pollfd p{fd, POLLIN, 0};
			if (test_net::poll(&p, 1, 20) <= 0)
				continue;
			std::uint8_t block[65536];
			const auto n = test_net::recv(fd, block, sizeof block, 0);
			test::require(n > 0, "the server closed the connection");
			raw.insert(raw.end(), block, block + n);
			std::size_t at = 0;
			while (raw.size() - at >= 6)
			{
				std::uint32_t len;
				std::uint16_t type;
				std::memcpy(&len, &raw[at], 4);
				if (raw.size() - at - 4 < len)
					break;
				std::memcpy(&type, &raw[at + 4], 2);
				const std::uint8_t* payload = &raw[at + 6];
				test::require(type != ksim::simlink::kError, "the server sent an ERROR frame");
				if (type == ksim::simlink::kSnapBegin)
				{
					++snapshots;
					std::memcpy(&lastTick, payload, 8);
				} else if (type == ksim::simlink::kAppearance && len >= 18) {
					std::uint64_t id;
					std::memcpy(&id, payload + 8, 8);
					appeared.insert(id);
				}
				at += 4 + len;
			}
			raw.erase(raw.begin(), raw.begin() + static_cast<std::ptrdiff_t>(at));
		}
	}
};

std::vector<std::uint8_t> speedOrder(float speed)
{
	std::vector<std::uint8_t> payload;
	const std::uint32_t seq = 9001;
	const std::uint16_t kind = 41;
	const auto put = [&](const void* data, std::size_t size) { payload.insert(payload.end(), static_cast<const std::uint8_t*>(data), static_cast<const std::uint8_t*>(data) + size); };
	put(&seq, 4);
	put(&kind, 2);
	put(&speed, 4);
	std::vector<std::uint8_t> out(6);
	const std::uint32_t len = static_cast<std::uint32_t>(payload.size() + 2);
	const std::uint16_t type = ksim::simlink::kOrder;
	std::memcpy(out.data(), &len, 4);
	std::memcpy(out.data() + 4, &type, 2);
	out.insert(out.end(), payload.begin(), payload.end());
	return out;
}

} // namespace

int main(int argc, char** argv)
{
	test::require(argc == 2, "usage: ksim_server_import FIXTURE_DIR");
	try
	{
		const std::string save = argv[1];
		const fs::path report = fs::path(save) / "import_report.json";
		fs::remove(report);
		const kworld::LoadOrder order = kworld::resolveLoadOrder(KENSHI_DATA, "");

		// A missing save: exit code 1 with the importer's message, never a new game.
		{
			std::ostringstream log;
			ksim::ServerConfig bad;
			bad.listen = "127.0.0.1:0";
			bad.log = &log;
			bad.navCache = false;
			bad.importSave = save + "/missing";
			std::atomic<bool> stop{false};
			test::require(ksim::runServer(bad, stop) == 1 && log.str().find("missing") != std::string::npos, "a missing save ends the server with code 1 and names the path: " + log.str());
		}

		LineBuf lines;
		std::ostream out(&lines);
		std::ostringstream log;
		ksim::ServerConfig config;
		config.listen = "127.0.0.1:0";
		config.out = &out;
		config.log = &log;
		config.navCache = false;
		config.importSave = save;
		config.importReport = report.string();
		config.paused = true;
		std::atomic<bool> stop{false};
		int code = -1;
		std::thread thread([&] { code = ksim::runServer(config, stop); });
		const struct Joiner
		{
			std::atomic<bool>& stop;
			std::thread& thread;
			~Joiner()
			{
				stop.store(true);
				if (thread.joinable())
					thread.join();
			}
		} joiner{stop, thread};

		const std::string listening = lines.waitFor("listening 127.0.0.1:", 240000);
		test::require(!listening.empty(), "the server never listened: " + log.str());
		const int port = std::stoi(listening.substr(listening.rfind(':') + 1));

		Frames client;
		client.fd = ::socket(AF_INET, SOCK_STREAM, 0);
		sockaddr_in address{};
		address.sin_family = AF_INET;
		address.sin_port = htons(static_cast<std::uint16_t>(port));
		::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
		test::require(::connect(client.fd, reinterpret_cast<sockaddr*>(&address), sizeof address) == 0, "connect");
		std::vector<std::uint8_t> hello;
		::simlink::OrderWriter::hello(hello, ksim::simlink::dataHash(order), "ksim_server_import");
		client.send(hello);
		client.pump(3000);
		test::require(client.snapshots >= 1, "no initial snapshot after HELLO");
		const std::uint64_t held = client.lastTick;
		const std::uint64_t heldSnapshots = client.snapshots;
		client.pump(2000);
		test::require(client.snapshots == heldSnapshots && client.lastTick == held, "the paused server ticked before a speed order");

		client.send(speedOrder(1.0f));
		client.pump(3000);
		test::require(client.lastTick > held && client.snapshots > heldSnapshots, "no tick after the speed order");

		std::ifstream in(report);
		std::stringstream text;
		text << in.rdbuf();
		const std::string json = text.str();
		test::require(json.rfind("{\"format\":1,", 0) == 0 && json.back() == '\n', "the report is format 1 JSON: " + json);
		test::require(!fs::exists(report.string() + ".tmp"), "the report's tmp file was renamed");
		const std::size_t members = json.find("\"members\":[");
		const std::size_t end = json.find("],\"selected\"");
		test::require(members != std::string::npos && end != std::string::npos && end > members + 11, "the report has members");
		std::set<std::uint64_t> ids;
		for (std::size_t at = json.find("\"id\":", members); at != std::string::npos && at < end; at = json.find("\"id\":", at + 5))
			ids.insert(std::stoull(json.substr(at + 5)));
		test::require(ids.size() == 2, "the fixture's two members are in the report");
		const std::string countsKey = "\"counts\":{\"members\":";
		test::require(std::stoul(json.substr(json.find(countsKey) + countsKey.size())) == 2, "counts.members is 2");
		for (const std::uint64_t id : ids)
			test::require(client.appeared.count(id) == 1, "member id " + std::to_string(id) + " exists in the sim (it was sent to the client)");
		const std::string selKey = "\"selected\":";
		test::require(ids.count(std::stoull(json.substr(json.find(selKey) + selKey.size()))) == 1, "the selected id is a member");
		std::cout << "ksim_server_import: paused until the speed order, report ok, " << ids.size() << " members\n";
	}
	catch (const std::exception& error)
	{
		std::cerr << "ksim_server_import FAILED: " << error.what() << "\n";
		return 1;
	}
	return 0;
}
