#include "kworld/collision.hpp"

#include "kdata/gamedata.hpp"
#include "kworld/buildingparts.hpp"
#include "kworld/error.hpp"
#include "kworld/features.hpp"
#include "kworld/gamedata.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace kworld {

namespace {

constexpr double kPi = 3.14159265358979323846;

// NxMat34: 9 rotation values row-major, then translation; p' = M p + t.
struct Pose
{
	std::array<double, 12> v{ 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0 };

	std::array<double, 3> apply(const std::array<double, 3>& p) const
	{
		return {
			v[0] * p[0] + v[1] * p[1] + v[2] * p[2] + v[9],
			v[3] * p[0] + v[4] * p[1] + v[5] * p[2] + v[10],
			v[6] * p[0] + v[7] * p[1] + v[8] * p[2] + v[11],
		};
	}
};

// a * b: apply b first.
Pose compose(const Pose& a, const Pose& b)
{
	Pose out;
	for (int i = 0; i < 3; ++i)
	{
		for (int j = 0; j < 3; ++j)
		{
			double sum = 0.0;
			for (int k = 0; k < 3; ++k)
				sum += a.v[3 * i + k] * b.v[3 * k + j];
			out.v[3 * i + j] = sum;
		}
		out.v[9 + i] = a.v[3 * i] * b.v[9] + a.v[3 * i + 1] * b.v[10]
			+ a.v[3 * i + 2] * b.v[11] + a.v[9 + i];
	}
	return out;
}

Pose parsePose(std::string_view text)
{
	Pose pose;
	std::istringstream in{ std::string(text) };
	for (double& value : pose.v)
		if (!(in >> value))
			throw Error("collision pose with fewer than 12 values");
	return pose;
}

// Triangle soup of one shape before the pose: positions and indices.
struct Soup
{
	std::vector<std::array<double, 3>> points;
	std::vector<std::uint32_t> indices;
};

template <class T>
T readLE(const std::string& bytes, std::size_t offset)
{
	if (offset + sizeof(T) > bytes.size())
		throw Error("cooked collision stream is truncated");
	T value;
	std::memcpy(&value, bytes.data() + offset, sizeof(T));
	return value;
}

Soup readVertices(const std::string& bytes, std::size_t offset, std::uint32_t count)
{
	Soup soup;
	soup.points.reserve(count);
	for (std::uint32_t i = 0; i < count; ++i)
		soup.points.push_back({ readLE<float>(bytes, offset + 12ull * i),
			readLE<float>(bytes, offset + 12ull * i + 4),
			readLE<float>(bytes, offset + 12ull * i + 8) });
	return soup;
}

void checkIndices(const Soup& soup)
{
	for (const std::uint32_t index : soup.indices)
		if (index >= soup.points.size())
			throw Error("cooked collision index out of range");
}

// 'NXS\x01MESH': header u32s at 8..35 (version, flags, 0.001, 255, 0, Nv,
// Nt), Nv*3 f32, then Nt*3 u8 indices (u16 when flags & 0x10).
Soup decodeTriangleMesh(const std::string& bytes)
{
	const auto flags = readLE<std::uint32_t>(bytes, 12);
	const auto vertexCount = readLE<std::uint32_t>(bytes, 28);
	const auto triangleCount = readLE<std::uint32_t>(bytes, 32);
	Soup soup = readVertices(bytes, 36, vertexCount);
	std::size_t offset = 36 + 12ull * vertexCount;
	const bool wide = (flags & 0x10) != 0;
	for (std::uint32_t i = 0; i < 3ull * triangleCount; ++i)
		soup.indices.push_back(wide ? readLE<std::uint16_t>(bytes, offset + 2ull * i)
			: readLE<std::uint8_t>(bytes, offset + i));
	checkIndices(soup);
	return soup;
}

// 'NXS\x01CVXM': the 'ICE\x01CVHL' chunk holds 7 u32 (5, Nv, Nt, ...),
// Nv*3 f32, one u32, then Nt*3 u8 indices.
Soup decodeConvexMesh(const std::string& bytes)
{
	const std::size_t tag = bytes.find("ICE\x01" "CVHL", 8);
	if (tag == std::string::npos)
		throw Error("convex mesh without a CVHL chunk");
	const auto vertexCount = readLE<std::uint32_t>(bytes, tag + 12);
	const auto triangleCount = readLE<std::uint32_t>(bytes, tag + 16);
	Soup soup = readVertices(bytes, tag + 36, vertexCount);
	const std::size_t offset = tag + 36 + 12ull * vertexCount + 4;
	for (std::uint32_t i = 0; i < 3ull * triangleCount; ++i)
		soup.indices.push_back(readLE<std::uint8_t>(bytes, offset + i));
	if (soup.indices.empty())
		throw Error("convex mesh without triangles");
	checkIndices(soup);
	return soup;
}

Soup decodeCooked(const std::string& bytes)
{
	if (bytes.compare(0, 8, "NXS\x01" "MESH") == 0)
		return decodeTriangleMesh(bytes);
	if (bytes.compare(0, 8, "NXS\x01" "CVXM") == 0)
		return decodeConvexMesh(bytes);
	throw Error("unknown cooked collision stream");
}

Soup boxSoup(double hx, double hy, double hz)
{
	Soup soup;
	soup.points = { { -hx, -hy, -hz }, { hx, -hy, -hz }, { hx, hy, -hz }, { -hx, hy, -hz },
		{ -hx, -hy, hz }, { hx, -hy, hz }, { hx, hy, hz }, { -hx, hy, hz } };
	soup.indices = { 0, 2, 1, 0, 3, 2, 4, 5, 6, 4, 6, 7, 0, 1, 5, 0, 5, 4,
		1, 2, 6, 1, 6, 5, 2, 3, 7, 2, 7, 6, 3, 0, 4, 3, 4, 7 };
	return soup;
}

// Along local Y; `height` is the distance between the hemisphere centres
// (COLLISION_FORMAT.md section 1.1: axis and origin are a hypothesis).
Soup capsuleSoup(double radius, double height)
{
	constexpr int kSegments = 12;
	constexpr int kRings = 3;
	std::vector<std::pair<double, double>> rows; // y, ring radius
	for (int i = kRings; i >= 0; --i)
	{
		const double angle = kPi / 2 * i / kRings;
		rows.emplace_back(-height / 2 - radius * std::sin(angle), radius * std::cos(angle));
	}
	for (int i = 0; i <= kRings; ++i)
	{
		const double angle = kPi / 2 * i / kRings;
		rows.emplace_back(height / 2 + radius * std::sin(angle), radius * std::cos(angle));
	}
	Soup soup;
	for (const auto& [y, ringRadius] : rows)
		for (int k = 0; k < kSegments; ++k)
			soup.points.push_back({ ringRadius * std::cos(2 * kPi * k / kSegments), y,
				ringRadius * std::sin(2 * kPi * k / kSegments) });
	for (std::size_t ring = 0; ring + 1 < rows.size(); ++ring)
	{
		for (int k = 0; k < kSegments; ++k)
		{
			const auto a = static_cast<std::uint32_t>(ring * kSegments);
			const auto b = a + kSegments;
			const auto k2 = static_cast<std::uint32_t>((k + 1) % kSegments);
			const auto k1 = static_cast<std::uint32_t>(k);
			// A pole ring has radius 0: its triangles collapse and are dropped.
			for (const std::array<std::uint32_t, 3>& tri :
				{ std::array<std::uint32_t, 3>{ a + k1, b + k1, b + k2 },
					std::array<std::uint32_t, 3>{ a + k1, b + k2, a + k2 } })
			{
				const auto same = [&](int i, int j) { return soup.points[tri[i]] == soup.points[tri[j]]; };
				if (!same(0, 1) && !same(1, 2) && !same(0, 2))
					soup.indices.insert(soup.indices.end(), tri.begin(), tri.end());
			}
		}
	}
	return soup;
}

// ---- minimal XML scanner: the files are machine-written and regular -------

struct Tag
{
	std::string_view name;
	std::string_view attrs;
	bool close = false;
	bool selfClose = false;
};

class Scanner
{
public:
	explicit Scanner(std::string_view text) : text_(text) {}

