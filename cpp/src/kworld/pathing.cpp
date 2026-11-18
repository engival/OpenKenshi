#include "kworld/pathing.hpp"

#include "kworld/error.hpp"
#include "io.hpp"
#include "le.hpp"

#include <cmath>
#include <map>
#include <set>
#include <string>
#include <utility>

namespace kworld {

namespace {

constexpr double kBlockOrigin = -147456.0;
constexpr double kBlockSize = 18432.0;

class Reader
{
public:
	explicit Reader(std::span<const std::byte> bytes) : bytes_(bytes) {}

	std::size_t remaining() const noexcept { return bytes_.size() - position_; }
	std::size_t position() const noexcept { return position_; }

	void need(std::size_t count, const char* what) const
	{
		if (count > remaining())
			throw Error(std::string("globalPathing: truncated ") + what + " at offset "
				+ std::to_string(position_));
	}

	std::uint32_t u32()
	{
		need(4, "u32");
		const std::uint32_t value = detail::readU32(bytes_.data() + position_);
		position_ += 4;
		return value;
	}

	float f32()
	{
		need(4, "f32");
		const float value = detail::readF32(bytes_.data() + position_);
		position_ += 4;
		return value;
	}

private:
	std::span<const std::byte> bytes_;
	std::size_t position_ = 0;
};

std::set<std::pair<std::uint32_t, std::uint32_t>> cellsOf(const PathNode& node)
{
	return { { node.cellX, node.cellZ }, { node.neighbourX, node.neighbourZ } };
}

} // namespace

GlobalPathing GlobalPathing::parse(std::span<const std::byte> bytes)
{
	Reader reader(bytes);
	GlobalPathing pathing;
	pathing.byteSize = bytes.size();
	pathing.version = reader.u32();
	pathing.count = reader.u32();
	pathing.count2 = reader.u32();

	// Every node needs at least its 40 fixed bytes; this also stops a corrupt
	// count from attempting an absurd allocation.
	if (static_cast<std::size_t>(pathing.count) > reader.remaining() / 40)
		throw Error("globalPathing: node count " + std::to_string(pathing.count)
			+ " does not fit in " + std::to_string(bytes.size()) + " bytes");

	pathing.nodes.reserve(pathing.count);
	for (std::uint32_t i = 0; i < pathing.count; ++i)
	{
		reader.need(40, "node");
		PathNode node;
		node.id = reader.u32();
		node.cellX = reader.u32();
		node.cellZ = reader.u32();
		node.neighbourX = reader.u32();
		node.neighbourZ = reader.u32();
		node.minX = reader.f32();
		node.minZ = reader.f32();
		node.maxX = reader.f32();
		node.maxZ = reader.f32();
		const std::uint32_t edgeCount = reader.u32();
		if (static_cast<std::size_t>(edgeCount) > reader.remaining() / 12)
			throw Error("globalPathing: edge count of node "
				+ std::to_string(node.id) + " does not fit in the file");
		node.edges.reserve(edgeCount);
		for (std::uint32_t e = 0; e < edgeCount; ++e)
			node.edges.push_back({ reader.u32(), reader.f32(), reader.u32() });
		pathing.nodes.push_back(std::move(node));
	}

	reader.need(16, "trailer");
	for (auto& value : pathing.trailer)
		value = reader.u32();
	if (reader.remaining() != 0)
		throw Error("globalPathing: " + std::to_string(reader.remaining())
			+ " trailing bytes");
	return pathing;
}

GlobalPathing GlobalPathing::load(const std::string& path)
{
	auto bytes = detail::readFile(path);
	return parse(bytes);
}

std::size_t GlobalPathing::directedEdgeCount() const noexcept
{
	std::size_t count = 0;
	for (const auto& node : nodes)
		count += node.edges.size();
	return count;
}

std::vector<std::string> GlobalPathing::validate() const
{
	std::vector<std::string> errors;
	const auto need = [&errors](bool condition, std::string message) {
		if (!condition)
			errors.push_back(std::move(message));
	};

	bool idsSequential = nodes.size() == count;
	for (std::size_t i = 0; i < nodes.size() && idsSequential; ++i)
		idsSequential = nodes[i].id == i + 1;
	need(idsSequential, "ids not 1..N in order");
	need(count2 == count, "count2 does not equal count");

	for (const auto& node : nodes)
	{
		const double x0 = node.minX;
		const double z0 = node.minZ;
		const double x1 = node.maxX;
		const double z1 = node.maxZ;
		const double columnMin = kBlockOrigin + node.cellX * kBlockSize;
		const double columnMax = kBlockOrigin + (node.cellX + 1) * kBlockSize;
		const double rowMin = kBlockOrigin + node.cellZ * kBlockSize;
		const double rowMax = kBlockOrigin + (node.cellZ + 1) * kBlockSize;

		bool okay = false;
		if (z1 - z0 == 1.0)
		{
			okay = node.neighbourX == node.cellX
				&& node.neighbourZ == node.cellZ + 1
				&& z0 == kBlockOrigin + (node.cellZ + 1) * kBlockSize
				&& columnMin <= x0 && x0 < x1 && x1 <= columnMax
				&& std::fmod(x0, 72.0) == 0.0 && std::fmod(x1, 72.0) == 0.0;
		}
		else if (x1 - x0 == 1.0)
		{
			okay = node.neighbourX == node.cellX + 1
				&& node.neighbourZ == node.cellZ
				&& x0 == kBlockOrigin + (node.cellX + 1) * kBlockSize
				&& rowMin <= z0 && z0 < z1 && z1 <= rowMax
				&& std::fmod(z0, 72.0) == 0.0 && std::fmod(z1, 72.0) == 0.0;
		}
		need(okay, "node " + std::to_string(node.id) + " geometry");
	}

	std::map<std::pair<std::uint32_t, std::uint32_t>,
		std::pair<float, std::uint32_t>> edges;
	std::size_t edgeEntries = 0;
	for (const auto& node : nodes)
		for (const auto& edge : node.edges)
		{
			++edgeEntries;
			const auto inserted = edges.emplace(
				std::make_pair(node.id, edge.target),
				std::make_pair(edge.cost, edge.edgeClass));
			need(inserted.second, "duplicate edge target from node "
				+ std::to_string(node.id));
		}
	need(edges.size() == edgeEntries, "duplicate edge targets");
	for (const auto& entry : edges)
	{
		const auto reverse = edges.find({ entry.first.second, entry.first.first });
		need(reverse != edges.end() && reverse->second == entry.second,
			"edges not symmetric at " + std::to_string(entry.first.first) + "->"
				+ std::to_string(entry.first.second));
	}

	std::map<std::uint32_t, const PathNode*> byId;
	for (const auto& node : nodes)
		byId[node.id] = &node;
	bool sharedCell = true;
	for (const auto& entry : edges)
	{
		const auto source = byId.find(entry.first.first);
		const auto target = byId.find(entry.first.second);
		if (source == byId.end() || target == byId.end())
		{
			sharedCell = false;
			continue;
		}
		const auto sourceCells = cellsOf(*source->second);
		const auto targetCells = cellsOf(*target->second);
		std::size_t shared = 0;
		for (const auto& cell : sourceCells)
			if (targetCells.count(cell) != 0)
				++shared;
		if (shared != 1)
			sharedCell = false;
	}
	need(sharedCell, "edge endpoints do not share exactly one cell");
	return errors;
}

} // namespace kworld
