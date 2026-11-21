#include "dev/followparse.hpp"

#include <cmath>
#include <cstdlib>
#include <cerrno>

namespace follow
{
	const std::string* Line::get(std::string_view key) const
	{
		for (size_t i = kv.size(); i-- > 0;) { if (kv[i].first == key) { return &kv[i].second; } }
		return nullptr;
	}

	bool tokenize(std::string_view s, Line& out)
	{
		out = {};
		if (s.empty() || s.size() >= kMaxLine) { return false; }
		size_t i = 0;
		const auto word = [&](bool quotedOk, std::string& w) -> bool
		{
			w.clear();
			if (quotedOk && i < s.size() && s[i] == '"')
			{
				i++;
				for (;; i++)
				{
					if (i >= s.size()) { return false; }	// unterminated
					if (s[i] == '\\' && i + 1 < s.size() && (s[i + 1] == '\\' || s[i + 1] == '"')) { w += s[++i]; }
					else if (s[i] == '"') { i++; break; }
					else { w += s[i]; }
				}
				return i == s.size() || s[i] == ' ';	// text after the closing quote
			}
			while (i < s.size() && s[i] != ' ') { w += s[i++]; }
			return true;
		};
		std::string verb;
		if (!word(false, verb) || verb.empty() || verb.find('=') != std::string::npos) { return false; }
		out.verb = verb;
		while (i < s.size())
		{
			if (s[i] != ' ') { return false; }
			i++;
			if (i >= s.size() || s[i] == ' ') { return false; }	// a trailing or double space
			std::string key;
			while (i < s.size() && s[i] != ' ' && s[i] != '=') { key += s[i++]; }
			if (key.empty() || i >= s.size() || s[i] != '=') { return false; }
			i++;
			std::string val;
			if (!word(true, val)) { return false; }
			out.kv.emplace_back(std::move(key), std::move(val));
		}
		return true;
	}

	bool parseDouble(std::string_view s, double& out)
	{
		if (s.empty() || s.size() > 64 || s.front() == ' ') { return false; }
		const std::string t(s);
		char* end = nullptr;
		errno = 0;
		const double v = std::strtod(t.c_str(), &end);
		if (end != t.c_str() + t.size() || !std::isfinite(v)) { return false; }
		if (t.size() > 1 && (t[1] == 'x' || t[1] == 'X' || t.find("inf") != std::string::npos || t.find("nan") != std::string::npos)) { return false; }
		out = v;
		return true;
	}

	bool parseId(std::string_view s, uint64_t& out)
	{
		if (s.empty() || s.size() > 20) { return false; }
		uint64_t v = 0;
		for (const char c : s)
		{
			if (c < '0' || c > '9') { return false; }
			if (v > (UINT64_MAX - (uint64_t)(c - '0')) / 10) { return false; }
			v = v * 10 + (uint64_t)(c - '0');
		}
		out = v;
		return true;
	}

	bool parseIds(std::string_view s, std::vector<uint64_t>& out)
	{
		out.clear();
		if (s.empty()) { return true; }
		size_t at = 0;
		for (;;)
		{
			const size_t comma = s.find(',', at);
			uint64_t id;
			if (!parseId(s.substr(at, comma == std::string_view::npos ? std::string_view::npos : comma - at), id)) { return false; }
			out.push_back(id);
			if (comma == std::string_view::npos) { return true; }
			at = comma + 1;
		}
	}

	bool parseVec(std::string_view s, double* out, int n)
	{
		size_t at = 0;
		for (int k = 0; k < n; k++)
		{
			const size_t comma = s.find(',', at);
			if ((comma == std::string_view::npos) != (k == n - 1)) { return false; }
			if (!parseDouble(s.substr(at, comma == std::string_view::npos ? std::string_view::npos : comma - at), out[k])) { return false; }
			at = comma + 1;
		}
		return true;
	}

	std::string quote(std::string_view v)
	{
		if (!v.empty() && v.find_first_of(" \"\\") == std::string_view::npos) { return std::string(v); }
		std::string r = "\"";
		for (const char c : v) { if (c == '"' || c == '\\') { r += '\\'; } r += c; }
		return r + "\"";
	}

	namespace
	{
		bool fail(std::string* why, const char* what) { if (why) { *why = what; } return false; }

		bool num(const Line& l, const char* key, double& out, std::string* why)
		{
			const std::string* v = l.get(key);
			if (!v) { if (why) { *why = std::string("missing ") + key; } return false; }
			if (!parseDouble(*v, out)) { if (why) { *why = std::string("bad ") + key; } return false; }
			return true;
		}

		bool vec(const Line& l, const char* key, double* out, int n, std::string* why)
		{
			const std::string* v = l.get(key);
			if (!v) { if (why) { *why = std::string("missing ") + key; } return false; }
			if (!parseVec(*v, out, n)) { if (why) { *why = std::string("bad ") + key; } return false; }
			return true;
		}

		bool ids(const Line& l, Record& r, bool required, std::string* why)
		{
			const std::string* v = l.get("ids");
			if (!v) { if (required && why) { *why = "missing ids"; } return !required; }
			if (!parseIds(*v, r.ids)) { if (why) { *why = "bad ids"; } return false; }
			return true;
		}

