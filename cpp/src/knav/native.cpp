#include "knav/native.hpp"

#include "kworld/coords.hpp"

#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace knav::native {

namespace {

// Member types of the tagfile's class definitions; an array or a fixed tuple of any of them is flagged on top.
enum : int { kVoid, kByte, kInt, kReal, kVec4, kVec8, kVec12, kVec16, kObject, kStruct, kCString };
constexpr int kArray = 0x10;
constexpr int kTuple = 0x20;

int vectorSize(int type)
{
	switch (type)
	{
	case kVec4: return 4;
	case kVec8: return 8;
	case kVec12: return 12;
	case kVec16: return 16;
	default: return 0;
	}
}

// One decoded value: what a member holds. Objects keep their members by name; arrays are lists.
struct Value
{
	enum class Kind : std::uint8_t { None, Int, Real, Floats, String, Ref, Object, List };
	Kind kind = Kind::None;
	std::int64_t i = 0; // Int, Ref
	double r = 0.0;
	std::vector<float> floats;
	std::string s;
	std::vector<std::pair<std::string, Value>> members;
	std::vector<Value> items;

	const Value* field(std::string_view name) const
	{
		for (const auto& [key, value] : members)
		{
			if (key == name)
				return &value;
		}
		return nullptr;
	}
	std::int64_t intOf(std::string_view name, std::int64_t fallback = 0) const
	{
		const Value* v = field(name);
		return v != nullptr && v->kind == Kind::Int ? v->i : fallback;
	}
	std::string stringOf(std::string_view name) const
	{
		const Value* v = field(name);
		return v != nullptr && v->kind == Kind::String ? v->s : std::string();
	}
	const std::vector<Value>& listOf(std::string_view name) const
	{
		static const std::vector<Value> none;
		const Value* v = field(name);
		return v != nullptr && v->kind == Value::Kind::List ? v->items : none;
	}
};

struct Member
{
	std::string name;
	int type = 0;
	std::string cls;
	int count = 0;
};

struct Class
{
	std::string name;
	int parent = 0;
	std::vector<Member> members;
};

// The Havok 2014 binary tagfile: magic, then tagged records (file info, class definitions, objects, back references, end).
// Integers are varints whose first byte carries the sign in bit 0; strings and classes are interned as they first appear.
class Reader
{
public:
	explicit Reader(std::vector<unsigned char> data) : d_(std::move(data)) {}

	void run()
	{
		static const unsigned char magic[8] = {0x1e, 0x0d, 0xb0, 0xca, 0xce, 0xfa, 0x11, 0xd0};
		if (d_.size() < 8 || std::memcmp(d_.data(), magic, 8) != 0)
			throw std::runtime_error("not a Havok binary tagfile");
		p_ = 8;
		for (;;)
		{
			switch (integer())
			{
			case 1: // file info: version, SDK string, 6 more bytes (constant in every navtile)
				integer();
				string();
				need(6);
				p_ += 6;
				break;
			case 2:
				meta();
				break;
			case 3:
				all_.push_back(body(static_cast<int>(integer())));
				break;
			case 4:
				all_.push_back(body(static_cast<int>(integer())));
				remembered_.push_back(all_.size() - 1);
				break;
			case 5: // a back reference as a root
				integer();
				break;
			case 6: // a null root
				break;
			case 7:
				return;
			default:
				throw std::runtime_error("unknown tag at " + std::to_string(p_));
			}
		}
	}

