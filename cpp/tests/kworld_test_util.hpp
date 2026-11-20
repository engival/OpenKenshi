#pragma once

#include "test_util.hpp"
#include "ksim/dev/build_paths.hpp"

#include <cstdio>
#include <string>

namespace test {

inline std::string shellQuote(const std::string& value)
{
	std::string out = "'";
	for (char c : value)
	{
		if (c == '\'')
			out += "'\\''";
		else
			out.push_back(c);
	}
	return out + "'";
}

inline std::string runCommand(const std::string& command)
{
	FILE* pipe = popen(command.c_str(), "r");
	if (pipe == nullptr)
		throw Failure("cannot run command: " + command);
	std::string output;
	char buffer[4096];
	while (auto count = std::fread(buffer, 1, sizeof(buffer), pipe))
		output.append(buffer, count);
	const int status = pclose(pipe);
	if (status != 0)
		throw Failure("command failed (status " + std::to_string(status) + "): "
			+ command + "\n" + output);
	return output;
}

inline std::string refTool(const char* name)
{
	return std::string(KWORLD_REF_TOOLS) + "/" + name;
}

inline std::string pythonScript(const char* name)
{
	return std::string(KWORLD_SOURCE_DIR) + "/tests/kworld_py/" + name;
}

inline std::string dataPath(const char* relative)
{
	return std::string(KENSHI_DATA) + "/" + relative;
}

} // namespace test
