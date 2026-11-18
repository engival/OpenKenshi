#include "ksim/save.hpp"
#include "ksim/producers.hpp"

#include "ksim/clock.hpp"
#include "ksim/state.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <fstream>
#include <istream>
#include <iterator>
#include <ostream>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unordered_map>

namespace ksim {

namespace {

constexpr char kMagic[4] = {'K', 'S', 'A', 'V'};
constexpr std::uint32_t kNoEntity = 0xFFFFFFFFu;
constexpr std::uint64_t kFnvOffset = 1469598103934665603ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

template <typename... T, typename F>
void forEachType(state::TypeList<T...>, F&& f)
{
	std::size_t index = 0;
	(f.template operator()<T>(index++), ...);
}

template <typename... T>
constexpr std::size_t typeCount(state::TypeList<T...>) noexcept
{
	return sizeof...(T);
}

static_assert(typeCount(state::SavedComponents{}) <= 64,
	"the presence mask is a u64");
static_assert(typeCount(state::SavedRelations{}) <= 256, "the pair kind is a u8");
static_assert(typeCount(state::SavedSingletons{}) <= 32,
	"the singleton presence mask is a u32");

class Encoder
{
public:
	explicit Encoder(std::vector<std::uint8_t>& bytes,
		const std::unordered_map<flecs::entity_t, std::uint32_t>* index = nullptr)
		: bytes_(bytes)
		, index_(index)
	{
	}

	// The layout being written: the current one unless a migration asks for the previous (writeSave's version).
	std::uint32_t version = kSaveVersion;

	void u8(std::uint8_t value) { bytes_.push_back(value); }

	void u32(std::uint32_t value) { little(value, 4); }

	void u64(std::uint64_t value) { little(value, 8); }

	void str(std::string_view value)
	{
		u32(static_cast<std::uint32_t>(value.size()));
		bytes_.insert(bytes_.end(), value.begin(), value.end());
	}

	void operator()(const std::string& value) { str(value); }

	void operator()(double value) { u64(std::bit_cast<std::uint64_t>(value)); }

	void operator()(const state::EntityRef& ref)
	{
		if (ref.id == 0)
		{
			u32(kNoEntity);
			return;
		}
		const auto found = index_->find(ref.id);
		if (found == index_->end())
			throw SaveError("an EntityRef points at a non-instance entity");
		u32(found->second);
	}

	template <typename T>
		requires std::is_integral_v<T> || std::is_enum_v<T>
	void operator()(T value)
	{
		if constexpr (std::is_enum_v<T>)
			little(static_cast<std::uint64_t>(static_cast<std::underlying_type_t<T>>(value)), sizeof(T));
		else
			little(static_cast<std::uint64_t>(value), sizeof(T));
	}

	// A nested record: its own fields, in order.
	template <typename T>
		requires requires(Encoder& out, const T& value) { T::fields(out, value); }
	void operator()(const T& value)
	{
		T::fields(*this, value);
	}

	// u32 count, then the elements.
	template <typename T>
	void operator()(const std::vector<T>& values)
	{
		u32(static_cast<std::uint32_t>(values.size()));
		for (const T& value : values)
			(*this)(value);
	}

private:
	void little(std::uint64_t value, std::size_t size)
	{
		for (std::size_t i = 0; i < size; ++i)
			bytes_.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
	}

	std::vector<std::uint8_t>& bytes_;
	const std::unordered_map<flecs::entity_t, std::uint32_t>* index_;
};

class Decoder
{
public:
	explicit Decoder(const std::vector<std::uint8_t>& bytes)
		: bytes_(bytes)
	{
	}

	// The layout the file was written with (readSave sets it from the header).
	std::uint32_t version = kSaveVersion;

	void setEntities(const std::vector<flecs::entity_t>* entities) { entities_ = entities; }

	bool atEnd() const noexcept { return pos_ == bytes_.size(); }

	std::uint8_t u8() { return static_cast<std::uint8_t>(little(1)); }

