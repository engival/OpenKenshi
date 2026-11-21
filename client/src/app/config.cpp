#include "app/config.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>

#include "app/userdirs.hpp"

namespace fs = std::filesystem;

namespace cfg
{
	namespace
	{
		// info.value / num = the effective value. user = what the file holds (or will after a save); a command-line override pins the effective value
		// without touching user, so it is never saved and a reload does not undo it.
		struct Entry
		{
			Info info;
			double num = 0;
			std::string user;
			bool pinned = false;
		};

		// KCLIENT_CONFIG: unset = the user's file; a path = that file; "none" = defaults only, nothing read or written (headless.sh, scene tests).
		struct Registry
		{
			std::mutex m;
			std::map<std::string, Entry> opts;
			std::vector<std::string> lines;	// the file as read, rewritten in place on save
			std::map<std::string, std::string> retired;	// keys this build dropped -> the line load() prints when the file still sets one
			fs::file_time_type mtime{};
			std::chrono::steady_clock::time_point lastPoll{}, lastSet{};
			uint64_t gen = 1;
			bool dirty = false, loaded = false;
		};

		Registry& reg()
		{
			static Registry r;
			return r;
		}

		std::string fmtNum(double v)
		{
			char buf[32];
			snprintf(buf, sizeof buf, "%g", v);
			return buf;
		}

		// Parses `text` for e's type into canonical text + number; false if it is not a valid value. Numbers are clamped to the range.
		bool parse(const Entry& e, const std::string& text, std::string& canon, double& num)
		{
			switch (e.info.type)
			{
				case Type::kBool:
					if (text == "on" || text == "1" || text == "true" || text == "yes") { canon = "on"; num = 1; return true; }
					if (text == "off" || text == "0" || text == "false" || text == "no") { canon = "off"; num = 0; return true; }
					return false;
				case Type::kInt:
				case Type::kFloat:
				{
					char* end = nullptr;
					double v = strtod(text.c_str(), &end);
					if (text.empty() || *end || !std::isfinite(v)) { return false; }
					v = std::fmin(std::fmax(v, e.info.min), e.info.max);
					if (e.info.type == Type::kInt) { v = std::round(v); }
					canon = fmtNum(v);
					num = v;
					return true;
				}
				case Type::kText:
					if (!e.info.choices.empty() && std::find(e.info.choices.begin(), e.info.choices.end(), text) == e.info.choices.end()) { return false; }
					canon = text;
					num = 0;
					return true;
			}
			return false;
		}

		bool disabled()
		{
			const char* c = getenv("KCLIENT_CONFIG");
			return c && std::string(c) == "none";
		}

		// Caller holds the lock. key=value split at the first '='; surrounding blanks trimmed. false for comments, blanks and junk.
		bool splitLine(const std::string& line, std::string& key, std::string& value)
		{
			const size_t a = line.find_first_not_of(" \t");
			if (a == std::string::npos || line[a] == '#') { return false; }
			const size_t eq = line.find('=', a);
			if (eq == std::string::npos) { return false; }
			size_t kb = eq;
			while (kb > a && (line[kb - 1] == ' ' || line[kb - 1] == '\t')) { kb--; }
			size_t vb = eq + 1, ve = line.size();
			while (vb < ve && (line[vb] == ' ' || line[vb] == '\t')) { vb++; }
			while (ve > vb && (line[ve - 1] == ' ' || line[ve - 1] == '\t' || line[ve - 1] == '\r')) { ve--; }
			key = line.substr(a, kb - a);
			value = line.substr(vb, ve - vb);
			return !key.empty();
		}