	// Next element tag; false at end of input.
	bool next(Tag& tag)
	{
		for (;;)
		{
			const std::size_t open = text_.find('<', pos_);
			if (open == std::string_view::npos)
				return false;
			if (open + 1 >= text_.size())
				return false;
			const char lead = text_[open + 1];
			if (lead == '?' || lead == '!')
			{
				const std::size_t end = text_.find('>', open);
				if (end == std::string_view::npos)
					return false;
				pos_ = end + 1;
				continue;
			}
			std::size_t cursor = open + 1;
			tag = {};
			if (text_[cursor] == '/')
			{
				tag.close = true;
				++cursor;
			}
			const std::size_t nameStart = cursor;
			while (cursor < text_.size() && !std::isspace(static_cast<unsigned char>(text_[cursor]))
				&& text_[cursor] != '>' && text_[cursor] != '/')
				++cursor;
			tag.name = text_.substr(nameStart, cursor - nameStart);
			const std::size_t attrStart = cursor;
			char quote = 0;
			while (cursor < text_.size() && (quote != 0 || text_[cursor] != '>'))
			{
				const char c = text_[cursor];
				if (quote != 0)
					quote = c == quote ? 0 : quote;
				else if (c == '"' || c == '\'')
					quote = c;
				++cursor;
			}
			if (cursor >= text_.size())
				return false;
			tag.attrs = text_.substr(attrStart, cursor - attrStart);
			tag.selfClose = !tag.attrs.empty() && tag.attrs.back() == '/';
			pos_ = cursor + 1;
			return true;
		}
	}

	// Text up to the closing tag of `name`; the closing tag is consumed.
	std::string_view textUntilClose(std::string_view name)
	{
		const std::string closing = "</" + std::string(name) + ">";
		const std::size_t end = text_.find(closing, pos_);
		if (end == std::string_view::npos)
			throw Error("unterminated <" + std::string(name) + ">");
		const std::string_view inner = text_.substr(pos_, end - pos_);
		pos_ = end + closing.size();
		return inner;
	}

private:
	std::string_view text_;
	std::size_t pos_ = 0;
};

std::string_view attribute(std::string_view attrs, std::string_view name)
{
	const std::string needle = std::string(name) + "=\"";
	std::size_t at = 0;
	while ((at = attrs.find(needle, at)) != std::string_view::npos)
	{
		if (at == 0 || std::isspace(static_cast<unsigned char>(attrs[at - 1])))
		{
			const std::size_t start = at + needle.size();
			return attrs.substr(start, attrs.find('"', start) - start);
		}
		at += needle.size();
	}
	return {};
}

std::array<double, 3> parseVec3(std::string_view text)
{
	std::array<double, 3> out{};
	std::istringstream in{ std::string(text) };
	for (double& value : out)
		if (!(in >> value))
			throw Error("collision vector with fewer than 3 values");
	return out;
}

std::string decodeHex(std::string_view text)
{
	std::string bytes;
	bytes.reserve(text.size() / 2);
	int high = -1;
	for (const char c : text)
	{
		int nibble;
		if (c >= '0' && c <= '9')
			nibble = c - '0';
		else if (c >= 'a' && c <= 'f')
			nibble = c - 'a' + 10;
		else if (c >= 'A' && c <= 'F')
			nibble = c - 'A' + 10;
		else
			continue; // whitespace between lines
		if (high < 0)
			high = nibble;
		else
		{
			bytes.push_back(static_cast<char>(high * 16 + nibble));
			high = -1;
		}
	}
	return bytes;
}

enum class ShapeKind { Convex, TriMesh, Box, Capsule, Sphere, Plane };

struct RawShape
{
	ShapeKind kind = ShapeKind::Plane;
	Pose localPose;
	std::string meshData;
	std::array<double, 3> box{};
	double radius = 0.0;
	double height = 0.0;
};

bool shapeKindOf(std::string_view name, ShapeKind& kind)
{
	static constexpr std::pair<std::string_view, ShapeKind> kKinds[] = {
		{ "NxConvexShapeDesc", ShapeKind::Convex },
		{ "NxTriangleMeshShapeDesc", ShapeKind::TriMesh },
		{ "NxBoxShapeDesc", ShapeKind::Box },
		{ "NxCapsuleShapeDesc", ShapeKind::Capsule },
		{ "NxSphereShapeDesc", ShapeKind::Sphere },
		{ "NxPlaneShapeDesc", ShapeKind::Plane },
	};
	for (const auto& [tag, value] : kKinds)
		if (tag == name)
		{
			kind = value;
			return true;
		}
	return false;
}

RawShape rawShape(ShapeKind kind, std::string_view attrs)
{
	RawShape shape;
	shape.kind = kind;
	if (kind == ShapeKind::Box)
		shape.box = parseVec3(attribute(attrs, "dimensions"));
	else if (kind == ShapeKind::Capsule)
	{
		shape.radius = std::stod(std::string(attribute(attrs, "radius")));
		shape.height = std::stod(std::string(attribute(attrs, "height")));
	} else if (kind == ShapeKind::Sphere) {
		shape.radius = std::stod(std::string(attribute(attrs, "radius")));
	} else if (kind == ShapeKind::Convex || kind == ShapeKind::TriMesh) {
		shape.meshData = std::string(attribute(attrs, "meshData"));
	}
	return shape;
}

struct RawActor
{
	Pose globalPose;
	std::vector<RawShape> shapes;
};

} // namespace