	std::uint32_t u32() { return static_cast<std::uint32_t>(little(4)); }

	std::uint64_t u64() { return little(8); }

	std::string str()
	{
		const std::uint32_t size = u32();
		need(size);
		std::string value(reinterpret_cast<const char*>(bytes_.data() + pos_), size);
		pos_ += size;
		return value;
	}

	void bytes(char* out, std::size_t size)
	{
		need(size);
		std::memcpy(out, bytes_.data() + pos_, size);
		pos_ += size;
	}

	void operator()(std::string& value) { value = str(); }

	void operator()(double& value) { value = std::bit_cast<double>(u64()); }

	void operator()(state::EntityRef& ref)
	{
		const std::uint32_t index = u32();
		if (index == kNoEntity)
		{
			ref.id = 0;
			return;
		}
		ref.id = entity(index);
	}

	template <typename T>
		requires std::is_integral_v<T> || std::is_enum_v<T>
	void operator()(T& value)
	{
		value = static_cast<T>(little(sizeof(T)));
	}

	template <typename T>
		requires requires(Decoder& in, T& value) { T::fields(in, value); }
	void operator()(T& value)
	{
		T::fields(*this, value);
	}

	template <typename T>
	void operator()(std::vector<T>& values)
	{
		const std::uint32_t count = u32();
		values.clear();
		for (std::uint32_t i = 0; i < count; ++i)
		{
			// Every element is at least one byte: a corrupt count runs out
			// of input instead of allocating.
			need(1);
			T value{};
			(*this)(value);
			values.push_back(std::move(value));
		}
	}

	flecs::entity_t entity(std::uint32_t index) const
	{
		if (index >= entities_->size())
			throw SaveError("entity index out of range");
		return (*entities_)[index];
	}

private:
	void need(std::size_t size) const
	{
		if (bytes_.size() - pos_ < size)
			throw SaveError("save is truncated");
	}

	std::uint64_t little(std::size_t size)
	{
		need(size);
		std::uint64_t value = 0;
		for (std::size_t i = 0; i < size; ++i)
			value |= static_cast<std::uint64_t>(bytes_[pos_ + i]) << (8 * i);
		pos_ += size;
		return value;
	}