		// Caller holds the lock. Applies the file's lines over the defaults; true if any value changed.
		bool readLocked(Registry& r)
		{
			std::ifstream in(path());
			std::vector<std::string> lines;
			for (std::string line; std::getline(in, line); ) { lines.push_back(line); }
			std::error_code ec;
			r.mtime = fs::last_write_time(path(), ec);
			r.lines = lines;
			bool changed = false;
			std::map<std::string, std::string> seen;
			for (const std::string& line : lines)
			{
				std::string key, value;
				if (!splitLine(line, key, value)) { continue; }
				auto it = r.opts.find(key);
				if (it == r.opts.end()) { continue; }	// another build's option: kept in `lines`, not ours to judge
				std::string canon;
				double num;
				if (!parse(it->second, value, canon, num)) { fprintf(stderr, "kclient: %s: bad value '%s' for %s, keeping %s\n", path().c_str(), value.c_str(), key.c_str(), it->second.info.value.c_str()); continue; }
				seen[key] = canon;
			}
			for (auto& [key, e] : r.opts)	// a key not in the file (any more) is its default
			{
				const auto s = seen.find(key);
				e.user = s == seen.end() ? e.info.def : s->second;
				if (e.pinned || e.user == e.info.value) { continue; }
				parse(e, e.user, e.info.value, e.num);
				changed = true;
			}
			if (changed) { r.gen++; }
			return changed;
		}

		std::string headerOf(const Info& i) { return "# " + i.section + " / " + i.group; }

		// Caller holds the lock. The options in section, group, key order (the order the file and the defaults listing are written in).
		std::vector<const Entry*> ordered(const Registry& r)
		{
			std::vector<const Entry*> v;
			for (const auto& [k, e] : r.opts) { v.push_back(&e); }
			const auto& names = sectionNames();
			auto rank = [&](const Info& i) { return std::find(names.begin(), names.end(), i.section) - names.begin(); };
			std::stable_sort(v.begin(), v.end(), [&](const Entry* a, const Entry* b)
			{
				if (rank(a->info) != rank(b->info)) { return rank(a->info) < rank(b->info); }
				if (a->info.group != b->info.group) { return a->info.group < b->info.group; }
				return a->info.key < b->info.key;
			});
			return v;
		}

		// A "# Section / Group" line some earlier save wrote (or one for a group this build no longer has: the section is a known one).
		bool isHeaderLine(const Registry& r, const std::string& line)
		{
			if (line.rfind("# ", 0) != 0) { return false; }
			const size_t sl = line.find(" / ");
			if (sl == std::string::npos) { return false; }
			const auto& names = sectionNames();
			if (std::find(names.begin(), names.end(), line.substr(2, sl - 2)) == names.end()) { return false; }
			(void)r;
			return true;
		}

		// Caller holds the lock. Rewrites the file: a known key's line takes the user's value, except when that value equals the option's
		// static compiled-in default, when the line is dropped instead (user 2026-11-14, kenshi-k16.2.243). Doing so is the point: the
		// setting then follows a future change of the default. Only static defaults count: a value detected at runtime (the monitor, the
		// window layout's window.<layout>.* entries, screen size, hardware-probed quality) is not an option's default, and a raw line is
		// never dropped for matching one. Keys this build does not know (another build's, retired ones) keep their lines as they are.
		bool saveLocked(Registry& r)
		{
			if (!r.dirty || disabled()) { r.dirty = false; return true; }
			// Grouped layout (kenshi-k16.2.244.1): the leading comments of the file stay on top; then each group that has a line to write, under a
			// "# Section / Group" header (written by an earlier save, so they are dropped and rebuilt here); then everything else (unknown
			// keys, window.<layout>.* lines, later comments) in the order it had, at the end. A known key's line takes the user's value, or is
			// not written when that equals the default.
			std::vector<std::string> preamble, tail;
			bool seenOption = false;
			for (const std::string& line : r.lines)
			{
				std::string key, value;
				if (!splitLine(line, key, value))
				{
					if (isHeaderLine(r, line)) { seenOption = true; continue; }
					(seenOption ? tail : preamble).push_back(line);
					continue;
				}
				if (r.opts.count(key)) { seenOption = true; continue; }	// re-emitted below, grouped
				if (r.retired.count(key)) { seenOption = true; continue; }	// a retired key (cfg::retire) is warned about at load and dropped here (kenshi-jj3.30)
				seenOption = true;
				tail.push_back(line);	// another build's option, or a free-form key (window.<layout>.*)
			}
			auto trim = [](std::vector<std::string>& v)
			{
				while (!v.empty() && v.back().find_first_not_of(" \t\r") == std::string::npos) { v.pop_back(); }
				size_t i = 0;
				while (i < v.size() && v[i].find_first_not_of(" \t\r") == std::string::npos) { i++; }
				v.erase(v.begin(), v.begin() + (long)i);
			};
			trim(preamble);
			trim(tail);
			std::vector<std::string> out = preamble;
			std::string curHeader;
			for (const Entry* e : ordered(r))
			{
				if (e->user == e->info.def) { continue; }	// equal to the default: not saved, so a changed default reaches it
				const std::string h = headerOf(e->info);
				if (h != curHeader) { if (!out.empty()) { out.push_back(""); } out.push_back(h); curHeader = h; }
				out.push_back(e->info.key + "=" + e->user);
			}
			if (!tail.empty()) { if (!out.empty()) { out.push_back(""); } out.insert(out.end(), tail.begin(), tail.end()); }
			std::error_code ec;
			fs::create_directories(fs::path(path()).parent_path(), ec);
			const std::string tmp = path() + ".tmp";
			{
				std::ofstream f(tmp);
				for (const std::string& line : out) { f << line << "\n"; }
				if (!f) { fprintf(stderr, "kclient: cannot write %s\n", tmp.c_str()); return false; }
			}
			fs::rename(tmp, path(), ec);
			if (ec) { fprintf(stderr, "kclient: cannot replace %s: %s\n", path().c_str(), ec.message().c_str()); return false; }
			r.lines = out;
			r.mtime = fs::last_write_time(path(), ec);
			r.dirty = false;
			return true;
		}