CollisionModel parseCollisionXml(std::string_view text)
{
	std::map<std::string, Soup> meshes;
	std::vector<RawActor> actors;
	std::vector<std::string_view> stack;
	std::string meshId;
	bool sawCollection = false;

	Scanner scanner(text);
	Tag tag;
	while (scanner.next(tag))
	{
		if (tag.close)
		{
			if (!stack.empty())
				stack.pop_back();
			continue;
		}
		const std::string_view parent = stack.empty() ? std::string_view() : stack.back();
		ShapeKind kind;
		if (tag.name == "NxuPhysicsCollection")
			sawCollection = true;
		else if (tag.name == "NxConvexMeshDesc" || tag.name == "NxTriangleMeshDesc")
			meshId = std::string(attribute(tag.attrs, "id"));
		else if (tag.name == "cookedData" && !tag.selfClose)
		{
			const std::string_view hex = scanner.textUntilClose(tag.name);
			if (hex.find_first_not_of(" \t\r\n") != std::string_view::npos)
				meshes[meshId] = decodeCooked(decodeHex(hex));
			continue;
		}
		else if (tag.name == "NxActorDesc")
			actors.emplace_back();
		else if (tag.name == "globalPose" && parent == "NxActorDesc" && !actors.empty())
		{
			actors.back().globalPose = parsePose(scanner.textUntilClose(tag.name));
			continue;
		}
		else if (parent == "NxActorDesc" && shapeKindOf(tag.name, kind) && !actors.empty())
			actors.back().shapes.push_back(rawShape(kind, tag.attrs));
		else if (tag.name == "localPose" && parent == "NxShapeDesc" && !actors.empty()
			&& !actors.back().shapes.empty())
		{
			actors.back().shapes.back().localPose = parsePose(scanner.textUntilClose(tag.name));
			continue;
		}
		if (!tag.selfClose)
			stack.push_back(tag.name);
	}
	if (!sawCollection)
		throw Error("not an NxuPhysicsCollection");

	CollisionModel model;
	for (const RawActor& actor : actors)
	{
		for (const RawShape& shape : actor.shapes)
		{
			Soup soup;
			bool closed = true;
			switch (shape.kind)
			{
			case ShapeKind::Plane:
				++model.planes;
				continue;
			case ShapeKind::Box:
				soup = boxSoup(shape.box[0], shape.box[1], shape.box[2]);
				break;
			case ShapeKind::Capsule:
				soup = capsuleSoup(shape.radius, shape.height);
				break;
			case ShapeKind::Sphere:
				soup = capsuleSoup(shape.radius, 0.0);
				break;
			case ShapeKind::TriMesh:
				closed = false;
				[[fallthrough]];
			case ShapeKind::Convex:
			{
				const auto found = meshes.find(shape.meshData);
				if (found == meshes.end())
					continue;
				soup = found->second;
				break;
			}
			}
			const Pose pose = compose(actor.globalPose, shape.localPose);
			CollisionShape out;
			out.closed = closed;
			out.indices = std::move(soup.indices);
			for (const auto& point : soup.points)
			{
				const auto p = pose.apply(point);
				// Collision files are z-up: the game folds (x, y, z) -> (x, z, -y).
				out.vertices.push_back(static_cast<float>(p[0]));
				out.vertices.push_back(static_cast<float>(p[2]));
				out.vertices.push_back(static_cast<float>(-p[1]));
			}
			for (int j = 0; j < 3; ++j)
			{
				const double cx = pose.v[j];
				const double cy = pose.v[3 + j];
				const double cz = pose.v[6 + j];
				const double length = std::sqrt(cx * cx + cy * cy + cz * cz);
				out.axes[3 * j] = static_cast<float>(cx / length);
				out.axes[3 * j + 1] = static_cast<float>(cz / length);
				out.axes[3 * j + 2] = static_cast<float>(-cy / length);
			}
			out.actor = { static_cast<float>(actor.globalPose.v[9]), static_cast<float>(actor.globalPose.v[11]),
				static_cast<float>(-actor.globalPose.v[10]) };
			if (shape.kind == ShapeKind::Box)
				out.half = { static_cast<float>(shape.box[0]), static_cast<float>(shape.box[1]), static_cast<float>(shape.box[2]) };
			model.shapes.push_back(std::move(out));
		}
	}
	return model;
}

DoorKey doorKey(std::string_view id) noexcept
{
	std::uint64_t hash = 0xcbf29ce484222325ull;
	for (const char c : id)
		hash = (hash ^ static_cast<std::uint8_t>(c)) * 0x100000001b3ull;
	return hash;
}

std::string placementId(const PlacedInstance& placed)
{
	return std::to_string(placed.cell.x) + "," + std::to_string(placed.cell.z) + "/" + placed.instanceID;
}

std::string doorId(const PlacedInstance& placed, std::size_t index)
{
	return placementId(placed) + "#" + std::to_string(index);
}

