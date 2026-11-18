#include "kworld/loadorder.hpp"

#include "kworld/gamedata.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <system_error>

namespace kworld {

namespace {

namespace fs = std::filesystem;

fs::path installOf(std::string_view dataDir)
{
	fs::path data{ std::string(dataDir) };
	if (!data.has_filename())
		data = data.parent_path(); // trailing slash
	return data.parent_path();
}

std::string modId(const fs::path& folder)
{
	std::error_code ec;
	std::vector<fs::path> infos;
	for (const auto& entry : fs::directory_iterator(folder, ec))
	{
		const std::string name = entry.path().filename().string();
		if (name.size() >= 6 && name[0] == '_' && name.ends_with(".info"))
			infos.push_back(entry.path());
	}
	std::sort(infos.begin(), infos.end());
	for (const fs::path& info : infos)
	{
		std::ifstream in(info);
		std::stringstream text;
		text << in.rdbuf();
		const std::string body = text.str();
		const auto open = body.find("<id>");
		const auto close = body.find("</id>");
		if (open != std::string::npos && close != std::string::npos && close > open)
			return body.substr(open + 4, close - open - 4);
	}
	return {};
}

std::string trimmed(std::string_view text)
{
	const auto first = text.find_first_not_of(" \t\r\n");
	if (first == std::string_view::npos)
		return {};
	return std::string(text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1));
}

} // namespace

std::vector<ModFolder> discoverModFolders(std::string_view dataDir)
{
	const fs::path install = installOf(dataDir);
	const fs::path roots[2] = {
		install / "mods",
		install.parent_path().parent_path() / "workshop" / "content" / "233860",
	};
	std::vector<ModFolder> result;
	std::error_code ec;
	for (const fs::path& root : roots)
	{
		std::vector<fs::path> folders;
		for (const auto& entry : fs::directory_iterator(root, ec))
			if (entry.is_directory(ec))
				folders.push_back(entry.path());
		std::sort(folders.begin(), folders.end());
		for (const fs::path& folder : folders)
			result.push_back({ modId(folder), folder.string() });
	}
	return result;
}

LoadOrder vanillaLoadOrder(std::string_view dataDir)
{
	const fs::path root{ std::string(dataDir) };
	LoadOrder order;
	for (const auto name : kGameDataLoadOrder)
		order.files.push_back({ std::string(name), (root / std::string(name)).string() });
	return order;
}

LoadOrder resolveLoadOrder(std::string_view dataDir, std::string_view modsCfgPath)
{
	if (modsCfgPath.empty())
		return vanillaLoadOrder(dataDir);
	const fs::path root{ std::string(dataDir) };
	std::ifstream cfg{ fs::path(std::string(modsCfgPath)) };
	std::vector<std::string> names;
	for (std::string line; std::getline(cfg, line);)
	{
		std::string name = trimmed(line);
		if (name.empty() || name[0] == '#')
			continue;
		if (!name.ends_with(".mod") && !name.ends_with(".base"))
			name += ".mod";
		names.push_back(std::move(name));
	}
	if (names.empty())
		return vanillaLoadOrder(dataDir);
	if (std::find(names.begin(), names.end(), "gamedata.base") == names.end())
		names.insert(names.begin(), "gamedata.base");

	const std::vector<ModFolder> folders = discoverModFolders(dataDir);
	LoadOrder order;
	std::error_code ec;
	for (const std::string& name : names)
	{
		// A name is a file name, never a path.
		const bool plain = name.find_first_of("/\\") == std::string::npos && name != "..";
		fs::path found;
		if (plain && fs::is_regular_file(root / name, ec))
			found = root / name;
		for (std::size_t i = 0; plain && found.empty() && i < folders.size(); ++i)
			if (fs::is_regular_file(fs::path(folders[i].path) / name, ec))
				found = fs::path(folders[i].path) / name;
		if (found.empty())
			order.missing.push_back(name);
		else
			order.files.push_back({ name, found.string() });
	}
	return order;
}

std::uint64_t dataHash(const LoadOrder& order)
{
	constexpr std::uint64_t kFnvOffset = 14695981039346656037ull;
	constexpr std::uint64_t kFnvPrime = 1099511628211ull;
	std::uint64_t hash = kFnvOffset;
	const auto mix = [&hash](const void* data, std::size_t size)
	{
		const auto* bytes = static_cast<const std::uint8_t*>(data);
		for (std::size_t i = 0; i < size; ++i)
		{
			hash ^= bytes[i];
			hash *= kFnvPrime;
		}
	};
	for (const LoadOrderEntry& entry : order.files)
	{
		std::ifstream in(entry.path, std::ios::binary);
		if (!in)
			throw std::runtime_error("dataHash: cannot read " + entry.path);
		std::uint64_t content = kFnvOffset;
		std::uint64_t size = 0;
		std::vector<char> block(1 << 20);
		while (in)
		{
			in.read(block.data(), static_cast<std::streamsize>(block.size()));
			const std::size_t got = static_cast<std::size_t>(in.gcount());
			for (std::size_t i = 0; i < got; ++i)
			{
				content ^= static_cast<std::uint8_t>(block[i]);
				content *= kFnvPrime;
			}
			size += got;
		}
		mix(entry.name.data(), entry.name.size());
		const std::uint8_t zero = 0;
		mix(&zero, 1);
		mix(&size, sizeof(size)); // little-endian host (x86-64)
		mix(&content, sizeof(content));
	}
	return hash;
}

} // namespace kworld