	// The first object of the file (the root level container).
	const Value& root() const
	{
		if (all_.empty())
			throw std::runtime_error("no objects");
		return all_.front();
	}
	// A remembered object by its reference (1-based).
	const Value& object(std::int64_t ref) const
	{
		if (ref < 1 || static_cast<std::size_t>(ref) > remembered_.size())
			throw std::runtime_error("bad object reference " + std::to_string(ref));
		return all_[remembered_[static_cast<std::size_t>(ref - 1)]];
	}

private:
	void need(std::size_t n) const
	{
		if (p_ + n > d_.size())
			throw std::runtime_error("truncated at " + std::to_string(p_));
	}
	unsigned char byte()
	{
		need(1);
		return d_[p_++];
	}
	std::int64_t integer()
	{
		unsigned char b = byte();
		const bool negative = (b & 1) != 0;
		std::int64_t v = (b >> 1) & 0x3f;
		int shift = 6;
		while ((b & 0x80) != 0)
		{
			b = byte();
			v |= static_cast<std::int64_t>(b & 0x7f) << shift;
			shift += 7;
		}
		return negative ? -v : v;
	}
	std::vector<float> floats(std::size_t n)
	{
		need(4 * n);
		std::vector<float> out(n);
		std::memcpy(out.data(), d_.data() + p_, 4 * n);
		p_ += 4 * n;
		return out;
	}
	std::string string()
	{
		const std::int64_t n = integer();
		if (n <= 0)
		{
			if (static_cast<std::size_t>(-n) >= strings_.size())
				throw std::runtime_error("bad string reference at " + std::to_string(p_));
			return strings_[static_cast<std::size_t>(-n)];
		}
		need(static_cast<std::size_t>(n));
		strings_.emplace_back(reinterpret_cast<const char*>(d_.data() + p_), static_cast<std::size_t>(n));
		p_ += static_cast<std::size_t>(n);
		return strings_.back();
	}
	void meta()
	{
		Class c;
		c.name = string();
		integer(); // version
		c.parent = static_cast<int>(integer());
		const std::int64_t n = integer();
		for (std::int64_t k = 0; k < n; ++k)
		{
			Member m;
			m.name = string();
			m.type = static_cast<int>(integer());
			if ((m.type & 0xf) == kObject || (m.type & 0xf) == kStruct)
				m.cls = string();
			if ((m.type & kTuple) != 0)
				m.count = static_cast<int>(integer());
			c.members.push_back(std::move(m));
		}
		classes_.push_back(std::move(c));
	}
	// A class's members, its ancestors' first.
	std::vector<Member> allMembers(int index) const
	{
		if (index <= 0 || static_cast<std::size_t>(index) >= classes_.size())
			throw std::runtime_error("bad class index " + std::to_string(index));
		const Class& c = classes_[static_cast<std::size_t>(index)];
		std::vector<Member> out = c.parent != 0 ? allMembers(c.parent) : std::vector<Member>{};
		out.insert(out.end(), c.members.begin(), c.members.end());
		return out;
	}
	int classIndex(const std::string& name) const
	{
		for (std::size_t k = 1; k < classes_.size(); ++k)
		{
			if (classes_[k].name == name)
				return static_cast<int>(k);
		}
		throw std::runtime_error("unknown class " + name);
	}
	// Which of n members are present: a little-endian bit set.
	std::vector<bool> bits(std::size_t n)
	{
		const std::size_t bytes = (n + 7) / 8;
		need(bytes);
		std::vector<bool> out(n);
		for (std::size_t k = 0; k < n; ++k)
			out[k] = ((d_[p_ + k / 8] >> (k % 8)) & 1) != 0;
		p_ += bytes;
		return out;
	}
	Value body(int index)
	{
		Value o;
		o.kind = Value::Kind::Object;
		const std::vector<Member> members = allMembers(index);
		const std::vector<bool> have = bits(members.size());
		for (std::size_t k = 0; k < members.size(); ++k)
		{
			if (have[k])
				o.members.emplace_back(members[k].name, value(members[k].type, members[k].cls, members[k].count));
		}
		return o;
	}
	Value value(int type, const std::string& cls, int count)
	{
		if ((type & kTuple) != 0)
			return array(type & ~kTuple, cls, count);
		if ((type & kArray) != 0)
			return array(type & ~kArray, cls, static_cast<std::size_t>(integer()));
		Value v;
		switch (type)
		{
		case kByte:
			v.kind = Value::Kind::Int;
			v.i = byte();
			return v;
		case kInt:
			v.kind = Value::Kind::Int;
			v.i = integer();
			return v;
		case kReal:
			v.kind = Value::Kind::Real;
			v.r = floats(1)[0];
			return v;
		case kObject:
			v.kind = Value::Kind::Ref;
			v.i = integer();
			return v;
		case kStruct:
			return body(classIndex(cls));
		case kCString:
			v.kind = Value::Kind::String;
			v.s = string();
			return v;
		default:
			if (const int n = vectorSize(type); n != 0)
			{
				v.kind = Value::Kind::Floats;
				v.floats = floats(static_cast<std::size_t>(n));
				return v;
			}
			throw std::runtime_error("member type " + std::to_string(type) + " at " + std::to_string(p_));
		}
	}
	Value array(int type, const std::string& cls, std::size_t n)
	{
		Value list;
		list.kind = Value::Kind::List;
		if (n == 0)
			return list;
		list.items.resize(n);
		if (type == kByte)
		{
			need(n);
			for (Value& item : list.items)
			{
				item.kind = Value::Kind::Int;
				item.i = d_[p_++];
			}
		} else if (type == kInt) {
			integer(); // the element width in bytes (4 or 8); the values stay varints
			for (Value& item : list.items)
			{
				item.kind = Value::Kind::Int;
				item.i = integer();
			}
		} else if (type == kReal) {
			const std::vector<float> f = floats(n);
			for (std::size_t k = 0; k < n; ++k)
			{
				list.items[k].kind = Value::Kind::Real;
				list.items[k].r = f[k];
			}
		} else if (vectorSize(type) != 0) {
			const std::size_t stored = static_cast<std::size_t>(integer()); // components stored per vector (3: w dropped)
			const std::vector<float> f = floats(n * stored);
			for (std::size_t k = 0; k < n; ++k)
			{
				list.items[k].kind = Value::Kind::Floats;
				list.items[k].floats.resize(stored);
				if (stored != 0)
					std::memcpy(list.items[k].floats.data(), f.data() + k * stored, stored * sizeof(float));
			}
		} else if (type == kStruct) {
			// A struct array is stored by column: each present member's n values in a row.
			const std::vector<Member> members = allMembers(classIndex(cls));
			for (Value& item : list.items)
				item.kind = Value::Kind::Object;
			const std::vector<bool> have = bits(members.size());
			for (std::size_t m = 0; m < members.size(); ++m)
			{
				if (!have[m])
					continue;
				const Member& member = members[m];
				std::vector<Value> column;
				if ((member.type & kArray) != 0)
				{
					for (std::size_t k = 0; k < n; ++k)
						column.push_back(value(member.type, member.cls, 0));
				} else if ((member.type & kTuple) != 0) {
					for (std::size_t k = 0; k < n; ++k)
						column.push_back(array(member.type & ~kTuple, member.cls, static_cast<std::size_t>(member.count)));
				} else {
					column = std::move(array(member.type, member.cls, n).items);
				}
				for (std::size_t k = 0; k < n; ++k)
					list.items[k].members.emplace_back(member.name, std::move(column[k]));
			}
		} else {
			for (Value& item : list.items)
				item = value(type, cls, 0);
		}
		return list;
	}