		bool setLocked(Registry& r, const std::string& key, const std::string& value, bool fromUser)
		{
			auto it = r.opts.find(key);
			if (it == r.opts.end()) { fprintf(stderr, "kclient: unknown option %s\n", key.c_str()); return false; }
			std::string canon;
			double num;
			if (!parse(it->second, value, canon, num)) { fprintf(stderr, "kclient: bad value '%s' for %s\n", value.c_str(), key.c_str()); return false; }
			Entry& e = it->second;
			if (fromUser)	// the user's choice also ends a command-line pin
			{
				const bool wasPinned = e.pinned;
				e.pinned = false;
				if (canon != e.user || wasPinned)	// ending a pin is a change (the UI refreshes the row) and is saved, even to the value already in effect
				{
					e.user = canon;
					r.dirty = true;
					r.lastSet = std::chrono::steady_clock::now();
					if (wasPinned) { r.gen++; }
				}
			}
			else { e.pinned = true; }
			if (canon == e.info.value) { return true; }
			e.info.value = canon;
			e.num = num;
			r.gen++;
			return true;
		}
	}

	Option::Option(const Group& grp, const char* key, Type t, std::string def, double min, double max, const char* help) : key_(key)
	{
		Registry& r = reg();
		std::lock_guard<std::mutex> g(r.m);
		Entry& e = r.opts[key_];
		e.info = { key_, help, "", def, t, min, max, false, {}, grp.section.empty() ? sectionOfKey(key_) : grp.section, grp.group, grp.needs };
		parse(e, def, e.info.value, e.num);
		e.info.def = e.user = e.info.value;
	}

	std::string Option::text() const
	{
		Registry& r = reg();
		std::lock_guard<std::mutex> g(r.m);
		return r.opts.at(key_).info.value;
	}

	double Option::number() const
	{
		Registry& r = reg();
		std::lock_guard<std::mutex> g(r.m);
		return r.opts.at(key_).num;
	}

	Float::Float(const Group& g, const char* key, double def, double min, double max, const char* help) : Option(g, key, Type::kFloat, fmtNum(def), min, max, help) {}

	Text::Text(const Group& grp, const char* key, const char* def, std::vector<std::string> choices, const char* help) : Option(grp, key, Type::kText, def, 0, 0, help)
	{
		Registry& r = reg();
		std::lock_guard<std::mutex> g(r.m);
		r.opts[key].info.choices = std::move(choices);
	}