	const std::vector<std::uint8_t>& bytes_;
	const std::vector<flecs::entity_t>* entities_ = nullptr;
	std::size_t pos_ = 0;
};

struct PairRecord
{
	std::uint32_t source = 0;
	std::uint8_t kind = 0;
	std::uint32_t target = 0;
	flecs::entity_t sourceEntity = 0;
	flecs::entity_t targetEntity = 0;
};

// The metadata block (v36): u32 size, then day u32, hour f32 bits as u32, faction str, leader str, members u16,
// money i64, file size u64 (the whole file's byte count, patched in last). The size field lets later versions append.
constexpr std::size_t kMetaMax = 1u << 16;

// Offset of the block's fileSize field from the start of the block's bytes (after the size field).
std::size_t metaFileSizeOffset(const SaveInfo& info)
{
	return 4 + 4 + 4 + info.factionName.size() + 4 + info.leaderName.size() + 2 + 8;
}

void writeHeader(Encoder& out, const std::vector<std::string>& loadOrder, const SaveInfo& info)
{
	for (const char c : kMagic)
		out.u8(static_cast<std::uint8_t>(c));
	out.u32(out.version);
	out.u32(static_cast<std::uint32_t>(metaFileSizeOffset(info) + 8));
	out.u32(info.day);
	out.u32(std::bit_cast<std::uint32_t>(info.hour));
	out.str(info.factionName);
	out.str(info.leaderName);
	out.u8(static_cast<std::uint8_t>(info.members));
	out.u8(static_cast<std::uint8_t>(info.members >> 8));
	out.u64(static_cast<std::uint64_t>(info.money));
	out.u64(0); // file size, patched by writeSave
	out.u32(static_cast<std::uint32_t>(loadOrder.size()));
	for (const std::string& name : loadOrder)
		out.str(name);
}

// A save older than v105 has no ExtraTown, the last component kind, one older than v102 no StragglerOf before it, and one older than v101 no MemberSlot
// before that (v99 and v100 end at MemberDelta, v101 at MemberSlot, v102 to v104 at StragglerOf): its lists end short, and no entity can carry the bit.
std::size_t componentKinds(std::uint32_t version)
{
	return typeCount(state::SavedComponents{}) - (version < 101 ? 3 : version < 102 ? 2 : version < 105 ? 1 : 0);
}

template <typename List>
void writeKindNames(Encoder& out, List list, std::size_t kinds)
{
	out.u32(static_cast<std::uint32_t>(kinds));
	forEachType(list, [&]<typename T>(std::size_t at)
	{
		if (at < kinds)
			out.str(T::kName);
	});
}

template <typename List>
void readKindNames(Decoder& in, List list, std::string_view what, std::size_t kinds)
{
	const std::uint32_t count = in.u32();
	bool matches = count == kinds;
	if (matches)
	{
		forEachType(list, [&]<typename T>(std::size_t at)
		{
			if (at < kinds && in.str() != T::kName)
				matches = false;
		});
	}
	if (!matches)
		throw SaveError("saved " + std::string(what) + " list differs from this build's"
			" (version " + std::to_string(kSaveVersion) + ")");
}

} // namespace

static std::vector<std::uint8_t> encodeStateAt(const flecs::world& ecs, std::uint32_t version)
{
	std::vector<std::pair<std::uint64_t, flecs::entity_t>> instances;
	ecs.each([&instances](flecs::entity entity, const state::Id& id)
	{
		if (!producerSaved(entity))
			return; // an untouched producer is made again from the data (producers.hpp)
		instances.emplace_back(id.value, entity.id());
	});
	std::sort(instances.begin(), instances.end());
	std::unordered_map<flecs::entity_t, std::uint32_t> index;
	for (std::size_t i = 0; i < instances.size(); ++i)
		index.emplace(instances[i].second, static_cast<std::uint32_t>(i));

	std::vector<std::uint8_t> bytes;
	Encoder out(bytes, &index);
	out.version = version;
	const state::Clock* clock = ecs.get<state::Clock>();
	const state::Ids* ids = ecs.get<state::Ids>();
	out.u64(clock != nullptr ? clock->tick : 0);
	out.u64(ids != nullptr ? ids->next : 1);
	writeKindNames(out, state::SavedComponents{}, componentKinds(version));
	writeKindNames(out, state::SavedRelations{}, typeCount(state::SavedRelations{}));
	writeKindNames(out, state::SavedSingletons{}, typeCount(state::SavedSingletons{}));

	out.u32(static_cast<std::uint32_t>(instances.size()));
	for (const auto& [id, raw] : instances)
	{
		const flecs::entity entity(ecs.c_ptr(), raw);
		out.u64(id);
		std::uint64_t mask = 0;
		forEachType(state::SavedComponents{}, [&]<typename T>(std::size_t bit)
		{
			if (bit < componentKinds(version) && entity.get<T>() != nullptr)
				mask |= 1ull << bit;
		});
		out.u64(mask);
		forEachType(state::SavedComponents{}, [&]<typename T>(std::size_t bit)
		{
			if (const T* value = entity.get<T>(); value != nullptr && bit < componentKinds(version))
				T::fields(out, *value);
		});
	}

	std::vector<PairRecord> pairs;
	for (std::size_t i = 0; i < instances.size(); ++i)
	{
		const flecs::entity entity(ecs.c_ptr(), instances[i].second);
		forEachType(state::SavedRelations{}, [&]<typename R>(std::size_t kind)
		{
			entity.each<R>([&](flecs::entity target)
			{
				const auto found = index.find(target.id());
				if (found == index.end())
					throw SaveError(std::string(R::kName)
						+ " pair targets a non-instance entity");
				pairs.push_back(PairRecord{static_cast<std::uint32_t>(i),
					static_cast<std::uint8_t>(kind), found->second,
					entity.id(), target.id()});
			});
		});
	}
	std::sort(pairs.begin(), pairs.end(),
		[](const PairRecord& left, const PairRecord& right)
		{
			return std::tie(left.source, left.kind, left.target)
				< std::tie(right.source, right.kind, right.target);
		});
	out.u32(static_cast<std::uint32_t>(pairs.size()));
	for (const PairRecord& pair : pairs)
	{
		out.u32(pair.source);
		out.u8(pair.kind);
		out.u32(pair.target);
		forEachType(state::SavedRelations{}, [&]<typename R>(std::size_t kind)
		{
			if constexpr (!std::is_empty_v<R>)
			{
				if (kind == pair.kind)
				{
					const flecs::entity source(ecs.c_ptr(), pair.sourceEntity);
					R::fields(out, *source.get<R>(pair.targetEntity));
				}
			}
		});
	}

	std::uint32_t singletons = 0;
	forEachType(state::SavedSingletons{}, [&]<typename T>(std::size_t bit)
	{
		if (ecs.get<T>() != nullptr)
			singletons |= 1u << bit;
	});
	out.u32(singletons);
	forEachType(state::SavedSingletons{}, [&]<typename T>(std::size_t)
	{
		if (const T* value = ecs.get<T>())
			T::fields(out, *value);
	});
	return bytes;
}

std::vector<std::uint8_t> encodeState(const flecs::world& ecs)
{
	return encodeStateAt(ecs, kSaveVersion);
}

std::uint64_t stateHash(const flecs::world& ecs, std::uint32_t version)
{
	std::uint64_t hash = kFnvOffset;
	for (const std::uint8_t byte : encodeStateAt(ecs, version))
	{
		hash ^= byte;
		hash *= kFnvPrime;
	}
	return hash;
}

void writeSave(std::ostream& out, const flecs::world& ecs,
	const std::vector<std::string>& loadOrder, const std::vector<Command>& pending, std::uint32_t version)
{
	std::vector<std::uint8_t> bytes;
	Encoder header(bytes);
	header.version = version;
	const SaveInfo info = describeState(ecs);
	writeHeader(header, loadOrder, info);
	const std::size_t sizeAt = 12 + metaFileSizeOffset(info);
	const std::vector<std::uint8_t> body = encodeStateAt(ecs, version);
	bytes.insert(bytes.end(), body.begin(), body.end());
	header(pending);
	for (std::size_t i = 0; i < 8; ++i)
		bytes[sizeAt + i] = static_cast<std::uint8_t>(static_cast<std::uint64_t>(bytes.size()) >> (8 * i));
	out.write(reinterpret_cast<const char*>(bytes.data()),
		static_cast<std::streamsize>(bytes.size()));
	if (!out)
		throw SaveError("write failed");
}

void writeSaveFile(const std::string& path, const flecs::world& ecs,
	const std::vector<std::string>& loadOrder, const std::vector<Command>& pending, std::uint32_t version)
{
	std::ofstream out(path, std::ios::binary | std::ios::trunc);
	if (!out)
		throw SaveError("cannot open " + path + " for writing");
	writeSave(out, ecs, loadOrder, pending, version);
}

SaveInfo describeState(const flecs::world& ecs)
{
	SaveInfo info;
	info.version = kSaveVersion;
	info.readable = true;
	const std::uint64_t tick = ecs.get<state::Clock>() != nullptr ? ecs.get<state::Clock>()->tick : 0;
	info.day = static_cast<std::uint32_t>(gameTimeAt(tick).day);
	info.hour = static_cast<float>(std::fmod(static_cast<double>(tick) / kTicksPerGameMinute, 1440.0) / 60.0);

	// The player faction: the lowest Id that holds a state::PlayerFaction (as the PLAYER_SQUADS record picks it).
	std::uint64_t factionId = 0;
	state::PlayerFaction faction;
	ecs.each([&](const state::Id& id, const state::PlayerFaction& value)
	{
		if (factionId == 0 || id.value < factionId)
		{
			factionId = id.value;
			faction = value;
		}
	});
	if (factionId == 0)
		return info;
	info.factionName = faction.name;
	info.money = faction.money;
	std::unordered_map<std::uint64_t, std::vector<std::uint64_t>> squadMembers;
	ecs.each([&](const state::Id& id, const state::PlayerSquad& squad)
	{
		squadMembers[id.value] = squad.members;
	});
	std::unordered_map<std::uint64_t, const state::Fighter*> fighters;
	ecs.each([&](const state::Id& id, const state::Fighter& fighter)
	{
		fighters[id.value] = &fighter;
	});
	std::uint64_t leader = 0;
	std::uint64_t members = 0;
	for (const std::uint64_t squadId : faction.squads)
	{
		const auto squad = squadMembers.find(squadId);
		if (squad == squadMembers.end())
			continue;
		if (leader == 0 && !squad->second.empty())
			leader = squad->second.front();
		for (const std::uint64_t member : squad->second)
		{
			const auto fighter = fighters.find(member);
			if (fighter == fighters.end() || !fighter->second->dead)
				++members;
		}
	}
	const auto lead = fighters.find(leader);
	if (lead != fighters.end())
		info.leaderName = lead->second->name;
	info.members = static_cast<std::uint16_t>(std::min<std::uint64_t>(members, 0xFFFF));
	return info;
}

SaveInfo readSaveInfo(std::istream& in, std::uint64_t fileSize)
{
	SaveInfo info;
	try
	{
		std::vector<std::uint8_t> head(12);
		in.read(reinterpret_cast<char*>(head.data()), static_cast<std::streamsize>(head.size()));
		if (in.gcount() < 8 || std::memcmp(head.data(), kMagic, sizeof(kMagic)) != 0)
			return info;
		Decoder fixed(head);
		char magic[4];
		fixed.bytes(magic, sizeof(magic));
		info.version = fixed.u32();
		if ((info.version != kSaveVersion && info.version != kSaveVersion - 1) || in.gcount() < 12)
			return info; // an older or newer layout: nothing is promised about its head
		const std::uint32_t size = fixed.u32();
		if (size < 4 + 4 + 4 + 4 + 2 + 8 + 8 || size > kMetaMax)
			return info;
		std::vector<std::uint8_t> block(size);
		in.read(reinterpret_cast<char*>(block.data()), static_cast<std::streamsize>(block.size()));
		if (in.gcount() != static_cast<std::streamsize>(block.size()))
			return info;
		Decoder meta(block);
		info.day = meta.u32();
		info.hour = std::bit_cast<float>(meta.u32());
		info.factionName = meta.str();
		info.leaderName = meta.str();
		const std::uint8_t low = meta.u8();
		info.members = static_cast<std::uint16_t>(low | (meta.u8() << 8));
		info.money = static_cast<std::int64_t>(meta.u64());
		info.readable = meta.u64() == fileSize;
		if (!info.readable)
		{
			const std::uint32_t version = info.version;
			info = SaveInfo{};
			info.version = version; // a damaged head promises nothing else
		}
	} catch (const SaveError&) {
		info = SaveInfo{};
		info.version = 0;
	}
	return info;
}

SaveLoadResult readSave(std::istream& in, flecs::world& ecs,
	const std::vector<std::string>& expectedLoadOrder, bool force)
{
	const std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(in),
		std::istreambuf_iterator<char>()};
	Decoder save(bytes);