	std::vector<unsigned char> d_;
	std::size_t p_ = 0;
	std::vector<std::string> strings_{"", ""};
	std::vector<Class> classes_{Class{}};
	std::vector<Value> all_;
	std::vector<std::size_t> remembered_;
};

// An hkaiNavMesh object into world coordinates: its vertices are tenths of a game unit from `base`.
void fill(Mesh& mesh, const Value& nav)
{
	for (const auto& [key, v] : nav.members)
	{
		if (v.kind == Value::Kind::Int)
			mesh.scalars.emplace_back(key, static_cast<double>(v.i));
		else if (v.kind == Value::Kind::Real)
			mesh.scalars.emplace_back(key, v.r);
	}
	for (const Value& v : nav.listOf("vertices"))
	{
		if (v.floats.size() < 3)
			throw std::runtime_error("navmesh vertex with fewer than 3 components");
		mesh.vertices.push_back({mesh.base[0] + 10.0 * v.floats[0], mesh.base[1] + 10.0 * v.floats[1], mesh.base[2] + 10.0 * v.floats[2]});
	}
	const std::vector<Value>& data = nav.listOf("faceData");
	const std::vector<Value>& faces = nav.listOf("faces");
	for (std::size_t k = 0; k < faces.size(); ++k)
	{
		Face face;
		face.startEdge = static_cast<std::uint32_t>(faces[k].intOf("startEdgeIndex"));
		face.numEdges = static_cast<std::uint16_t>(faces[k].intOf("numEdges"));
		face.data = k < data.size() ? static_cast<std::int32_t>(data[k].i) : 0;
		mesh.faces.push_back(face);
	}
	for (const Value& e : nav.listOf("edges"))
	{
		Edge edge;
		edge.a = static_cast<std::uint32_t>(e.intOf("a"));
		edge.b = static_cast<std::uint32_t>(e.intOf("b"));
		edge.oppositeEdge = static_cast<std::int32_t>(e.intOf("oppositeEdge", -1));
		edge.oppositeFace = static_cast<std::int32_t>(e.intOf("oppositeFace", -1));
		edge.flags = static_cast<std::uint8_t>(e.intOf("flags"));
		if (edge.a >= mesh.vertices.size() || edge.b >= mesh.vertices.size())
			throw std::runtime_error("navmesh edge vertex out of range");
		mesh.edges.push_back(edge);
	}
	for (const Face& face : mesh.faces)
	{
		if (static_cast<std::size_t>(face.startEdge) + face.numEdges > mesh.edges.size())
			throw std::runtime_error("navmesh face edges out of range");
	}
	for (const Value& set : nav.listOf("streamingSets"))
	{
		for (const Value& connection : set.listOf("meshConnections"))
		{
			const std::int64_t edge = connection.intOf("edgeIndex", -1);
			if (edge >= 0 && static_cast<std::size_t>(edge) < mesh.edges.size())
				mesh.edges[static_cast<std::size_t>(edge)].linked = true;
		}
	}
}

} // namespace