bool placementRuined(const kdata::GameData& gameData, const PlacedInstance& placed)
{
	const kdata::Record* squad = placed.squadTemplate.empty() ? nullptr : gameData.find(placed.squadTemplate);
	return squad != nullptr && squad->boolOr("building ruined", false);
}

namespace {

// A closed convex hull with an upward face steeper than the walk slope (40 degrees) but within the
// stair slope the bake accepts (55): a stair wedge or ramp, not a block. Its footprint must not be
// filled (the wedge is buried in the ground where the terrain is above it); the bake fills it by
// column instead and keeps the terrain where it is higher (WorldShape::solid).
constexpr double kWalkSlopeDegrees = 40.0;
constexpr double kStairSlopeDegrees = 55.0;
constexpr std::size_t kHullMinBoxes = 3; // addBuilding: masks with this many boxes clip upper floors to their convex hull

// The planes of the ONE convex hull of every point of a building's interior mask: every group-13 box corner plus every
// convex-mesh mask vertex (exe 1403c0510 / 140fb5260; kinspect rwh282_*.md, rwh292_*.md). Neither the boxes' intersection nor
// their union. Brute force over point triples (a mask has tens of points): a plane is kept when every point lies on one side,
// coplanar duplicates collapse. Planes face outward: inside is n . p <= d.
std::vector<std::array<float, 4>> hullPlanes(const std::vector<WorldShape>& shapes)
{
	constexpr double kPlaneEps = 1e-4;
	std::vector<std::array<double, 3>> points;
	for (const WorldShape& shape : shapes)
	{
		for (std::size_t i = 0; i + 2 < shape.vertices.size(); i += 3)
		{
			const std::array<double, 3> p = { shape.vertices[i], shape.vertices[i + 1], shape.vertices[i + 2] };
			if (std::none_of(points.begin(), points.end(), [&](const auto& q)
				{ return std::fabs(q[0] - p[0]) + std::fabs(q[1] - p[1]) + std::fabs(q[2] - p[2]) < 1e-6; }))
				points.push_back(p);
		}
	}
	std::vector<std::array<float, 4>> planes;
	const std::size_t n = points.size();
	if (n < 4)
		return planes;
	std::array<double, 3> centre{};
	for (const auto& p : points)
		for (std::size_t k = 0; k < 3; ++k)
			centre[k] += p[k] / static_cast<double>(n);
	std::vector<std::array<double, 4>> found;
	for (std::size_t i = 0; i < n; ++i)
	{
		for (std::size_t j = i + 1; j < n; ++j)
		{
			for (std::size_t k = j + 1; k < n; ++k)
			{
				const auto& a = points[i];
				const double ux = points[j][0] - a[0], uy = points[j][1] - a[1], uz = points[j][2] - a[2];
				const double vx = points[k][0] - a[0], vy = points[k][1] - a[1], vz = points[k][2] - a[2];
				double nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
				const double length = std::sqrt(nx * nx + ny * ny + nz * nz);
				if (length < 1e-9)
					continue;
				nx /= length;
				ny /= length;
				nz /= length;
				double d = nx * a[0] + ny * a[1] + nz * a[2];
				if (nx * centre[0] + ny * centre[1] + nz * centre[2] > d)
				{
					nx = -nx;
					ny = -ny;
					nz = -nz;
					d = -d;
				}
				bool supporting = nx * centre[0] + ny * centre[1] + nz * centre[2] < d - 1e-9; // else all points coplanar
				for (std::size_t m = 0; m < n && supporting; ++m)
					supporting = nx * points[m][0] + ny * points[m][1] + nz * points[m][2] <= d + kPlaneEps;
				if (!supporting)
					continue;
				const bool duplicate = std::any_of(found.begin(), found.end(), [&](const auto& f)
				{
					return std::fabs(f[0] - nx) + std::fabs(f[1] - ny) + std::fabs(f[2] - nz) < 1e-5 && std::fabs(f[3] - d) < 1e-3;
				});
				if (!duplicate)
					found.push_back({ nx, ny, nz, d });
			}
		}
	}
	for (const auto& f : found)
		planes.push_back({ static_cast<float>(f[0]), static_cast<float>(f[1]), static_cast<float>(f[2]), static_cast<float>(f[3]) });
	return planes;
}

bool hasStairFace(const CollisionShape& shape)
{
	if (!shape.closed || shape.vertices.size() < 9)
		return false;
	std::array<double, 3> centre{};
	const std::size_t count = shape.vertices.size() / 3;
	for (std::size_t i = 0; i < count; ++i)
		for (int axis = 0; axis < 3; ++axis)
			centre[static_cast<std::size_t>(axis)] += shape.vertices[i * 3 + static_cast<std::size_t>(axis)] / static_cast<double>(count);
	const auto vertex = [&](std::uint32_t index, int axis)
	{
		return static_cast<double>(shape.vertices[index * 3 + static_cast<std::size_t>(axis)]);
	};
	for (std::size_t t = 0; t + 2 < shape.indices.size(); t += 3)
	{
		std::array<double, 3> e1{};
		std::array<double, 3> e2{};
		for (int axis = 0; axis < 3; ++axis)
		{
			e1[static_cast<std::size_t>(axis)] = vertex(shape.indices[t + 1], axis) - vertex(shape.indices[t], axis);
			e2[static_cast<std::size_t>(axis)] = vertex(shape.indices[t + 2], axis) - vertex(shape.indices[t], axis);
		}
		std::array<double, 3> n = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
		const double length = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
		if (length < 1e-9)
			continue;
		double outward = 0.0;
		for (int axis = 0; axis < 3; ++axis)
			outward += n[static_cast<std::size_t>(axis)] * (vertex(shape.indices[t], axis) - centre[static_cast<std::size_t>(axis)]);
		const double up = (outward < 0 ? -n[1] : n[1]) / length;
		if (up <= 0.0)
			continue;
		const double slope = std::acos(std::min(up, 1.0)) * 180.0 / 3.14159265358979323846;
		if (slope > kWalkSlopeDegrees + 0.5 && slope <= kStairSlopeDegrees)
			return true;
	}
	return false;
}

} // namespace