	char magic[4] = {};
	save.bytes(magic, sizeof(magic));
	if (std::memcmp(magic, kMagic, sizeof(magic)) != 0)
		throw SaveError("not a runtime save (bad magic)");
	const std::uint32_t version = save.u32();
	// The previous version stays readable for the migration (its run bool loads as a speed mode, RUNTIME_SAVE.md).
	if (version != kSaveVersion && version != kSaveVersion - 1)
		throw SaveError("save version " + std::to_string(version)
			+ " is not supported (this build reads " + std::to_string(kSaveVersion) + ")");
	save.version = version;
	const std::uint32_t metaSize = save.u32();
	if (metaSize > kMetaMax)
		throw SaveError("save metadata block is too large");
	for (std::uint32_t i = 0; i < metaSize; ++i)
		save.u8(); // the metadata block is for listings; the body below is the state

	SaveLoadResult result;
	const std::uint32_t files = save.u32();
	for (std::uint32_t i = 0; i < files; ++i)
		result.savedLoadOrder.push_back(save.str());
	result.loadOrderMatches = result.savedLoadOrder == expectedLoadOrder;
	if (!result.loadOrderMatches && !force)
		throw SaveError("save was made with a different game-data load order");

	state::registerComponents(ecs);
	if (ecs.count<state::Id>() != 0)
		throw SaveError("readSave needs a world without instances");