std::string navtilesDir(const std::string& dataDir)
{
	return dataDir + "/newland/land/navtiles";
}

std::vector<Mesh> loadTile(const std::string& dir, int i, int j, StoredHashes* stored)
{
	const std::string path = dir + "/tile" + std::to_string(i) + "." + std::to_string(j) + ".hkt";
	if (!std::filesystem::exists(path))
		return {};
	std::ifstream in(path, std::ios::binary);
	std::vector<unsigned char> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
	Reader reader(std::move(data));
	std::vector<Mesh> meshes;
	std::vector<const Value*> navs;
	bool infoAwaitsHash = false; // the interior Info just read: its Hash is the next Hash variant
	try
	{
		reader.run();
		// The root container's named variants: the world mesh, then the zone's Hash, then each interior's NavMesh and
		// Info followed by its Hash (kinspect findings/navhash.md section 5).
		for (const Value& variant : reader.root().listOf("namedVariants"))
		{
			const Value* ref = variant.field("variant");
			if (ref == nullptr || ref->kind != Value::Kind::Ref)
				continue;
			const Value& object = reader.object(ref->i);
			const std::string name = variant.stringOf("name");
			if (variant.stringOf("className") == "hkaiNavMesh")
			{
				Mesh mesh;
				mesh.tileX = i;
				mesh.tileZ = j;
				mesh.base = {kworld::kWorldMin + kworld::kZoneSize * i, 0.0, kworld::kWorldMin + kworld::kZoneSize * j};
				meshes.push_back(std::move(mesh));
				navs.push_back(&object);
			} else if (name == "Hash") {
				const std::string text = object.stringOf("string");
				std::uint32_t hash = 0;
				try
				{
					hash = static_cast<std::uint32_t>(std::stoul(text, nullptr, 16));
				}
				catch (const std::exception&)
				{
					throw std::runtime_error("bad Hash string '" + text + "'");
				}
				if (stored != nullptr)
				{
					if (infoAwaitsHash && !stored->interiors.empty())
						stored->interiors.back() = hash;
					else if (!infoAwaitsHash) {
						stored->zone = hash;
						stored->hasZone = true;
					}
				}
				infoAwaitsHash = false;
			} else if (name == "Info" && !meshes.empty()) {
				// "hexid;x;y;z": the building's world position; the interior's vertices are axis-aligned from it.
				const std::string text = object.stringOf("string");
				std::vector<std::string> parts(1);
				for (const char c : text)
				{
					if (c == ';')
						parts.emplace_back();
					else
						parts.back() += c;
				}
				if (parts.size() < 4)
					throw std::runtime_error("bad Info string '" + text + "'");
				meshes.back().info = parts[0];
				meshes.back().base = {std::stod(parts[1]), std::stod(parts[2]), std::stod(parts[3])};
				if (stored != nullptr)
					stored->interiors.emplace_back();
				infoAwaitsHash = true;
			}
		}
		for (std::size_t k = 0; k < meshes.size(); ++k)
			fill(meshes[k], *navs[k]);
	}
	catch (const std::exception& error)
	{
		throw std::runtime_error(path + ": " + error.what());
	}
	return meshes;
}

