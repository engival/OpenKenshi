#include "resources.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

std::string lower(std::string s)
{
	for (char& c : s) { c = (char)tolower((unsigned char)c); }
	return s;
}

bool ResourceIndex::init(const std::string& installRoot)
{
	root_ = installRoot;
	std::ifstream in(installRoot + "/resources.cfg");
	if (!in) { return false; }
	std::string line;
	while (std::getline(in, line))
	{
		while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) { line.pop_back(); }
		if (line.rfind("FileSystem=", 0) != 0) { continue; }
		std::string dir = line.substr(11);
		std::replace(dir.begin(), dir.end(), '\\', '/');
		if (dir.rfind("./", 0) == 0) { dir.erase(0, 2); }
		std::error_code ec;
		fs::directory_iterator it(installRoot + "/" + dir, ec);
		if (ec) { continue; }
		dirs_++;
		for (const fs::directory_entry& e : it)
		{
			std::error_code ec2;
			if (!e.is_regular_file(ec2)) { continue; }
			if (files_.emplace(lower(e.path().filename().string()), e.path().string()).second) { order_.push_back(e.path().string()); }
		}
	}
	return true;
}

std::string ResourceIndex::find(const std::string& name) const
{
	std::string n = lower(name);
	std::replace(n.begin(), n.end(), '\\', '/');
	auto it = files_.find(n.substr(n.rfind('/') == std::string::npos ? 0 : n.rfind('/') + 1));
	if (it == files_.end()) { return ""; }
	return it->second;
}

std::vector<std::string> ResourceIndex::withExtension(const std::string& ext) const
{
	std::vector<std::string> out;
	for (const std::string& p : order_)
	{
		const std::string name = lower(fs::path(p).filename().string());
		if (name.size() > ext.size() && name.compare(name.size() - ext.size(), ext.size(), ext) == 0) { out.push_back(p); }
	}
	return out;
}

std::string ResourceIndex::locate(const std::string& path) const
{
	std::string p = path;
	std::replace(p.begin(), p.end(), '\\', '/');
	while (p.rfind("./", 0) == 0) { p.erase(0, 2); }
	std::error_code ec;
	for (const std::string& base : { std::string(), root_ + "/", root_ + "/data/" })
	{
		const std::string full = base.empty() ? p : base + p;
		if (fs::is_regular_file(full, ec)) { return full; }
	}
	return find(p);
}