	const std::uint64_t tick = save.u64();
	const std::uint64_t nextId = save.u64();
	readKindNames(save, state::SavedComponents{}, "component", componentKinds(version));
	readKindNames(save, state::SavedRelations{}, "relationship", typeCount(state::SavedRelations{}));
	readKindNames(save, state::SavedSingletons{}, "singleton", typeCount(state::SavedSingletons{}));

	// Every entity first, so references can point forward.
	const std::uint32_t count = save.u32();
	std::vector<flecs::entity_t> entities;
	entities.reserve(count);
	for (std::uint32_t i = 0; i < count; ++i)
		entities.push_back(ecs.entity().id());
	save.setEntities(&entities);

	for (const flecs::entity_t raw : entities)
	{
		flecs::entity entity(ecs.c_ptr(), raw);
		entity.set<state::Id>(state::Id{save.u64()});
		const std::uint64_t mask = save.u64();
		forEachType(state::SavedComponents{}, [&]<typename T>(std::size_t bit)
		{
			if ((mask & (1ull << bit)) == 0)
				return;
			T value;
			T::fields(save, value);
			entity.set<T>(std::move(value));
		});
		if ((mask >> typeCount(state::SavedComponents{})) != 0)
			throw SaveError("unknown component bit in a presence mask");
	}

