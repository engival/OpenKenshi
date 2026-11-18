#include "assets/materials.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <vector>

namespace
{

std::string baseName(std::string s)
{
	std::replace(s.begin(), s.end(), '\\', '/');
	return s.substr(s.rfind('/') == std::string::npos ? 0 : s.rfind('/') + 1);
}

// Words of a script line without comments; '{' '}' ':' are words of their own, double quotes group.
std::vector<std::string> words(const std::string& line)
{
	std::vector<std::string> out;
	std::string cur;
	bool quoted = false;
	auto flush = [&]() { if (!cur.empty()) { out.push_back(cur); cur.clear(); } };
	for (size_t i = 0; i < line.size(); i++)
	{
		const char c = line[i];
		if (quoted) { if (c == '"') { quoted = false; flush(); } else { cur += c; } }
		else if (c == '/' && i + 1 < line.size() && line[i + 1] == '/') { break; }
		else if (c == '"') { quoted = true; }
		else if (c == '{' || c == '}' || c == ':') { flush(); out.push_back(std::string(1, c)); }
		else if (isspace((unsigned char)c)) { flush(); }
		else { cur += c; }
	}
	flush();
	return out;
}

}

bool MaterialLib::placeholder(const std::string& texture)
{
	const std::string t = lower(texture);
	return t.empty() || t == "black.dds" || t == "flat.dds" || t == "nothing.dds";
}

size_t MaterialLib::load(const ResourceIndex& res)
{
	struct Token { std::string text; bool first; };	// first = first word on its line
	for (const std::string& path : res.withExtension(".material"))
	{
		std::ifstream in(path);
		if (!in) { continue; }
		files++;
		std::vector<Token> toks;
		for (std::string line; std::getline(in, line);)
		{
			bool first = true;
			for (std::string& w : words(line)) { toks.push_back({ std::move(w), first }); first = false; }
		}
		std::vector<std::string> blocks;	// kinds of the open blocks: material, technique, pass, texture_unit, other
		std::string pending;			// block kind announced by the last header, opened by the next '{'
		Material* mat = nullptr;
		Material skipped;			// a duplicate declaration's block: Ogre keeps the first one, this one is parsed and dropped
		int rank = 0;				// quality of mat->diffuse: 3 alias diffuse, 2 unit named diffuse*, 1 first unit with a texture
		std::string unitName, unitTexture, unitAlias;
		for (size_t i = 0; i < toks.size(); i++)
		{
			const std::string& t = toks[i].text;
			const std::string next = i + 1 < toks.size() ? toks[i + 1].text : "";
			if (t == "{")
			{
				blocks.push_back(pending.empty() ? "other" : pending);
				if (pending == "texture_unit") { unitTexture = unitAlias = ""; }
				pending.clear();
			}
			else if (t == "}")
			{
				if (blocks.empty()) { continue; }
				if (blocks.back() == "texture_unit" && mat && !unitTexture.empty())
				{
					const int r = lower(unitAlias) == "diffuse" ? 3 : lower(unitName).rfind("diffuse", 0) == 0 ? 2 : 1;
					if (r > rank) { mat->diffuse = unitTexture; rank = r; }
				}
				if (blocks.back() == "material") { mat = nullptr; }
				blocks.pop_back();
			}
			else if (!toks[i].first) { continue; }
			else if (blocks.empty() && t == "material" && !next.empty())
			{
				const std::string name = lower(next);
				auto [it, fresh] = mats_.try_emplace(name);	// Ogre: a duplicate name keeps the first definition (diffuse())
				mat = fresh ? &it->second : &skipped;
				*mat = Material();
				rank = 0;
				pending = "material";
				if (i + 3 < toks.size() && toks[i + 2].text == ":") { mat->parent = lower(toks[i + 3].text); }
			}
			else if (!blocks.empty() && blocks.back() == "texture_unit")
			{
				if (t == "texture" && unitTexture.empty()) { unitTexture = baseName(next); }
				else if (t == "texture_alias") { unitAlias = next; }
			}
			else if (!blocks.empty() && t == "texture_unit") { pending = t; unitName = next == "{" ? "" : next; }
			else if (!blocks.empty() && (t == "technique" || t == "pass")) { pending = t; }
		}
	}
	return mats_.size();
}

std::string MaterialLib::diffuse(const std::string& material) const
{
	std::string name = lower(material);
	for (int depth = 0; depth < 16; depth++)
	{
		auto it = mats_.find(name);
		if (it == mats_.end()) { return ""; }
		if (!it->second.diffuse.empty()) { return it->second.diffuse; }
		if (it->second.parent.empty()) { return ""; }
		name = it->second.parent;
	}
	return "";
}