void placeModel(const CollisionModel& model, const std::array<double, 3>& pos,
	const std::array<double, 4>& quat, double scale, NavUse use, bool fill,
	std::vector<WorldShape>& out)
{
	placeModel(model, pos, quat, std::array<double, 3>{ scale, scale, scale }, use, fill, out);
}

void placeModel(const CollisionModel& model, const std::array<double, 3>& pos,
	const std::array<double, 4>& quat, const std::array<double, 3>& scale3, NavUse use, bool fill,
	std::vector<WorldShape>& out)
{
	const double scale = std::cbrt(std::abs(scale3[0] * scale3[1] * scale3[2]));	// boxes' half extents only; the vertices carry the true scale
	const bool mirrored = scale3[0] * scale3[1] * scale3[2] < 0;
	const auto [w, x, y, z] = quat;
	// Ogre Quaternion * Vector3: v + w t + q.xyz x t, t = 2 q.xyz x v.
	const auto rotate = [&](double vx, double vy, double vz) -> std::array<double, 3>
	{
		const double tx = 2 * (y * vz - z * vy);
		const double ty = 2 * (z * vx - x * vz);
		const double tz = 2 * (x * vy - y * vx);
		return { vx + w * tx + (y * tz - z * ty), vy + w * ty + (z * tx - x * tz), vz + w * tz + (x * ty - y * tx) };
	};
	for (const CollisionShape& shape : model.shapes)
	{
		WorldShape world;
		world.use = use;
		const bool stairHull = use == NavUse::Walkable && fill && hasStairFace(shape);
		world.closed = shape.closed && fill && !stairHull;
		world.solid = shape.closed && (!fill || stairHull) && use == NavUse::Walkable;
		world.indices = shape.indices;
		world.min = { 1e30f, 1e30f, 1e30f };
		world.max = { -1e30f, -1e30f, -1e30f };
		for (std::size_t i = 0; i + 2 < shape.vertices.size(); i += 3)
		{
			const auto r = rotate(shape.vertices[i] * scale3[0], shape.vertices[i + 1] * scale3[1], shape.vertices[i + 2] * scale3[2]);
			const std::array<float, 3> p = {
				static_cast<float>(pos[0] + r[0]),
				static_cast<float>(pos[1] + r[1]),
				static_cast<float>(pos[2] + r[2]),
			};
			world.vertices.insert(world.vertices.end(), p.begin(), p.end());
			for (int axis = 0; axis < 3; ++axis)
			{
				world.min[axis] = std::min(world.min[axis], p[axis]);
				world.max[axis] = std::max(world.max[axis], p[axis]);
			}
		}
		for (int j = 0; j < 3; ++j)
		{
			const auto r = rotate(shape.axes[3 * j], shape.axes[3 * j + 1], shape.axes[3 * j + 2]);
			for (int axis = 0; axis < 3; ++axis)
				world.axes[3 * j + axis] = static_cast<float>(r[axis]);
			world.half[j] = static_cast<float>(shape.half[j] * scale);
		}
		const auto actor = rotate(shape.actor[0] * scale, shape.actor[1] * scale, shape.actor[2] * scale);
		for (int axis = 0; axis < 3; ++axis)
			world.actor[axis] = static_cast<float>(pos[axis] + actor[axis]);
		if (mirrored)	// a mirroring scale flips the winding
			for (std::size_t i = 0; i + 2 < world.indices.size(); i += 3)
				std::swap(world.indices[i + 1], world.indices[i + 2]);
		if (!world.indices.empty())
			out.push_back(std::move(world));
	}
}

namespace {

// Lower-case, forward slashes, no leading "./" or "data/" (the record paths
// look like ".\data\buildings\walls\basicgate.XML").
std::string normalisePath(std::string path)
{
	for (char& c : path)
		c = c == '\\' ? '/' : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	const auto strip = [&](std::string_view prefix)
	{
		if (path.compare(0, prefix.size(), prefix) == 0)
			path.erase(0, prefix.size());
	};
	strip("./");
	strip("data/");
	return path;
}

} // namespace

CollisionLibrary::CollisionLibrary(std::string_view dataDir, const kdata::GameData& gameData)
	: gameData_(gameData)
{
	namespace fs = std::filesystem;
	const fs::path root{ std::string(dataDir) };
	std::error_code ec;
	for (fs::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec))
		if (it->is_regular_file(ec))
			files_.emplace(normalisePath(it->path().lexically_relative(root).generic_string()), // not fs::relative: it canonicalises (a stat per path component per file)
				it->path().string());
}

const CollisionModel* CollisionLibrary::model(const std::string& recordPath, bool& unreadable)
{
	unreadable = false;
	if (recordPath.empty())
		return nullptr;
	const auto file = files_.find(normalisePath(recordPath));
	if (file == files_.end())
	{
		unreadable = true;
		return nullptr;
	}
	auto cached = models_.find(file->second);
	if (cached == models_.end())
	{
		std::ifstream in(file->second, std::ios::binary);
		std::ostringstream text;
		text << in.rdbuf();
		std::unique_ptr<CollisionModel> parsed;
		try
		{
			parsed = std::make_unique<CollisionModel>(parseCollisionXml(text.str()));
		} catch (const Error&) {
		}
		cached = models_.emplace(file->second, std::move(parsed)).first;
	}
	unreadable = cached->second == nullptr;
	return cached->second.get();
}

bool CollisionLibrary::partHasMesh(const kdata::Record& part) const
{
	const std::string path = normalisePath(part.fileOr("phs or mesh"));
	return path.ends_with(".mesh") && files_.contains(path);
}

bool CollisionLibrary::addInteriorMask(const PlacedInstance& placed, std::vector<WorldShape>& out)
{
	const kdata::Record* building = gameData_.find(placed.buildingID);
	if (building == nullptr || building->type != itemTypeByName("BUILDING"))
		return false;
	bool added = false;
	// A mask's chance is 0 in the data (the Storm House's): a trigger that always exists, no draw.
	for (const std::string& partID : selectBuildingParts(gameData_, *building, static_cast<float>(placed.x),
		static_cast<float>(placed.z), PartStage::InteriorMask))
	{
		const kdata::Record* part = gameData_.find(partID);
		bool unreadable = false;
		if (part == nullptr)
			continue;
		if (const CollisionModel* shapes = model(part->fileOr("xml collision"), unreadable))
		{
			placeModel(*shapes, { placed.x, placed.y, placed.z },
				{ placed.qw, placed.qx, placed.qy, placed.qz }, placed.scale, NavUse::Obstacle, false, out);
			added = true;
		}
	}
	return added;
}

