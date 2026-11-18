// Record::find*/..Or typed lookups: last match wins, sections are separate,
// missing keys fall back, floats decode little-endian.

#include "kdata/format.hpp"
#include "test_util.hpp"

#include <array>
#include <bit>
#include <cstdint>
#include <iostream>

namespace {

std::array<std::byte, 4> floatBytes(float value)
{
	const auto bits = std::bit_cast<std::uint32_t>(value);
	return {
		static_cast<std::byte>(bits & 0xff),
		static_cast<std::byte>((bits >> 8) & 0xff),
		static_cast<std::byte>((bits >> 16) & 0xff),
		static_cast<std::byte>((bits >> 24) & 0xff),
	};
}

} // namespace

int main()
{
	try
	{
		kdata::Record record;
		record.ints = { { "n", 1 }, { "n", 7 } };
		record.bools = { { "b", true }, { "b", false } };
		record.floats = { { "f", floatBytes(1.0f) }, { "f", floatBytes(-2.5f) } };
		record.strings = { { "s", "a" }, { "s", "b" } };
		record.files = { { "mesh", "x.mesh" } };
		record.references = { { "kids", { { "child-1" } } } };

		test::require(record.intOr("n", 0) == 7, "last int wins");
		test::require(record.intOr("zz", 42) == 42, "int fallback");
		test::require(!record.boolOr("b", true), "last bool wins");
		test::require(record.boolOr("zz", true), "bool fallback");
		test::require(record.floatOr("f", 0.0f) == -2.5f, "last float wins");
		test::require(record.floatOr("zz", 3.0f) == 3.0f, "float fallback");
		test::require(record.stringOr("s") == "b", "last string wins");
		test::require(record.stringOr("zz", "dflt") == "dflt", "string fallback");
		test::require(record.fileOr("mesh") == "x.mesh", "file lookup");
		test::require(record.fileOr("zz").empty(), "file fallback");
		// Sections are separate: a float key is not an int.
		test::require(record.intOr("f", -1) == -1, "no cross-section int");
		test::require(record.findFloat("n") == nullptr, "no cross-section float");
		test::require(record.findRefs("kids") != nullptr
			&& record.findRefs("kids")->entries.size() == 1, "refs lookup");
		test::require(record.findRefs("zz") == nullptr, "refs missing");
	}
	catch (const std::exception& error)
	{
		std::cerr << "fields: " << error.what() << "\n";
		return 1;
	}
	std::cout << "fields: ok\n";
	return 0;
}