		bool intKey(const Line& l, const char* key, int& out, bool required, std::string* why)
		{
			const std::string* v = l.get(key);
			if (!v) { if (required && why) { *why = std::string("missing ") + key; } return !required; }
			double d;
			if (!parseDouble(*v, d) || d != std::floor(d) || std::fabs(d) > 1e9) { if (why) { *why = std::string("bad ") + key; } return false; }
			out = (int)d;
			return true;
		}
	}

	Status parse(std::string_view line, Record& r, std::string* why)
	{
		r = {};
		Line l;
		if (!tokenize(line, l))
		{
			// the verb decides whether it is ours: a garbled line of an unknown verb is just skipped
			const std::string_view verb = line.substr(0, line.find(' '));
			static const char* const kKnown[] = { "hello", "align", "cam", "speed", "select", "order", "orig", "clock" };
			for (const char* k : kKnown) { if (verb == k) { if (why) { *why = "bad line grammar"; } return Status::kMalformed; } }
			return Status::kUnknown;
		}
		const std::string& v = l.verb;
		bool ok = true;
		if (v == "hello") { r.type = Type::kHello; ok = intKey(l, "v", r.v, true, why); }
		else if (v == "align") { r.type = Type::kAlign; ok = num(l, "t", r.t, why); }
		else if (v == "cam")
		{
			r.type = Type::kCam;
			ok = vec(l, "pos", r.pos, 3, why) && vec(l, "rot", r.rot, 4, why) && num(l, "zoom", r.zoom, why) && intKey(l, "floor", r.floor, true, why);
			if (ok && !(r.zoom > 0)) { ok = fail(why, "zoom must be positive"); }
			if (ok && (r.floor < 0 || r.floor > 3)) { ok = fail(why, "floor out of 0..3"); }
			if (ok && std::fabs(r.rot[0] * r.rot[0] + r.rot[1] * r.rot[1] + r.rot[2] * r.rot[2] + r.rot[3] * r.rot[3] - 1.0) > 0.01) { ok = fail(why, "rot is not a unit quaternion"); }
		}
		else if (v == "speed")
		{
			r.type = Type::kSpeed;
			ok = num(l, "v", r.speed, why);
			if (ok && (r.speed < 0 || r.speed > 64)) { ok = fail(why, "speed out of 0..64"); }
		}
		else if (v == "select") { r.type = Type::kSelect; ok = ids(l, r, true, why); }
		else if (v == "order")
		{
			r.type = Type::kOrder;
			const std::string* k = l.get("kind");
			if (!k) { ok = fail(why, "missing kind"); }
			else
			{
				ok = intKey(l, "src", r.src, false, why);
				if (ok)
				{
					if (const std::string* q = l.get("queue")) { r.queue = *q == "1"; }
					if (*k == "move")
					{
						r.kind = Kind::kMove;
						ok = ids(l, r, true, why) && vec(l, "to", r.pos, 3, why);
						if (ok && r.ids.empty()) { ok = fail(why, "empty ids"); }
					}
					else if (*k == "attack" || *k == "interact")
					{
						r.kind = *k == "attack" ? Kind::kAttack : Kind::kInteract;
						ok = ids(l, r, true, why);
						const std::string* t = l.get("target");
						if (ok && (!t || !parseId(*t, r.target) || !r.target)) { ok = fail(why, "bad target"); }
						if (ok && r.ids.empty()) { ok = fail(why, "empty ids"); }
						if (ok && r.kind == Kind::kInteract) { ok = intKey(l, "action", r.action, true, why); }
					}
					else
					{
						r.kind = Kind::kUnmapped;	// "unmapped" itself, and any kind this client does not know: logged, never dropped
						ok = ids(l, r, false, why);
						if (ok)
						{
							const std::string* w = l.get("what");
							r.what = w ? *w : std::string();
							if (*k != "unmapped") { r.what = "kind=" + *k + (r.what.empty() ? "" : " " + r.what); }
						}
					}
				}
			}
		}
		else if (v == "orig")
		{
			r.type = Type::kOrig;
			const std::string* id = l.get("id");
			if (!id || !parseId(*id, r.target) || !r.target) { ok = fail(why, "bad id"); }
			else { ok = vec(l, "pos", r.pos, 3, why) && num(l, "yaw", r.yaw, why) && num(l, "t", r.t, why); }
			if (ok)
			{
				if (const std::string* s = l.get("state"))
				{
					if (*s == "up") { r.state = State::kUp; }
					else if (*s == "down") { r.state = State::kDown; }
					else if (*s == "ko") { r.state = State::kKo; }
					else if (*s == "dead") { r.state = State::kDead; }
					else if (*s == "carried") { r.state = State::kCarried; }
					else { ok = fail(why, "bad state"); }
				}
			}
		}
		else if (v == "clock")
		{
			r.type = Type::kClock;
			ok = num(l, "t", r.t, why);
			if (ok) { if (const std::string* d = l.get("day")) { ok = parseDouble(*d, r.day) || fail(why, "bad day"); } }
			if (ok) { if (const std::string* h = l.get("hours")) { ok = parseDouble(*h, r.hours) || fail(why, "bad hours"); } }
		}
		else { return Status::kUnknown; }
		return ok ? Status::kOk : Status::kMalformed;
	}
}