std::size_t CollisionLibrary::addFurniture(const PlacedInstance& building, const InteriorLayout& layout,
	std::vector<WorldShape>& out, CollisionCoverage& coverage)
{
	CollisionCoverage scratch; // the pieces are not zone placements: keep them out of those counts
	for (const LayoutItem& item : layout.items)
	{
		const std::size_t before = out.size();
		addBuilding(placeFurniture(building, item, gameData_), out, scratch, false, true);
		coverage.furnitureWithCollision += out.size() > before;
	}
	coverage.furniture += layout.items.size();
	return layout.items.size();
}

std::optional<BuildingNav> CollisionLibrary::addBuilding(const PlacedInstance& placed,
	std::vector<WorldShape>& out, CollisionCoverage& coverage, bool interiors, bool furniture)
{
	++coverage.placements;
	const kdata::Record* building = gameData_.find(placed.buildingID);
	if (building == nullptr || building->type != itemTypeByName("BUILDING"))
	{
		++coverage.placementsMissingTarget;
		return std::nullopt;
	}
	BuildingNav nav;
	nav.pathMode = static_cast<PathMode>(std::clamp(
		building->intOr("path mode", static_cast<int>(PathMode::Obstacle)), 0, 3));
	nav.buildThreshold = building->floatOr("build threshold", 0.0f);
	nav.destroyedNavmesh = building->boolOr("destroyed navmesh", false);
	nav.ruined = placementRuined(gameData_, placed);
	nav.gateway = building->boolOr("is gateway", false);
	nav.enforceCeiling = building->boolOr("enforce ceiling", false);
	nav.cavernous = building->boolOr("cavernous", false);
	nav.shelter = building->boolOr("is shelter", false);
	nav.interiorTerrain = building->boolOr("interior terrain", false);
	if ((furniture || building->boolOr("is interior furniture", false)) && nav.pathMode == PathMode::Projected)
		nav.pathMode = PathMode::Obstacle;
	// AI nodes (Node Generaal, Node Shopkeeper, n-guardpost, ...): invisible markers where NPCs stand; their selection box is not a solid.
	if (building->boolOr("is node", false))
		nav.pathMode = PathMode::Ignore;
	// Layout seats (a functionality with function 17, chair or stool; ksim routines::kFunctionChair): a sitter stands on the seat's own
	// spot, so the seat is not cut from the floor (ksim_town_bar). UNSURE: the original may walk to a use node beside it and then sit.
	if (furniture)
		if (const kdata::RefCategory* functionality = building->findRefs("functionality"))
			for (const kdata::RefEntry& entry : functionality->entries)
				if (const kdata::Record* record = gameData_.find(entry.targetID); record != nullptr && record->intOr("function", 0) == 17)
					nav.pathMode = PathMode::Ignore;
	++coverage.pathModes[static_cast<std::size_t>(nav.pathMode)];
	coverage.gatewayPlacements += nav.gateway;

	std::vector<WorldShape> placedShapes;
	std::size_t withCollision = 0;
	std::size_t renderOnly = 0;
	std::vector<std::size_t> upper; // placedShapes of upper-floor and stair parts
	std::vector<std::string> chosen = selectBuildingParts(gameData_, *building, static_cast<float>(placed.x), static_cast<float>(placed.z), PartStage::Exterior);
	// The floors and staircases of a walkable building are `interior` parts (their collision is not in
	// `parts`), with the same chance rule.
	const std::size_t exteriorParts = chosen.size();
	if (interiors && nav.pathMode == PathMode::Walkable)
	{
		std::vector<std::string> inside = selectBuildingParts(gameData_, *building, static_cast<float>(placed.x), static_cast<float>(placed.z), PartStage::Interior);
		chosen.insert(chosen.end(), inside.begin(), inside.end());
	}
	for (std::size_t chosenIndex = 0; chosenIndex < chosen.size(); ++chosenIndex)
	{
		const std::string& partID = chosen[chosenIndex];
		const bool interiorPart = chosenIndex >= exteriorParts;
		++coverage.parts;
		const kdata::Record* part = gameData_.find(partID);
		if (part == nullptr)
		{
			++coverage.partsNoGeometry;
			continue;
		}
		// `passable`: collision used only for clicking (COLLISION_FORMAT.md 3.3).
		if (part->boolOr("passable", false) || part->boolOr("no collision", false))
		{
			++coverage.partsFlagged;
			continue;
		}
		// A ruin (STORM_HOUSE_INTERIOR_LIVE.md "Static chain": the part loader reads `destroyed collision` when destroyed; the schema's
		// `destroyed mesh`: "upper floors will be removed if blank"). UNSURE: that a removed upper floor takes its collision with it.
		// Interior stairs go with them: the live ruined Storm House (3791 InteriorStairs) has no mesh above its floor, ours baked
		// a slope to the old roof (ground-floor interior parts stay: they can carry the floor).
		std::string collision = part->fileOr("xml collision");
		if (nav.ruined)
		{
			const bool destroyedMesh = !part->fileOr("destroyed mesh").empty();
			if ((part->intOr("building floor", 0) >= 1 || (interiorPart && part->boolOr("is stairs", false))) && !destroyedMesh)
			{
				++coverage.partsRuinRemoved;
				continue;
			}
			if (destroyedMesh && !part->fileOr("destroyed collision").empty())
				collision = part->fileOr("destroyed collision");
		}
		bool unreadable = false;
		if (const CollisionModel* shapes = model(collision, unreadable))
		{
			++coverage.partsWithCollision;
			++withCollision;
			NavUse use = NavUse::Walkable;
			bool fill = true;
			if (nav.pathMode == PathMode::Projected)
				use = NavUse::Projected;
			else if (nav.pathMode == PathMode::Obstacle || part->boolOr("is unwalkable roof", false))
				use = NavUse::Obstacle;
			else
				fill = !nav.gateway && !part->boolOr("is stairs", false);
			const std::size_t first = placedShapes.size();
			placeModel(*shapes, { placed.x, placed.y, placed.z },
				{ placed.qw, placed.qx, placed.qy, placed.qz }, placed.scale, use, fill, placedShapes);
			if (part->intOr("building floor", 0) >= 1 || part->boolOr("is stairs", false))
				for (std::size_t i = first; i < placedShapes.size(); ++i)
					upper.push_back(i);
		} else if (partHasMesh(*part)) {
			coverage.partsFileUnreadable += unreadable;
			++coverage.partsRenderOnly;
			++renderOnly;
		} else {
			coverage.partsFileUnreadable += unreadable;
			++coverage.partsNoGeometry;
		}
	}
	// A walkable building with an interior mask: the mask volume is ONE convex hull of all its points (hullPlanes), not the boxes'
	// intersection. UPPER shapes only (`building floor` >= 1 and stairs, `upper`) keep inside it: on this Recast path the zone side and the
	// interior bake from the same shapes, and in the original the hull bounds the interior mesh only, while the zone mesh is cut by the
	// boxes. Keeping every shape inside the hull cut the ground outside it (14 tests). A mask with no box (Outpost s-IV's) has hull
	// planes too, but BuildingSite floors (world.cpp) keep the height-by-slab rule for it.
	// Terrain inside the mask is left out of the mesh unless the BUILDING says `interior terrain` (rwh.90), so every shape carries that.
	for (const std::size_t i : upper)
		placedShapes[i].upperPart = true;
	if (nav.pathMode == PathMode::Walkable && (!upper.empty() || !nav.interiorTerrain))
	{
		std::vector<WorldShape> mask;
		if (addInteriorMask(placed, mask))
		{
			// Boxes only: the planes of each box, one volume per box (the carve below) or all together (their intersection).
			std::vector<std::vector<std::array<float, 4>>> boxVolumes;
			for (const WorldShape& box : mask)
				if (box.half != std::array<float, 3>{})
					if (auto own = hullPlanes({ box }); !own.empty())
						boxVolumes.push_back(std::move(own));
			// The hull replaces the boxes' intersection only where rwh.282 / rwh.292 measured it: a mask of kHullMinBoxes boxes or more (the
			// Longhouse's 3: the intersection cuts every upper floor) or one with a convex-mesh shape (the Hub's interiors, unclipped before).
			// Two overlapping boxes keep the intersection: the hull keeps upper floors there that the Recast bake then leaves as islands
			// (4014-TwoStorey: 33 new island regions, kenshi-k16.7.142); the generator (NAVMESH_GENERATOR s0) applies the hull to all.
			const bool hullShape = std::any_of(mask.begin(), mask.end(), [](const WorldShape& shape) { return shape.half == std::array<float, 3>{}; });
			std::vector<std::array<float, 4>> planes;
			if (boxVolumes.size() >= kHullMinBoxes || hullShape)
				planes = hullPlanes(mask);
			else
				for (const auto& volume : boxVolumes)
					planes.insert(planes.end(), volume.begin(), volume.end());
			if (!planes.empty())
			{
				const auto shared = std::make_shared<const std::vector<std::array<float, 4>>>(std::move(planes));
				for (const std::size_t i : upper)
					placedShapes[i].keepInside = shared;
				// The terrain carve stays the union of the mask's boxes (rwh.90, knav_interior_terrain); only the floor clip is the hull.
				if (!nav.interiorTerrain && !boxVolumes.empty())
				{
					const auto carve = std::make_shared<const std::vector<std::vector<std::array<float, 4>>>>(std::move(boxVolumes));
					for (WorldShape& shape : placedShapes)
						shape.carveTerrain = carve;
				}
			}
		}
	}
	// Doors: child buildings whose parts are modelled in the parent's frame (the door slab of
	// Defensive Gate IV sits in the gap between its towers with no offset), so they are placed with the
	// parent's transform and scale. Their own path mode (OBSTACLE) is ignored on purpose: a
	// door's state must not change the tiles. A ruin keeps the doorways, not the doors (BuildingNav::ruined).
	if (const auto* doors = building->findRefs("doors"))
	{
		std::size_t index = 0;
		for (const auto& entry : doors->entries)
		{
			if (entry.v0 == kdata::kReferenceRemoved)
				continue;
			const std::size_t at = index++;
			const kdata::Record* door = gameData_.find(entry.targetID);
			if (door == nullptr)
				continue;
			const DoorKey key = doorKey(doorId(placed, at));
			const std::size_t first = placedShapes.size();
			for (const std::string& partID : selectBuildingParts(gameData_, *door, static_cast<float>(placed.x), static_cast<float>(placed.z), PartStage::Door))
			{
				const kdata::Record* part = gameData_.find(partID);
				bool unreadable = false;
				if (part == nullptr)
					continue;
				if (const CollisionModel* shapes = model(part->fileOr("xml collision"), unreadable))
					placeModel(*shapes, { placed.x, placed.y, placed.z }, { placed.qw, placed.qx, placed.qy, placed.qz },
						placed.scale, NavUse::Door, false, placedShapes);
			}
			if (placedShapes.size() == first)
			{
				++coverage.doorsWithoutCollision;
				continue;
			}
			if (nav.ruined)
				continue;
			++coverage.doors;
			nav.doors.push_back(key);
			for (std::size_t i = first; i < placedShapes.size(); ++i)
				placedShapes[i].door = key;
		}
	}
	// Furniture, as against a building's shell: a layout piece, or a BUILDING record flagged `is interior furniture` (barrel, counter, bed,
	// crate, table, rack: the flag is on the record, so a piece placed on its own in a zone has it too). A house, wall or gateway has not.
	if (furniture || building->boolOr("is interior furniture", false))
		for (WorldShape& shape : placedShapes)
			shape.furniture = shape.use != NavUse::Door;
	// Use nodes in the world (the same placement as the parts: scaled, rotated by the quaternion, moved to the placement).
	{
		constexpr std::int32_t kNodeUse = 4; // ksim routines::kNodeUse
		std::vector<std::array<float, 3>> nodes;
		const double w = placed.qw, qx = placed.qx, qy = placed.qy, qz = placed.qz;
		for (const kdata::Instance& instance : building->instances)
		{
			const kdata::Record* node = instance.targetStringID.empty() ? nullptr : gameData_.find(instance.targetStringID);
			if (node == nullptr || !node->boolOr("is node", false) || node->intOr("node type", 1) != kNodeUse || instance.transform.size() < 12)
				continue;
			float local[3];
			std::memcpy(local, instance.transform.data(), sizeof local);
			const double vx = local[0] * placed.scale, vy = local[1] * placed.scale, vz = local[2] * placed.scale;
			const double tx = 2 * (qy * vz - qz * vy), ty = 2 * (qz * vx - qx * vz), tz = 2 * (qx * vy - qy * vx);
			nodes.push_back({ static_cast<float>(placed.x + vx + w * tx + (qy * tz - qz * ty)),
				static_cast<float>(placed.y + vy + w * ty + (qz * tx - qx * tz)),
				static_cast<float>(placed.z + vz + w * tz + (qx * ty - qy * tx)) });
		}
		if (!nodes.empty())
		{
			const auto shared = std::make_shared<const std::vector<std::array<float, 3>>>(std::move(nodes));
			for (WorldShape& shape : placedShapes)
				shape.useNodes = shared;
		}
	}
	if (withCollision != 0)
		++coverage.placementsWithCollision;
	else if (renderOnly != 0)
		++coverage.placementsRenderOnly;
	else
		++coverage.placementsNoGeometry;
	if (nav.pathMode != PathMode::Ignore)
		for (WorldShape& shape : placedShapes)
			out.push_back(std::move(shape));
	return nav;
}