	void Bool::set(bool v) const { std::lock_guard<std::mutex> g(reg().m); setLocked(reg(), key(), v ? "on" : "off", true); }
	void Int::set(int v) const { std::lock_guard<std::mutex> g(reg().m); setLocked(reg(), key(), std::to_string(v), true); }
	void Float::set(double v) const { std::lock_guard<std::mutex> g(reg().m); setLocked(reg(), key(), fmtNum(v), true); }
	void Text::set(const std::string& v) const { std::lock_guard<std::mutex> g(reg().m); setLocked(reg(), key(), v, true); }

	std::string path()
	{
		const char* c = getenv("KCLIENT_CONFIG");
		if (c && *c && std::string(c) != "none") { return c; }
		return plat::userConfigDir() + "/kclient/kclient.cfg";	// app/userdirs.hpp: XDG / macOS / %APPDATA%\OpenKenshi (kenshi-379.31.3)
	}

	void retire(const char* key, const char* message)
	{
		Registry& r = reg();
		std::lock_guard<std::mutex> g(r.m);
		r.retired[key] = message;
	}

	void load()
	{
		Registry& r = reg();
		std::lock_guard<std::mutex> g(r.m);
		r.loaded = true;
		if (disabled()) { fprintf(stderr, "kclient: options: defaults only (KCLIENT_CONFIG=none)\n"); return; }
		readLocked(r);
		// A retired key the file still sets: one clear line per key, the key ignored (readLocked keeps another build's keys, and so does save()).
		for (const auto& [key, message] : r.retired)
		{
			for (const std::string& line : r.lines)
			{
				std::string k, v;
				if (splitLine(line, k, v) && k == key) { fprintf(stderr, "kclient: %s: %s\n", path().c_str(), message.c_str()); break; }
			}
		}
	}

	bool save()
	{
		Registry& r = reg();
		std::lock_guard<std::mutex> g(r.m);
		return saveLocked(r);
	}

	bool poll()
	{
		Registry& r = reg();
		std::lock_guard<std::mutex> g(r.m);
		const auto now = std::chrono::steady_clock::now();
		if (!r.loaded || disabled() || now - r.lastPoll < std::chrono::seconds(1)) { return false; }
		r.lastPoll = now;
		if (r.dirty && now - r.lastSet >= std::chrono::seconds(1)) { saveLocked(r); }
		std::error_code ec;
		const fs::file_time_type t = fs::last_write_time(path(), ec);
		if (ec || t == r.mtime || r.dirty) { return false; }	// pending sets win over a concurrent external edit
		return readLocked(r);
	}

	bool set(const std::string& keyEqValue)
	{
		const size_t eq = keyEqValue.find('=');
		if (eq == std::string::npos) { fprintf(stderr, "kclient: expected key=value, got '%s'\n", keyEqValue.c_str()); return false; }
		Registry& r = reg();
		std::lock_guard<std::mutex> g(r.m);
		return setLocked(r, keyEqValue.substr(0, eq), keyEqValue.substr(eq + 1), false);	// a command-line override is not saved
	}

	std::string getRaw(const std::string& key)
	{
		Registry& r = reg();
		std::lock_guard<std::mutex> g(r.m);
		std::string found;
		for (const std::string& line : r.lines)
		{
			std::string k, v;
			if (splitLine(line, k, v) && k == key) { found = v; }	// the last line wins, as for an option
		}
		return found;
	}

	void setRaw(const std::string& key, const std::string& value)
	{
		Registry& r = reg();
		std::lock_guard<std::mutex> g(r.m);
		bool done = false;
		for (std::string& line : r.lines)
		{
			std::string k, v;
			if (!splitLine(line, k, v) || k != key) { continue; }
			if (v != value) { line = key + "=" + value; r.dirty = true; r.lastSet = std::chrono::steady_clock::now(); }
			done = true;
		}
		if (!done) { r.lines.push_back(key + "=" + value); r.dirty = true; r.lastSet = std::chrono::steady_clock::now(); }
	}

	bool setUser(const std::string& key, const std::string& value)
	{
		Registry& r = reg();
		std::lock_guard<std::mutex> g(r.m);
		return setLocked(r, key, value, true);
	}