std::vector<Mesh> loadKnm(const std::string& path)
{
	std::ifstream in(path, std::ios::binary);
	const std::vector<char> bytes{ std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
	std::size_t at = 0;
	auto take = [&](auto& value)
	{
		if (at + sizeof(value) > bytes.size())
			throw std::runtime_error(path + ": truncated KNM1 file");
		std::memcpy(&value, bytes.data() + at, sizeof(value));
		at += sizeof(value);
	};
	if (bytes.size() < 16 || std::string_view(bytes.data(), 4) != "KNM1")
		throw std::runtime_error(path + ": not a KNM1 file");
	at = 4;
	std::int32_t zoneX = 0;
	std::int32_t zoneZ = 0;
	std::uint32_t count = 0;
	take(zoneX);
	take(zoneZ);
	take(count);
	std::vector<Mesh> meshes(count);
	for (Mesh& mesh : meshes)
	{
		std::uint16_t nameLength = 0;
		take(nameLength);
		if (at + nameLength > bytes.size())
			throw std::runtime_error(path + ": truncated KNM1 file");
		mesh.info.assign(bytes.data() + at, nameLength);
		at += nameLength;
		for (double& axis : mesh.base)
			take(axis);
		std::uint32_t verts = 0;
		std::uint32_t faces = 0;
		std::uint32_t edges = 0;
		std::uint32_t linked = 0;
		take(verts);
		take(faces);
		take(edges);
		take(linked);
		std::array<double, 3> centre{};
		for (std::uint32_t v = 0; v < verts; ++v)
		{
			std::array<float, 3> p{};
			for (float& axis : p)
				take(axis);
			mesh.vertices.push_back({ p[0], p[1], p[2] });
			for (int k = 0; k < 3; ++k)
				centre[static_cast<std::size_t>(k)] += p[static_cast<std::size_t>(k)] / static_cast<double>(verts);
		}
		for (std::uint32_t f = 0; f < faces; ++f)
		{
			Face face;
			std::uint16_t data = 0;
			take(face.startEdge);
			take(face.numEdges);
			take(data);
			face.data = static_cast<std::int16_t>(data);
			mesh.faces.push_back(face);
		}
		for (std::uint32_t e = 0; e < edges; ++e)
		{
			Edge edge;
			std::array<std::uint8_t, 3> pad{};
			take(edge.a);
			take(edge.b);
			take(edge.oppositeEdge);
			take(edge.oppositeFace);
			take(edge.flags);
			take(pad);
			mesh.edges.push_back(edge);
		}
		for (std::uint32_t l = 0; l < linked; ++l)
		{
			std::uint32_t edge = 0;
			take(edge);
			if (edge < mesh.edges.size())
				mesh.edges[edge].linked = true;
		}
		mesh.tileX = static_cast<int>(std::floor((centre[0] - kworld::kWorldMin) / kworld::kZoneSize));
		mesh.tileZ = static_cast<int>(std::floor((centre[2] - kworld::kWorldMin) / kworld::kZoneSize));
	}
	return meshes;
}

} // namespace knav::native