bool CollisionLibrary::addFoliage(const FoliageInstance& instance, std::vector<WorldShape>& out,
	CollisionCoverage& coverage)
{
	++coverage.foliageInstances;
	const kdata::Record* mesh = gameData_.find(instance.recordID);
	if (mesh == nullptr)
		return false;
	const std::array<double, 3> pos = { instance.x, instance.y, instance.z };
	bool unreadable = false;
	if (const CollisionModel* shapes = model(mesh->fileOr("collision"), unreadable))
	{
		++coverage.foliageWithCollision;
		const bool walkable = mesh->boolOr("walkable", false);
		coverage.foliageWalkable += walkable;
		const std::size_t first = out.size();
		placeModel(*shapes, pos, { instance.qw, instance.qx, instance.qy, instance.qz },
			instance.scale, walkable ? NavUse::Walkable : NavUse::Obstacle, true, out);
		// A rock's collision is an open shell: the bake cuts its footprint (knav bake.cpp rockShell), or its
		// low edges stay standable and a deposit's approach stands inside the drawn rock (the iron by the Hub).
		for (std::size_t i = first; i < out.size(); ++i)
		{
			out[i].foliage = true;
			out[i].solid = out[i].solid || (out[i].use == NavUse::Obstacle && !out[i].closed);
		}
		return true;
	}
	// No collision: a disc of `navmesh cutter` radius, scaled by the instance.
	const double radius = mesh->floatOr("navmesh cutter", 0.0f) * instance.scale;
	if (radius <= 0.0)
		return false;
	constexpr int kSides = 12;
	CollisionShape disc;
	disc.closed = true;
	for (const double y : { 0.0, 1.0 })
		for (int k = 0; k < kSides; ++k)
		{
			disc.vertices.push_back(static_cast<float>(radius * std::cos(2 * kPi * k / kSides)));
			disc.vertices.push_back(static_cast<float>(y));
			disc.vertices.push_back(static_cast<float>(radius * std::sin(2 * kPi * k / kSides)));
		}
	for (std::uint32_t k = 0; k < kSides; ++k)
	{
		const std::uint32_t next = (k + 1) % kSides;
		for (const std::uint32_t index : { k, k + kSides, next + kSides, k, next + kSides, next })
			disc.indices.push_back(index);
	}
	CollisionModel cutter;
	cutter.shapes.push_back(std::move(disc));
	++coverage.foliageCutters;
	placeModel(cutter, pos, { 1.0, 0.0, 0.0, 0.0 }, 1.0, NavUse::Projected, true, out);
	return true;
}