	uint64_t generation()
	{
		Registry& r = reg();
		std::lock_guard<std::mutex> g(r.m);
		return r.gen;
	}

	const std::vector<std::string>& sectionNames()
	{
		static const std::vector<std::string> n = { "Graphics", "Post-processing", "Animation", "Camera", "Interface", "Keys", "Game", "Audio", "Mods", "Debug" };
		return n;
	}

	std::string sectionOfKey(const std::string& key)
	{
		static const std::map<std::string, std::string> m = {
			{ "render", "Graphics" }, { "gfx", "Graphics" }, { "world", "Graphics" },
			{ "post", "Post-processing" },
			{ "anim", "Animation" }, { "physics", "Animation" },
			{ "camera", "Camera" }, { "look", "Camera" },
			{ "ui", "Interface" }, { "markers", "Interface" }, { "screenshot", "Interface" }, { "window", "Interface" }, { "key", "Keys" },
			{ "combat", "Game" }, { "squad", "Game" }, { "xp", "Game" }, { "ai", "Game" }, { "sim", "Game" }, { "session", "Game" }, { "orders", "Game" },
			{ "audio", "Audio" }, { "drive", "Game" },
			{ "mod", "Mods" },
			{ "debug", "Debug" }, { "demo", "Debug" }, { "client", "Game" },
		};
		const auto it = m.find(key.substr(0, key.find('.')));
		return it == m.end() ? "" : it->second;
	}

	std::vector<Info> find(const std::string& name)
	{
		auto lower = [](std::string t) { for (char& c : t) { c = (char)tolower((unsigned char)c); } return t; };
		const std::string n = lower(name);
		Registry& r = reg();
		std::lock_guard<std::mutex> g(r.m);
		auto collect = [&](auto match)
		{
			std::vector<Info> out;
			for (const Entry* e : ordered(r)) { if (match(e->info)) { out.push_back(e->info); out.back().pinned = e->pinned; } }
			return out;
		};
		std::vector<Info> out = collect([&](const Info& i) { return lower(i.section) == n; });
		if (out.empty()) { out = collect([&](const Info& i) { return lower(i.group) == n; }); }
		if (out.empty() && !n.empty()) { out = collect([&](const Info& i) { return i.key.rfind(n, 0) == 0; }); }	// "render." or "render.sun"
		if (out.empty() && !n.empty()) { out = collect([&](const Info& i) { return lower(i.group).rfind(n, 0) == 0; }); }	// "indoor"
		return out;
	}

	std::vector<std::string> problems()
	{
		Registry& r = reg();
		std::lock_guard<std::mutex> g(r.m);
		std::vector<std::string> out;
		const auto& names = sectionNames();
		for (const auto& [k, e] : r.opts)
		{
			if (e.info.group.empty()) { out.push_back(k + ": no group"); }
			if (e.info.section.empty()) { out.push_back(k + ": no section (its prefix is in no section; pass one in its cfg::Group)"); }
			else if (std::find(names.begin(), names.end(), e.info.section) == names.end()) { out.push_back(k + ": unknown section '" + e.info.section + "'"); }
		}
		return out;
	}

	std::string defaultsText()
	{
		Registry& r = reg();
		std::lock_guard<std::mutex> g(r.m);
		std::string out, cur;
		for (const Entry* e : ordered(r))
		{
			const std::string h = headerOf(e->info);
			if (h != cur) { if (!out.empty()) { out += "\n"; } out += h + "\n"; cur = h; }
			out += "# " + e->info.key + ": " + e->info.help + (e->info.needs.empty() ? "" : " [" + e->info.needs + "]") + "\n" + e->info.key + "=" + e->info.def + "\n";
		}
		return out;
	}

	std::vector<Info> list()
	{
		Registry& r = reg();
		std::lock_guard<std::mutex> g(r.m);
		std::vector<Info> out;
		for (const auto& [key, e] : r.opts)
		{
			out.push_back(e.info);
			out.back().pinned = e.pinned;
		}
		return out;
	}
}
