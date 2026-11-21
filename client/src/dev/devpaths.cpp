#include "dev/devpaths.hpp"

#include <cstdio>
#include <filesystem>

namespace dev
{
#ifdef KCLIENT_SRC_TREE
	std::string sourceTreeDataDir()
	{
		std::error_code ec;
		const std::filesystem::path src = std::filesystem::path(KCLIENT_SRC_TREE) / "client" / "data";
		return std::filesystem::is_directory(src / "ui", ec) ? src.string() : std::string();
	}

	SharedServer sharedServer()
	{
		SharedServer r;
#ifndef _WIN32
		const std::string tree = KCLIENT_SRC_TREE;
		std::string got;
		if (FILE* f = popen(("'" + tree + "/client/tools/shared_server.sh' 2>/dev/null").c_str(), "r"))
		{
			char buf[1024];
			if (fgets(buf, sizeof buf, f)) { got = buf; }
			pclose(f);
		}
		while (!got.empty() && (got.back() == '\n' || got.back() == '\r')) { got.pop_back(); }
		if (!got.empty()) { r.path = got; r.tree = tree; r.simData = tree + "/cpp/data"; }
#endif
		return r;
	}
#else
	std::string sourceTreeDataDir() { return {}; }
	SharedServer sharedServer() { return {}; }
#endif
}