	const std::uint32_t pairs = save.u32();
	for (std::uint32_t i = 0; i < pairs; ++i)
	{
		flecs::entity source(ecs.c_ptr(), save.entity(save.u32()));
		const std::uint8_t kind = save.u8();
		const flecs::entity_t target = save.entity(save.u32());
		if (kind >= typeCount(state::SavedRelations{}))
			throw SaveError("unknown relationship kind");
		forEachType(state::SavedRelations{}, [&]<typename R>(std::size_t index)
		{
			if (index != kind)
				return;
			if constexpr (std::is_empty_v<R>)
			{
				source.add<R>(target);
			} else {
				R value;
				R::fields(save, value);
				source.set<R>(target, value);
			}
		});
	}

	const std::uint32_t singletons = save.u32();
	if ((singletons >> typeCount(state::SavedSingletons{})) != 0)
		throw SaveError("unknown singleton bit in a presence mask");
	forEachType(state::SavedSingletons{}, [&]<typename T>(std::size_t bit)
	{
		if ((singletons & (1u << bit)) == 0)
		{
			ecs.remove<T>();
			return;
		}
		T value;
		T::fields(save, value);
		ecs.set<T>(std::move(value));
	});

	save(result.pending);
	if (!save.atEnd())
		throw SaveError("trailing bytes after the pending commands");

	ecs.set<state::Clock>(state::Clock{tick});
	ecs.set<state::Ids>(state::Ids{nextId});
	return result;
}

SaveLoadResult readSaveFile(const std::string& path, flecs::world& ecs,
	const std::vector<std::string>& expectedLoadOrder, bool force)
{
	std::ifstream in(path, std::ios::binary);
	if (!in)
		throw SaveError("cannot open " + path);
	return readSave(in, ecs, expectedLoadOrder, force);
}

} // namespace ksim