bool CollisionLibrary::addFeature(const FeaturePlacement& placement, std::vector<WorldShape>& out)
{
	if (!placement.rotationFinite())
		return false;
	const kdata::Record* record = gameData_.find(placement.recordID);
	if (record == nullptr)
		return false;
	bool unreadable = false;
	const CollisionModel* shapes = model(record->fileOr("collision"), unreadable);
	if (shapes == nullptr)
		return false;
	placeModel(*shapes, { placement.x, placement.y, placement.z }, { placement.qw, placement.qx, placement.qy, placement.qz },
		{ placement.scaleX, placement.scaleY, placement.scaleZ }, NavUse::Walkable, false, out);
	return true;
}

double CollisionLibrary::featureReach(const FeaturePlacement& placement)
{
	const kdata::Record* record = gameData_.find(placement.recordID);
	if (record == nullptr)
		return 0.0;
	const std::string file = record->fileOr("collision");
	const auto known = reach_.find(file);
	double reach;
	if (known != reach_.end())
	{
		reach = known->second;
	} else {
		reach = 0.0;
		bool unreadable = false;
		if (const CollisionModel* shapes = model(file, unreadable))
			for (const CollisionShape& s : shapes->shapes)
				for (std::size_t i = 0; i + 2 < s.vertices.size(); i += 3)
					reach = std::max(reach, (double)std::sqrt(s.vertices[i] * s.vertices[i] + s.vertices[i + 1] * s.vertices[i + 1] + s.vertices[i + 2] * s.vertices[i + 2]));
		reach_[file] = reach;
	}
	return reach * std::max({ std::fabs(placement.scaleX), std::fabs(placement.scaleY), std::fabs(placement.scaleZ) });
}

std::vector<std::array<float, 4>> interiorMaskHull(const std::vector<WorldShape>& mask)
{
	return hullPlanes(mask);
}

} // namespace kworld
