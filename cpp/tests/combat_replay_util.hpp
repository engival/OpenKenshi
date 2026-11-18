#pragma once

// Helpers for the combat replay tests: a minimal JSON reader, the kinspect fight logs
// (docs/reference/combat/fights/COMBATLOG.md; read through `xz -dc`) and fighters built from
// their join records.

#include "ksim/combat.hpp"

#include "kdata/gamedata.hpp"

#include "test_util.hpp"

#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace replay {

struct Json;
using Object = std::map<std::string, Json>;
using Array = std::vector<Json>;

struct Json
{
	std::variant<std::nullptr_t, bool, double, std::string, Array, Object> v;

	bool isObject() const { return std::holds_alternative<Object>(v); }
	bool isArray() const { return std::holds_alternative<Array>(v); }
	double num(double fallback = 0.0) const
	{
		return std::holds_alternative<double>(v) ? std::get<double>(v) : fallback;
	}
	const std::string& str() const
	{
		static const std::string empty;
		return std::holds_alternative<std::string>(v) ? std::get<std::string>(v) : empty;
	}
	const Array& arr() const
	{
		static const Array empty;
		return std::holds_alternative<Array>(v) ? std::get<Array>(v) : empty;
	}
	const Json& operator[](const std::string& key) const
	{
		static const Json none;
		if (!std::holds_alternative<Object>(v))
			return none;
		const auto& o = std::get<Object>(v);
		auto it = o.find(key);
		return it == o.end() ? none : it->second;
	}
	bool has(const std::string& key) const
	{
		return std::holds_alternative<Object>(v) && std::get<Object>(v).count(key) != 0;
	}
};

class Parser
{
public:
	explicit Parser(const std::string& text) : s_(text) {}
	Json parse()
	{
		Json j = value();
		return j;
	}

private:
	void ws()
	{
		while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\n' || s_[i_] == '\r' || s_[i_] == '\t'))
			++i_;
	}
	Json value()
	{
		ws();
		const char c = s_.at(i_);
		if (c == '{')
		{
			++i_;
			Object o;
			ws();
			if (s_[i_] == '}')
			{
				++i_;
				return Json{o};
			}
			for (;;)
			{
				ws();
				std::string key = string();
				ws();
				++i_; // ':'
				o.emplace(std::move(key), value());
				ws();
				if (s_[i_++] == '}')
					break;
			}
			return Json{o};
		}
		if (c == '[')
		{
			++i_;
			Array a;
			ws();
			if (s_[i_] == ']')
			{
				++i_;
				return Json{a};
			}
			for (;;)
			{
				a.push_back(value());
				ws();
				if (s_[i_++] == ']')
					break;
			}
			return Json{a};
		}
		if (c == '"')
			return Json{string()};
		if (s_.compare(i_, 4, "true") == 0)
		{
			i_ += 4;
			return Json{true};
		}
		if (s_.compare(i_, 5, "false") == 0)
		{
			i_ += 5;
			return Json{false};
		}
		if (s_.compare(i_, 4, "null") == 0)
		{
			i_ += 4;
			return Json{nullptr};
		}
		char* end = nullptr;
		const double d = std::strtod(s_.c_str() + i_, &end);
		i_ = static_cast<std::size_t>(end - s_.c_str());
		return Json{d};
	}
	std::string string()
	{
		std::string out;
		++i_;
		while (s_[i_] != '"')
		{
			if (s_[i_] == '\\')
			{
				++i_;
				out += s_[i_] == 'n' ? '\n' : s_[i_];
			} else {
				out += s_[i_];
			}
			++i_;
		}
		++i_;
		return out;
	}
	const std::string& s_;
	std::size_t i_ = 0;
};

inline std::vector<Json> loadLog(const std::string& path)
{
	const std::string command = "xz -dc '" + path + "'";
	FILE* pipe = popen(command.c_str(), "r");
	test::require(pipe != nullptr, "cannot run xz for " + path);
	std::string line;
	std::vector<Json> out;
	char buffer[1 << 16];
	while (std::fgets(buffer, sizeof buffer, pipe) != nullptr)
	{
		line += buffer;
		if (!line.empty() && line.back() == '\n')
		{
			Parser p(line);
			out.push_back(p.parse());
			line.clear();
		}
	}
	pclose(pipe);
	test::require(!out.empty(), "empty log " + path);
	return out;
}

inline ksim::state::FighterSkills skillsOf(const Json& join)
{
	const Json& s = join["skills"];
	ksim::state::FighterSkills k;
	k.strength = s["strength"].num();
	k.dexterity = s["dexterity"].num();
	k.toughness = s["toughness"].num();
	k.athletics = s["athletics"].num();
	k.dodge = s["dodge"].num();
	k.attack = s["attack"].num();
	k.defence = s["defence"].num();
	k.katana = s["katana"].num();
	k.sabres = s["sabres"].num();
	k.hackers = s["hackers"].num();
	k.blunt = s["blunt"].num();
	k.heavy = s["heavy"].num();
	k.unarmed = s["unarmed"].num();
	k.poles = s["poles"].num();
	return k;
}

constexpr double kRadToDeg = 57.29577951308232;

} // namespace replay
