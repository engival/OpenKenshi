// globalPathing.path node/edge counts and invariants must match the reference
// global_pathing.py.

#include "kworld/pathing.hpp"
#include "kworld_test_util.hpp"

#include <cstdint>
#include <iostream>
#include <map>
#include <sstream>
#include <string>

namespace {

std::map<std::string, std::string> referenceCounts()
{
	const std::string command = "python3 -I "
		+ test::shellQuote(test::pythonScript("pathing_expected.py")) + " "
		+ test::shellQuote(test::refTool("global_pathing.py")) + " "
		+ test::shellQuote(test::dataPath("globalPathing.path")) + " 2>&1";
	const std::string output = test::runCommand(command);

	std::map<std::string, std::string> values;
	std::istringstream lines(output);
	std::string line;
	while (std::getline(lines, line))
	{
		if (line.empty())
			continue;
		const auto space = line.find(' ');
		const std::string key = line.substr(0, space);
		const std::string value = space == std::string::npos
			? "" : line.substr(space + 1);
		values[key] = value;
	}
	return values;
}

long number(const std::map<std::string, std::string>& values, const char* key)
{
	const auto found = values.find(key);
	if (found == values.end())
		throw test::Failure(std::string("reference output has no ") + key);
	return std::stol(found->second);
}

} // namespace

int main()
{
	try
	{
		const auto pathing = kworld::GlobalPathing::load(
			test::dataPath("globalPathing.path"));
		const auto expected = referenceCounts();

		test::require(number(expected, "size") == static_cast<long>(pathing.byteSize),
			"file size mismatch");
		test::require(number(expected, "version") == pathing.version,
			"version mismatch");
		test::require(number(expected, "count") == pathing.count, "count mismatch");
		test::require(number(expected, "count2") == pathing.count2, "count2 mismatch");
		test::require(number(expected, "nodes") == static_cast<long>(pathing.nodes.size()),
			"node count mismatch");
		test::require(number(expected, "edges")
			== static_cast<long>(pathing.directedEdgeCount()), "directed edge mismatch");
		test::require(number(expected, "errors") == 0,
			"reference validator reported errors: " + expected.at("errors"));

		std::uint32_t trailer[4] = {};
		std::istringstream trailerFields(expected.at("trailer"));
		for (auto& value : trailer)
			trailerFields >> value;
		for (int i = 0; i < 4; ++i)
			test::require(pathing.trailer[static_cast<std::size_t>(i)]
				== trailer[i], "trailer mismatch");

		std::size_t horizontal = 0;
		std::size_t vertical = 0;
		std::map<std::uint32_t, std::size_t> classes;
		for (const auto& node : pathing.nodes)
		{
			if (node.maxZ - node.minZ == 1.0f)
				++horizontal;
			else if (node.maxX - node.minX == 1.0f)
				++vertical;
			for (const auto& edge : node.edges)
				++classes[edge.edgeClass];
		}
		test::require(number(expected, "H") == static_cast<long>(horizontal),
			"horizontal portal count mismatch");
		test::require(number(expected, "V") == static_cast<long>(vertical),
			"vertical portal count mismatch");
		test::require(number(expected, "class20") == static_cast<long>(classes[20]),
			"class 20 edge count mismatch");
		test::require(number(expected, "class27") == static_cast<long>(classes[27]),
			"class 27 edge count mismatch");

		const auto errors = pathing.validate();
		test::require(errors.empty(), "validate() reported " + std::to_string(errors.size())
			+ " errors, first: " + (errors.empty() ? std::string() : errors.front()));

		std::cout << "pathing: " << pathing.nodes.size() << " nodes, "
			<< pathing.directedEdgeCount() << " directed edges (H " << horizontal
			<< ", V " << vertical << ", class20 " << classes[20] << ", class27 "
			<< classes[27] << "); validate PASS\n";
	}
	catch (const std::exception& error)
	{
		std::cerr << "pathing: " << error.what() << "\n";
		return 1;
	}
	return 0;
}
