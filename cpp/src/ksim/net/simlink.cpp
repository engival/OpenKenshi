#include "ksim/nav/stand_points.hpp"
#include "ksim/net/simlink.hpp"
#include "ksim/economy/inventory_grid.hpp"

#include "ksim/world/beams.hpp"
#include "ksim/core/clock.hpp"
#include "ksim/nav/navigator.hpp"
#include "ksim/core/rng.hpp"
#include "ksim/ai/sensor.hpp"
#include "ksim/net/simlink_saves.hpp"
#include "ksim/combat/stealth_ko.hpp"
#include "ksim/core/state.hpp"

#include "knav/navmesh.hpp"

#include "kworld/coords.hpp"
#include "kworld/gamedata.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <optional>
#include <ostream>
#include <set>
#include <unordered_set>
#include <sstream>
#include <stdexcept>

namespace ksim::simlink {

namespace {

constexpr double kBubbleHalf = 390.0; // 340 (ORIGINAL_AI.md) + 50 for the 100-unit snap
constexpr double kBubbleGrid = 100.0;
constexpr double kDefaultInterest = 1000.0;
constexpr std::size_t kNavTileBudget = 256u << 10; // bytes of tile data per tick (SIMLINK.md section 12)
constexpr std::uint64_t kSlowIntervalTicks = 20; // CLOCK and SQUADS: 1 Hz
constexpr std::uint64_t kMedicalIntervalTicks = 8; // CHAR_MEDICAL: 2.5 Hz
constexpr std::uint64_t kCharStatsIntervalTicks = 20; // CHAR_STATS: at most 1 Hz per character
// move_diverged log (the click vs the route's end): a snap farther than these is worth a line (OURS, diagnostics only).
constexpr double kMoveDivergeXZ = 20.0;
constexpr double kMoveDivergeY = 10.0;

const char* typeName(std::uint16_t type)
{
	switch (type)
	{
	case kHello: return "HELLO";
	case kStrtab: return "STRTAB_APPEND";
	case kError: return "ERROR";
	case kSnapBegin: return "SNAPSHOT_BEGIN";
	case kSnapEnd: return "SNAPSHOT_END";
	case kAppearance: return "CHAR_APPEARANCE";
	case kDynamic: return "CHAR_DYNAMIC";
	case kMedical: return "CHAR_MEDICAL";
	case kRoute: return "CHAR_ROUTE";
	case kInteractable: return "INTERACTABLE";
	case kInventory: return "INVENTORY";
	case kItemQuality: return "ITEM_QUALITY";
	case kItemGrid: return "ITEM_GRID";
	case kItemLock: return "ITEM_LOCK";
	case kItemStats: return "ITEM_STATS";
	case kMenuEntries: return "MENU_ENTRIES";
	case kCameraStart: return "CAMERA_START";
	case kCages: return "CAGES";
	case kProfile: return "PROFILE";
	case kCharStats: return "CHAR_STATS";
	case kNavParams: return "NAV_PARAMS";
	case kNavTile: return "NAV_TILE";
	case kNavPolyFlags: return "NAV_POLY_FLAGS";
	case kSquads: return "SQUADS";
	case kPlayerSquads: return "PLAYER_SQUADS";
	case kClock: return "CLOCK";
	case kWeather: return "WEATHER";
	case kBeams: return "BEAMS";
	case kSeenBy: return "SEEN_BY";
	case kDetectedBy: return "DETECTED_BY";
	case kBounds: return "DETAIL_BOUNDS";
	case kEvent: return "EVENT";
	case kSaves: return "SAVES";
	case kStarts: return "STARTS";
	case kJobs: return "JOBS";
	case kProspect: return "PROSPECT";
	case kUnlocks: return "UNLOCKS";
	case kResearchState: return "RESEARCH_STATE";
	case kDiscovered: return "DISCOVERED";
	case kFactionRelations: return "FACTION_RELATIONS";
	case kTownState: return "TOWN_STATE";
	case kGameOptions: return "GAME_OPTIONS";
	case kTaskProgress: return "TASK_PROGRESS";
	case kTrade: return "TRADE";
	case kBuilding: return "BUILDING";
	case kBuildingGrid: return "BUILDING_GRID";
	case kNewGameResult: return "NEW_GAME_RESULT";
	case kNewGame: return "NEW_GAME";
	case kOrder: return "ORDER";
	}
	return "UNKNOWN";
}

// Discards everything. The tick's records are written once into this (interning every string
// they use), then the string table is flushed, then the real pass runs.
class NullWriter : public RecordWriter
{
public:
	void begin(std::uint16_t) override {}
	void end() override {}
	void u8(const char*, std::uint8_t) override {}
	void u16(const char*, std::uint16_t) override {}
	void u32(const char*, std::uint32_t) override {}
	void u64(const char*, std::uint64_t) override {}
	void i16(const char*, std::int16_t) override {}
	void f32(const char*, float) override {}
	void f64(const char*, double) override {}
	void str(const char*, const std::string&) override {}
	void strid(const char*, std::uint32_t) override {}
	void bytes(const char*, const std::uint8_t*, std::size_t) override {}
	void list(const char*, std::size_t, int) override {}
	void item() override {}
	void listEnd() override {}
};

// SIMLINK.md section 6, CHAR_DYNAMIC: "yaw radians"; the sim's convention (0 = +z, towards +x).
float f(double v)
{
	return static_cast<float>(v);
}

// ARMOUR `slot` (AttachSlot, fcs_enums.def) -> the wire section; -1 = no section.
int sectionOfSlot(int slot)
{
	switch (slot)
	{
	case 0: return 9; // weapon: the hand
	case 1: return 0; // back
	case 3: return 3; // hat: head
	case 5: return 5; // body: armour
	case 6: return 6; // legs
	case 8: return 4; // shirt
	case 9: return 7; // boots
	case 12: return 8; // backpack
	case 14: return 2; // belt: hip
	default: return -1; // hair, eyes, none, gloves, neck, beard
	}
}

double legPercent(const state::Fighter& fighter, const char* name)
{
	for (const state::FighterPart& p : fighter.parts)
	{
		if (p.name == name)
		{
			// MedicalSystem 140646860 -> AnimationClassHuman 14051ccb0: no bonus term, clamped
			// to <= 100 (LOCOMOTION.md "Leg values"); negative stays negative.
			const double healthy = p.maxHealth - p.wear;
			return healthy > 0.0 ? std::min(100.0, 100.0 * (p.health - p.stun) / healthy) : 100.0;
		}
	}
	return 100.0;
}

bool finite(float v)
{
	return std::isfinite(v);
}

std::string joinIds(const std::vector<std::uint64_t>& ids)
{
	std::string text;
	for (const std::uint64_t id : ids)
		text += (text.empty() ? "" : ",") + std::to_string(id);
	return text;
}

} // namespace

// ---- data hash ----

// ---- string table ----

std::uint32_t StringTable::intern(std::uint8_t kind, const std::string& text)
{
	const auto key = std::make_pair(kind, text);
	const auto found = index_.find(key);
	if (found != index_.end())
		return found->second;
	entries_.push_back(Entry{kind, text});
	const auto id = static_cast<std::uint32_t>(entries_.size());
	index_.emplace(key, id);
	return id;
}

const std::string& StringTable::text(std::uint32_t id) const
{
	static const std::string empty;
	return id >= 1 && id <= entries_.size() ? entries_[id - 1].text : empty;
}

// ---- writers ----

void BinaryWriter::begin(std::uint16_t type)
{
	start_ = out_.size();
	put<std::uint32_t>(0);
	put(type);
}

void BinaryWriter::end()
{
	const auto len = static_cast<std::uint32_t>(out_.size() - start_ - 4);
	std::memcpy(out_.data() + start_, &len, 4);
}

void BinaryWriter::str(const char*, const std::string& v)
{
	if (v.size() > 0xffff)
		throw std::runtime_error("simlink: string too long");
	put(static_cast<std::uint16_t>(v.size()));
	out_.insert(out_.end(), v.begin(), v.end());
}

void BinaryWriter::list(const char*, std::size_t n, int countBits)
{
	if (countBits == 8)
		put(static_cast<std::uint8_t>(n));
	else if (countBits == 16)
		put(static_cast<std::uint16_t>(n));
	else
		put(static_cast<std::uint32_t>(n));
}

void TextWriter::begin(std::uint16_t type)
{
	out_ << typeName(type);
	inList_ = false;
}

void TextWriter::end()
{
	out_ << '\n';
}

void TextWriter::field(const char* key, const std::string& value)
{
	if (inList_)
	{
		out_ << (firstInEntry_ ? "" : ",") << key << '=' << value;
		firstInEntry_ = false;
	} else {
		out_ << ' ' << key << '=' << value;
	}
}

void TextWriter::f32(const char* k, float v)
{
	char text[48];
	std::snprintf(text, sizeof(text), "%.3f", static_cast<double>(v));
	field(k, text);
}

void TextWriter::f64(const char* k, double v)
{
	char text[48];
	std::snprintf(text, sizeof(text), "%.3f", v);
	field(k, text);
}

void TextWriter::str(const char* k, const std::string& v)
{
	field(k, "\"" + v + "\"");
}

void TextWriter::strid(const char* k, std::uint32_t id)
{
	std::string text = std::to_string(id);
	if (table_ != nullptr && id != 0)
		text += "(\"" + table_->text(id) + "\")";
	field(k, text);
}

void TextWriter::list(const char* k, std::size_t n, int)
{
	out_ << ' ' << k << '=' << n << '[';
	inList_ = true;
	firstItem_ = true;
	firstInEntry_ = true;
}

void TextWriter::item()
{
	if (!firstItem_)
		out_ << "; ";
	firstItem_ = false;
	firstInEntry_ = true;
}

void TextWriter::listEnd()
{
	out_ << ']';
	inList_ = false;
}

// ---- session ----

Session::Session(Simulation& sim, ServerContext& context)
	: sim_(sim)
	, ctx_(context)
	, key_(context.nextSessionKey++)
{
	ctx_.sessions.push_back(this);
	sim_.setCombatObserver([this](const CombatEvent& e)
	{
		const CombatArena* arena = sim_.combat();
		const state::Fighter* victim = arena != nullptr ? arena->find(e.who) : nullptr;
		if (victim == nullptr)
			return;
		Observed o;
		o.who = e.who;
		o.other = e.other;
		o.pos[0] = f(victim->x);
		o.pos[1] = sim_.navigation() != nullptr ? f(victim->y) : 0.0f;
		o.pos[2] = f(victim->z);
		switch (e.kind)
		{
		case CombatEvent::Kind::Hit:
		case CombatEvent::Kind::Resolve:
		{
			if (e.kind == CombatEvent::Kind::Resolve && e.code != 0 && e.code != 5)
				return; // a landed hit is reported by Kind::Hit
			o.kind = 1;
			o.result = e.kind == CombatEvent::Kind::Hit ? 0 : (e.code == 5 ? 1 : 2);
			if (e.kind == CombatEvent::Kind::Hit)
			{
				for (int i = 0; i < 4; ++i)
					o.damage[i] = f(e.dmg[i]);
				o.absorbed[0] = f(e.absorbed[0]);
				o.absorbed[1] = f(e.absorbed[1]);
				o.part = e.part;
			}
			if (const state::Fighter* attacker = arena->find(e.other))
			{
				const double angle = std::atan2(victim->z - attacker->z, victim->x - attacker->x);
				const double turns = angle / (2.0 * 3.14159265358979323846);
				o.dir = static_cast<std::uint8_t>(static_cast<long>(std::lround(turns * 256.0)) & 0xff);
			}
			break;
		}
		case CombatEvent::Kind::Knockout:
			o.kind = 3;
			break;
		case CombatEvent::Kind::Death:
			o.kind = 5;
			break;
		case CombatEvent::Kind::Ragdoll:
		{
			// EVENT 48 (task BG, PHYSICS.md 2-3): the ragdoll start. The direction is the hit's attacker -> victim vector in
			// the world frame (unit); a collapse (no attacker) sends (0, 0, 0) and strength 0.
			o.kind = 48;
			o.result = static_cast<std::uint8_t>(e.code);
			o.strength = f(e.a);
			o.seed = e.seed;
			o.part = e.part;
			if (const state::Fighter* attacker = arena->find(e.other))
			{
				const double dx = victim->x - attacker->x;
				const double dz = victim->z - attacker->z;
				const double len = std::hypot(dx, dz);
				if (len > 1e-9)
				{
					o.ragdollDir[0] = f(dx / len);
					o.ragdollDir[2] = f(dz / len);
				}
			}
			break;
		}
		case CombatEvent::Kind::LimbSevered:
			o.kind = 47;
			o.part = e.part;
			break;
		case CombatEvent::Kind::DriveBlocked:
			o.kind = 8;
			break;
		case CombatEvent::Kind::Noticed:
		{
			// EVENT 15 for the client's barks: a hostile observer became aware of a character, and one of the two is the player's.
			const state::Fighter* target = arena->find(e.other);
			if (target == nullptr || target->team == victim->team || !(victim->playerControlled || target->playerControlled))
				return;
			o.kind = 15;
			break;
		}
		case CombatEvent::Kind::PositionCorrected:
		{
			o.kind = 12;
			o.damage[0] = f(e.a);
			// The event repeats every tick while a fighter stays off the mesh (a distance -1 one cannot even be moved): the log says it once per
			// episode (a gap of a tick or more ends it), the wire carries them all.
			std::uint64_t& lastLogged = offMeshLogged_[e.who];
			const bool again = lastLogged != 0 && sim_.clockTick() <= lastLogged + 1;
			lastLogged = sim_.clockTick();
			if (ctx_.log && !again)
			{
				char text[160];
				std::snprintf(text, sizeof(text), "position_corrected id=%llu distance=%.3f at tick %llu",
					static_cast<unsigned long long>(e.who), e.a, static_cast<unsigned long long>(sim_.clockTick()));
				ctx_.log(text);
			}
			break;
		}
		case CombatEvent::Kind::Shot:
			// EVENT 49 (approved 2026-11-10): routed like event 1 (by the target), one per fired shot.
			o.kind = 49;
			o.result = static_cast<std::uint8_t>(e.code);
			o.part = e.part;
			o.turret = e.turret;
			for (int i = 0; i < 3; ++i)
			{
				o.pos[i] = f(e.dmg[i]);
				o.shotTo[i] = f(e.dmg[3 + i]);
			}
			break;
		case CombatEvent::Kind::StealthKo:
			// EVENT 50 (approved 2026-11-10): routed like event 1 (by the victim), once per attempt, at the roll or at the abort.
			o.kind = 50;
			o.result = static_cast<std::uint8_t>(e.code);
			o.chance = f(e.a);
			break;
		default:
			return;
		}
		observed_.push_back(std::move(o));
	});
	// Reply menus (DIALOGUE.md 13, SIMLINK REPLY_MENU 43 / CONVERSATION_END 44) for a player-controlled target: an open one freezes the
	// sim while this session shows it (V1 Freeze).
	sim_.setReplyMenuObserver([this](const Simulation::ReplyMenuEvent& e)
	{
		const state::Fighter* target = sim_.combat() != nullptr ? sim_.combat()->find(e.target) : nullptr;
		if (!e.end && (target == nullptr || !target->playerControlled))
			return;
		Observed o;
		o.kind = e.end ? 44 : 43;
		o.who = e.speaker;
		o.other = e.target;
		o.seq = e.menuSeq;
		o.result = e.end ? e.reason : static_cast<std::uint8_t>((e.resend ? 1 : 0) | 2 | (ctx_.dialogueFreeze ? 4 : 0)); // 2 dismissible (V1 always), 4 freezes
		o.options = e.options;
		if (e.end)
			releaseMenu(e.speaker);
		else
			holdMenu(e.speaker);
		observed_.push_back(std::move(o));
	});
	sim_.setTalkObserver([this](std::uint64_t actor, std::uint64_t, std::uint32_t seq)
	{
		const state::Fighter* talker = sim_.combat() != nullptr ? sim_.combat()->find(actor) : nullptr;
		if (talker != nullptr && talker->playerControlled)
			lateRejects_.emplace_back(seq, "nothing_to_say");
	});
	sim_.setWindowObserver([this](std::uint64_t speaker, std::uint64_t target, std::uint8_t kind)
	{
		const state::Fighter* who = sim_.combat() != nullptr ? sim_.combat()->find(target) : nullptr;
		if (kind == 1 && who != nullptr && who->playerControlled)
			openTrade(target, speaker);
	});
	sim_.setSpeechObserver([this](const Simulation::SpeechEvent& e)
	{
		Observed o;
		o.kind = e.end ? 42 : 41;
		o.who = e.speaker;
		o.other = e.target;
		o.dialogue = e.dialogue;
		o.line = e.line;
		o.text = e.text;
		o.damage[0] = f(e.seconds);
		o.result = e.flags;
		observed_.push_back(std::move(o));
	});
	sim_.setCrimeObserver([this](const Simulation::CrimeEvent& e)
	{
		// EVENT 51 / 52 (approved 2026-11-16): the player's characters only (the thief; the criminal).
		const state::Fighter* subject = sim_.combat() != nullptr ? sim_.combat()->find(e.kind == Simulation::CrimeEvent::Kind::CaughtStealing ? e.who : e.other) : nullptr;
		if (subject == nullptr || !subject->playerControlled)
			return;
		Observed o;
		o.who = e.who;
		o.other = e.other;
		o.result = e.cause;
		o.turret = e.container; // 51: the container, character or door
		if (e.kind == Simulation::CrimeEvent::Kind::CaughtStealing)
		{
			o.kind = 51;
		} else {
			o.kind = 52;
			o.part = e.faction;
			o.pos[0] = f(e.x);
			o.pos[1] = sim_.navigation() != nullptr && sim_.combat()->find(e.who) != nullptr ? f(sim_.combat()->find(e.who)->y) : 0.0f;
			o.pos[2] = f(e.z);
		}
		observed_.push_back(std::move(o));
	});
	sim_.setInteractObserver([this](const InteractEvent& e)
	{
		Observed o;
		o.who = e.who;
		o.other = e.target;
		if (e.kind == InteractEvent::Kind::Theft)
		{
			o.kind = 14;
			o.part = e.item; // the owner's FACTION record
		} else if (e.kind == InteractEvent::Kind::LockRoll) {
			const state::Fighter* roller = sim_.combat() != nullptr ? sim_.combat()->find(e.who) : nullptr;
			if (roller == nullptr || !roller->playerControlled)
				return;
			o.kind = 20;
			o.result = e.result;
		} else if (e.kind == InteractEvent::Kind::PickedUp || e.kind == InteractEvent::Kind::Dropped) {
			o.kind = e.kind == InteractEvent::Kind::PickedUp ? 7 : 6;
			o.part = e.item;
			o.pos[0] = f(e.x);
			o.pos[1] = sim_.navigation() != nullptr ? f(e.y) : 0.0f;
			o.pos[2] = f(e.z);
		} else {
			// EVENT 13 is for a player-controlled actor only (approved 2026-11-19, k16.6.427): the client words it as the player's own.
			const state::Fighter* actor = sim_.combat() != nullptr ? sim_.combat()->find(e.who) : nullptr;
			if (actor == nullptr || !actor->playerControlled)
				return;
			o.kind = 13;
			o.action = e.action == static_cast<std::uint8_t>(InteractAction::Close) && (e.flags & kInteractFlagLock) != 0 ? kLockDoorAction : e.action; // the lock order is action 21 on the wire
			o.result = e.result;
			// Loot open that arrived: the looter now keeps the body's (or container's) contents current (frame feed).
			if (e.result == InteractEvent::kDone && e.action == static_cast<std::uint8_t>(InteractAction::Loot)
				&& (e.flags & kInteractFlagOpen) != 0)
			{
				const state::Fighter* looter = sim_.combat() != nullptr ? sim_.combat()->find(e.who) : nullptr;
				if (looter != nullptr && looter->playerControlled)
				{
					openLoots_[e.target].insert(e.who);
					closeTrade(e.who, 1); // a new loot window of the same character closes its trade (MQ11)
				}
			}
			// Talk that arrived: a merchant's character opens a trade for the talker (trade.hpp, SIMLINK TRADE 53). With the dialogue engine on
			// the merchant talks instead and the window observer opens it from the trade reply.
			if (e.result == InteractEvent::kDone && e.action == static_cast<std::uint8_t>(InteractAction::Talk) && sim_.dialogue() == nullptr)
			{
				const state::Fighter* talker = sim_.combat() != nullptr ? sim_.combat()->find(e.who) : nullptr;
				if (talker != nullptr && talker->playerControlled)
					openTrade(e.who, e.target);
			}
		}
		observed_.push_back(std::move(o));
	});
	sim_.addProspectObserver(this, [this](const std::shared_ptr<const prospect::Result>& result)
	{
		prospectDue_ = result; // a singleton on the client: the latest finisher wins, as in the exe
	});
	sim_.addNewGameObserver(this, [this](const NewGameOutcome& outcome)
	{
		if (pendingNewGame_.erase(outcome.ticket) != 0)
			newGameOutcomes_.push_back(outcome);
	});
	sim_.setDismissObserver([this](const DismissEvent& e)
	{
		// The character now belongs to another faction: its appearance carries that faction (CHAR_APPEARANCE faction strid).
		ctx_.looks[e.character].factionSid = e.factionSid == "nofac" ? std::string() : e.factionSid;
		Observed o;
		o.kind = 18;
		o.who = e.character;
		o.other = e.squad;
		o.part = e.factionName;
		observed_.push_back(std::move(o));
	});
}

Session::~Session()
{
	ctx_.sessions.erase(std::remove(ctx_.sessions.begin(), ctx_.sessions.end(), this), ctx_.sessions.end());
	if (follow_ != 0)
	{
		// The follow dies with the session: a logged command, so a replay drops it at the same tick.
		Command command = Command::setCameraFollow(key_, 0);
		command.tick = sim_.clockTick() + 1;
		sim_.enqueue(command);
		ctx_.orderLog.push_back(OrderLogEntry{command.tick, 0, 52, true, command, "set_camera_follow id=0 (session dropped)"});
	}
	sim_.setCombatObserver({});
	sim_.setInteractObserver({});
	sim_.setSpeechObserver({});
	sim_.setReplyMenuObserver({});
	sim_.setTalkObserver({});
	sim_.setWindowObserver({});
	ctx_.menuHolds.clear(); // a gone session's menus run on under Hold (DIALOGUE.md 13.2); the server's grace ends them
	sim_.setDismissObserver({});
	sim_.removeNewGameObserver(this);
	sim_.removeProspectObserver(this);
}

void Session::begin(RecordWriter& out)
{
	out.begin(kHello);
	out.u32("magic", kMagic);
	out.u16("protocol", kProtocol);
	out.u8("role", 1);
	out.u64("dataHash", ctx_.dataHash);
	out.str("name", ctx_.name);
	out.end();
}

void Session::refuse(RecordWriter& out, const std::string& text)
{
	ready_ = false;
	out.begin(kError);
	out.str("text", text);
	out.end();
}

bool Session::onFrame(std::uint16_t type, const std::uint8_t* payload, std::size_t size, RecordWriter& out)
{
	if (type == kError)
		return false; // the client is closing; nothing to answer
	if (!gotHello_)
	{
		if (type != kHello)
		{
			refuse(out, "expected HELLO as the first frame, got type " + std::to_string(type));
			return false;
		}
		Reader r{payload, size};
		const std::uint32_t magic = r.u32();
		const std::uint16_t protocol = r.u16();
		const std::uint8_t role = r.u8();
		const std::uint64_t hash = r.u64();
		r.str();
		if (r.bad || r.at != size || magic != kMagic || role != 0)
		{
			refuse(out, "malformed HELLO");
			return false;
		}
		if (protocol != kProtocol)
		{
			refuse(out, "protocol mismatch: server " + std::to_string(kProtocol) + ", client " + std::to_string(protocol));
			return false;
		}
		if (hash != ctx_.dataHash)
		{
			char text[160];
			std::snprintf(text, sizeof(text), "data hash mismatch: server %016llx, client %016llx (different load order or game files)",
				static_cast<unsigned long long>(ctx_.dataHash), static_cast<unsigned long long>(hash));
			refuse(out, text);
			return false;
		}
		gotHello_ = true;
		ready_ = true;
		observed_.clear(); // events before the handshake are not the client's
		return true;
	}
	if (type == kPing)
	{
		if (size == 0)
			return true; // keepalive; the transport noted the frame, nothing to answer
		refuse(out, "PING carries no payload");
		return false;
	}
	if (type == kOrder)
	{
		// Drive orders (kind 20, after the u32 clientSeq) are counted after coalescing, in
		// handleOrder; here only against a hard cap on raw frames.
		const bool drive = size >= 6 && payload[4] == 20 && payload[5] == 0;
		if (rateLimit_ != 0 && drive && ++rawDrivesInWindow_ > rateLimit_ * kDriveRawFactor)
		{
			refuse(out, "drive order rate exceeded (" + std::to_string(rateLimit_ * kDriveRawFactor) + " per second)");
			return false;
		}
		if (rateLimit_ != 0 && !drive && ++ordersInWindow_ > rateLimit_)
		{
			refuse(out, "order rate exceeded (" + std::to_string(rateLimit_) + " per second)");
			return false;
		}
		return handleOrder(payload, size, out);
	}
	if (type == kScenarioListReq || type == kScenarioLoad || type == kScenarioEnd)
	{
		if (rateLimit_ != 0 && ++ordersInWindow_ > rateLimit_)
		{
			refuse(out, "order rate exceeded (" + std::to_string(rateLimit_) + " per second)");
			return false;
		}
		return handleScenarioFrame(type, payload, size, out);
	}
	if (type == kNewGame)
	{
		if (rateLimit_ != 0 && ++ordersInWindow_ > rateLimit_)
		{
			refuse(out, "order rate exceeded (" + std::to_string(rateLimit_) + " per second)");
			return false;
		}
		return handleNewGame(payload, size, out);
	}
	// The debug view (docs/formats/SIMLINK.md "DEBUG VIEW"): 200-219 is always accepted and dropped when it is compiled out or off.
	if (type >= debugview::kDebugFirst && type <= debugview::kDebugLast)
		return debug_.frame(sim_, ctx_.debugView && debugPeer_, type, payload, size);
	refuse(out, "unexpected frame type " + std::to_string(type) + " from the client");
	return false;
}

// NEW_GAME (MULTIPLAYER.md 4a): u32 clientSeq, str name, strid startoff, u8 n, n x Spec {strid race, u8 female, strid head, strid hair,
// strid beard, strid hairColour, f32 skin, u8 sliderCount, sliderCount x {str name, f32 value}, str name, i32 stance}. A frame-shape
// fault is a protocol error (MULTIPLAYER.md 4b kick); everything the data decides is a rejection in the reply (type 62).
bool Session::handleNewGame(const std::uint8_t* payload, std::size_t size, RecordWriter& out)
{
	Reader r{payload, size};
	r.u32(); // clientSeq: the reply (62) carries none, the session has one NEW_GAME in flight
	const std::string name = r.str();
	const std::uint32_t startoffId = r.u32();
	const std::uint8_t n = r.u8();
	bool tooLong = name.size() > kMaxClientString;
	if (n > kMaxSquadSpec)
		r.bad = true;
	// A strid is a record sid when the string table has it as one; 0 is none; anything else cannot name a record (kBadSid).
	const auto sidOf = [&](std::uint32_t id) -> std::string
	{
		if (id == 0)
			return std::string();
		return id <= strings_.size() && strings_.entry(id).kind == 1 ? strings_.text(id) : std::string(kBadSid);
	};
	std::vector<CharacterSpec> specs;
	for (std::uint8_t i = 0; i < n && !r.bad; ++i)
	{
		CharacterSpec spec;
		spec.race = sidOf(r.u32());
		spec.female = r.u8();
		spec.head = sidOf(r.u32());
		spec.hair = sidOf(r.u32());
		spec.beard = sidOf(r.u32());
		spec.hairColour = sidOf(r.u32());
		spec.skin = r.f32();
		const std::uint8_t sliders = r.u8();
		if (sliders > kMaxSliders)
			r.bad = true;
		for (std::uint8_t k = 0; k < sliders && !r.bad; ++k)
		{
			std::string sliderName = r.str();
			tooLong = tooLong || sliderName.size() > kMaxClientString;
			spec.sliders.emplace_back(std::move(sliderName), r.f32());
		}
		spec.name = r.str();
		tooLong = tooLong || spec.name.size() > kMaxClientString;
		spec.stance = static_cast<std::int32_t>(r.u32());
		specs.push_back(std::move(spec));
	}
	// The new-game advanced options (ADVANCED_OPTIONS_STATIC.md; task BT): an OPTIONAL trailing block, so a client that sends
	// none (every build before this one) is unchanged: u8 count, count x {str key, f32 value}. cod / gdm / ht only; a knob
	// with nothing behind it yet is refused, never silently ignored.
	std::string options;
	if (!r.bad && r.at < size)
	{
		const std::uint8_t count = r.u8();
		for (std::uint8_t i = 0; i < count && !r.bad; ++i)
		{
			std::string key = r.str();
			const float value = r.f32();
			tooLong = tooLong || key.size() > kMaxClientString;
			if (r.bad || tooLong)
				break;
			const bool known = key == "cod" || key == "gdm" || key == "ht";
			const bool inRange = key == "cod" ? value >= state::WorldOptions::kChanceOfDeathMin && value <= state::WorldOptions::kChanceOfDeathMax
				: key == "gdm" ? value >= state::WorldOptions::kDamageScaleMin && value <= state::WorldOptions::kDamageScaleMax
				: key == "ht" ? value >= state::WorldOptions::kHungerTimeMin && value <= state::WorldOptions::kHungerTimeMax
				: false;
			if (!known)
			{
				refuse(out, "unknown new-game option '" + key + "'");
				return false;
			}
			if (!inRange || !std::isfinite(value))
			{
				refuse(out, "new-game option '" + key + "' out of range");
				return false;
			}
			if (!options.empty())
				options += ';';
			options += key + '=' + std::to_string(value);
		}
	}
	if (r.bad || tooLong || r.at != size)
	{
		refuse(out, "malformed NEW_GAME (" + std::to_string(size) + " bytes)");
		return false;
	}
	if (!pendingNewGame_.empty())
	{
		refuse(out, "NEW_GAME while one is pending");
		return false;
	}
	std::string startoff;
	if (startoffId >= 1 && startoffId <= strings_.size() && strings_.entry(startoffId).kind == 1)
		startoff = strings_.text(startoffId);
	if (const std::string reason = sim_.checkNewGame(startoff, name, specs); !reason.empty())
	{
		NewGameOutcome rejected;
		rejected.reason = reason;
		writeNewGameResult(out, rejected);
		return true;
	}
	// The next tick applies it (a logged sim command: replay and save see it); the reply goes out when it has.
	const std::uint64_t ticket = ctx_.nextTicket++;
	Command command = Command::newGame(startoff, name, ticket, specs.empty() ? std::string() : encodeSpecs(specs), options);
	command.tick = sim_.clockTick() + 1;
	sim_.enqueue(command);
	ctx_.orderLog.push_back(OrderLogEntry{command.tick, 0, kNewGame, true, command, "new_game startoff=" + startoff + " name=" + name + " specs=" + std::to_string(specs.size())});
	pendingNewGame_.insert(ticket);
	return true;
}

void Session::writeNewGameResult(RecordWriter& out, const NewGameOutcome& outcome)
{
	if (!outcome.reason.empty())
	{
		strings_.intern(4, outcome.reason);
		flushStrings(out);
	}
	out.begin(kNewGameResult);
	out.u8("result", outcome.reason.empty() ? 0 : 1);
	if (outcome.reason.empty())
	{
		out.u64("faction", outcome.faction);
		out.list("members", outcome.members.size(), 16);
		for (const std::uint64_t member : outcome.members)
		{
			out.item();
			out.u64("member", member);
		}
		out.listEnd();
		out.f32("x", static_cast<float>(outcome.x)); // the leader's position after placement
		out.f32("y", static_cast<float>(outcome.y));
		out.f32("z", static_cast<float>(outcome.z));
	} else {
		out.strid("reason", strings_.intern(4, outcome.reason));
	}
	out.end();
}

void Session::writeStarts(RecordWriter& out, const std::vector<Simulation::StartReady>& starts)
{
	// The creator's sids ride in the string table (kind 1) so a client can name a race, head, hair, beard or colour by strid in NEW_GAME.
	if (const CreatorCatalog* creator = sim_.creatorCatalog())
	{
		for (const std::string& creatorSid : creator->sids())
			sid(creatorSid);
	}
	out.begin(kStarts);
	out.list("starts", starts.size(), 16);
	for (const Simulation::StartReady& start : starts)
	{
		out.item();
		out.strid("startoff", sid(start.sid));
		out.u8("ready", start.ready ? 1 : 0);
		// The character creator's part: the force race list and the squad members (SIMLINK.md STARTS extension).
		out.u8("k", static_cast<std::uint8_t>(start.races.size()));
		for (const std::string& race : start.races)
			out.strid("race", sid(race));
		out.u8("m", static_cast<std::uint8_t>(start.members.size()));
		for (const Simulation::StartReady::Member& member : start.members)
		{
			out.strid("race", member.race.empty() ? 0 : sid(member.race));
			out.u8("female", member.female);
			out.str("name", member.name);
			out.u8("flags", member.flags);
			out.strid("character", member.character.empty() ? 0 : sid(member.character));
			out.u8("a", static_cast<std::uint8_t>(member.armour.size()));
			for (const std::string& piece : member.armour)
				out.strid("armour", sid(piece));
			out.strid("weapon", member.weapon.empty() ? 0 : sid(member.weapon));
		}
	}
	out.listEnd();
	out.end();
}

void Session::flushStrings(RecordWriter& out)
{
	const std::uint32_t first = strings_.sent() + 1;
	const std::size_t count = strings_.size() - strings_.sent();
	if (count == 0)
		return;
	out.begin(kStrtab);
	out.u32("firstId", first);
	out.list("entries", count, 32);
	for (std::uint32_t id = first; id < first + count; ++id)
	{
		out.item();
		out.u8("kind", strings_.entry(id).kind);
		out.str("text", strings_.entry(id).text);
	}
	out.listEnd();
	out.end();
	strings_.markSent();
}

void Session::ackApplied(RecordWriter& out, std::uint32_t seq, std::uint64_t appliedTick)
{
	out.begin(kEvent);
	out.u64("tick", sim_.clockTick());
	out.u16("kind", 9);
	out.u32("clientSeq", seq);
	out.u64("appliedTick", appliedTick);
	out.end();
}

void Session::ackRejected(RecordWriter& out, std::uint32_t seq, const std::string& reason)
{
	strings_.intern(4, reason);
	flushStrings(out);
	out.begin(kEvent);
	out.u64("tick", sim_.clockTick());
	out.u16("kind", 10);
	out.u32("clientSeq", seq);
	out.strid("reason", strings_.intern(4, reason));
	out.end();
}

namespace
{

std::vector<const state::Inventory::Entry*> flatEntries(const state::Inventory& inventory);

} // namespace

// The one list of orders that apply at once while the clock is paused (SIMLINK.md section 7): the trade moves, a transfer to or from an open
// trader's stock. World actions stay tick-bound.
// (Order 121 set_game_option sets whilePaused itself: the options page is open on a paused game.)
static bool appliesWhilePaused(const InventoryOrder& order)
{
	return order.kind == 68 && order.trader != 0;
}

bool Session::handleOrder(const std::uint8_t* payload, std::size_t size, RecordWriter& out)
{
	Reader r{payload, size};
	const std::uint32_t seq = r.u32();
	const std::uint16_t kind = r.u16();
	if (r.bad)
	{
		refuse(out, "truncated order frame");
		return false;
	}
	const std::uint64_t now = sim_.clockTick();
	const std::uint64_t next = now + 1;
	const CombatArena* arena = sim_.combat();

	std::vector<std::uint64_t> ids;
	const auto readIds = [&]()
	{
		const std::uint16_t n = r.u16();
		if (n > kMaxOrderIds)
		{
			r.bad = true; // a protocol error, not an answerable rejection
			return;
		}
		for (std::uint16_t i = 0; i < n && !r.bad; ++i)
			ids.push_back(r.u64());
	};

	std::vector<Command> commands;
	std::string reject;
	std::string text;
	bool simulated = true;
	bool whilePaused = false; // the order applies between ticks when the clock is paused (appliesWhilePaused)
	std::function<void()> apply; // non-sim effects, run only once the whole frame has validated
	std::function<void()> control; // save / load session controls (80-83): answer the order themselves, never logged
	bool movesActor = false; // a non-queued order that sends the actors somewhere: it closes their trades (SIMLINK TRADE)
	std::uint64_t talkTo = 0; // the character an order 60 talk goes to: a trade open with it stays open (it is only opened again)

	// Every id of a group order must be a living player-controlled character.
	const auto checkIds = [&]() -> bool
	{
		if (ids.empty())
		{
			reject = "no_characters";
			return false;
		}
		for (const std::uint64_t id : ids)
		{
			const state::Fighter* fighter = arena != nullptr ? arena->find(id) : nullptr;
			if (fighter == nullptr)
			{
				reject = "unknown_id";
				return false;
			}
			if (!fighter->playerControlled)
			{
				reject = "not_player_controlled";
				return false;
			}
			if (fighter->dead)
			{
				reject = "dead";
				return false;
			}
		}
		return true;
	};

	switch (kind)
	{
	case 1: // move_to
	{
		readIds();
		const float x = r.f32();
		const float z = r.f32();
		const float y = r.f32();
		const std::uint8_t flags = r.u8();
		if (r.bad)
			break;
		const bool queue = (flags & kMoveQueue) != 0;
		const bool yValid = (flags & kMoveYValid) != 0;
		movesActor = !queue;
		text = "move_to ids=" + joinIds(ids) + " x=" + std::to_string(x) + " z=" + std::to_string(z)
			+ " y=" + std::to_string(y) + " flags=" + std::to_string(flags);
		if ((flags & ~(kMoveQueue | kMoveYValid)) != 0 || !finite(y) || (yValid && std::fabs(y) > kMaxMoveY))
		{
			reject = "bad_value";
			break;
		}
		if (!checkIds())
			break;
		if (!finite(x) || !finite(z) || std::fabs(x) > kworld::kWorldExtent || std::fabs(z) > kworld::kWorldExtent)
		{
			reject = "bad_value";
			break;
		}
		// The server routes (SIMLINK.md section 12). No navmesh, no nav: nothing is walked in a
		// straight line. On one mesh the local/long split of the navigator is the same search.
		// A group order (MOVE_CLICK_LIVE.md): the first member gets the exact point, the others a scatter
		// around it, click + r (sin a, cos a) with r = u1 x sqrt(count x 60) + 5 and a random angle, each
		// snapped like a single click. kinspect 0789d56 (DECODED): the original reseeds with srand(count),
		// so the scatter is deterministic per member count and selection order: the same count gives the
		// same offsets on every click (our seed is the count, not the tick/seq).
		RngStream scatter(streamSeed(kClickStream, ids.size()));
		std::vector<std::pair<std::pair<double, double>, double>> stand;
		const MeshNavigator* nav = sim_.navigation();
		for (std::size_t i = 0; i < ids.size(); ++i)
		{
			const std::uint64_t id = ids[i];
			double goalX = x;
			double goalZ = z;
			if (i != 0)
			{
				const double radius = scatter.uniform01() * std::sqrt(static_cast<double>(ids.size()) * 60.0) + 5.0;
				const double angle = scatter.uniform01() * 2.0 * 3.14159265358979323846;
				goalX += radius * std::sin(angle);
				goalZ += radius * std::cos(angle);
				// Stand points (k16.6.597, user 2026-11-21): the member's own spot, both footprints from every earlier member's.
				std::tie(goalX, goalZ) = standPoint({goalX, goalZ}, arena->data().pathRadius(arena->find(id)->raceSid), stand, angle);
			}
			stand.push_back({{goalX, goalZ}, arena->data().pathRadius(arena->find(id)->raceSid)});
			Route route;
			// A goal in a zone that is not loaded is never refused here (kenshi-k16.6.481, rwh208): the walk goes by legs along the line
			// and the roads, and the goal is routed when the walker's zone is its own (Simulation::startMoveOrder).
			if (nav != nullptr && nav->farGoal(arena->find(id)->x, arena->find(id)->z, goalX, goalZ))
			{
				commands.push_back(Command::fighterMoveFar(state::Id{id}, goalX, goalZ, yValid ? y : arena->find(id)->y, seq, queue));
				continue;
			}
			if (nav != nullptr)
			{
				const state::Fighter& fighter = *arena->find(id);
				// The character's own clearance (NAVMESH_CACHE.md "Clearance"): a click through a gap narrower than
				// its body is no_route, as the original refuses it.
				route = nav->routeFrom({fighter.x, fighter.y, fighter.z}, goalX, goalZ,
					yValid ? std::optional<double>(y) : std::nullopt, false, arena->data().body(fighter.raceSid), true, true);
			}
			if (!route.found)
			{
				// Which limit refused it (kenshi-h2v.25: a crowded-building click came back no_route and the cause was not on the wire).
				if (ctx_.log && nav != nullptr)
				{
					const state::Fighter& fighter = *arena->find(id);
					char line[480];
					std::snprintf(line, sizeof(line), "move_no_route id=%llu seq=%u from=(%.1f %.1f %.1f) click=(%.1f %s %.1f) radius=%.1f why=%s check=\"%s\"",
						static_cast<unsigned long long>(id), seq, fighter.x, fighter.y, fighter.z, goalX, yValid ? std::to_string(y).c_str() : "-", goalZ,
						arena->data().pathRadius(fighter.raceSid), std::string(state::aiReasonName(route.failure)).c_str(), std::string(route.check.empty() ? std::string_view("none named") : route.check).c_str());
					ctx_.log(line);
				}
				reject = "no_route";
				break;
			}
			// The click (the client's green marker) against where the route really ends: the goal snap can pick other ground (a floor
			// of a building in front, user 2026-11-08). A large gap is logged once per member; the order itself is unchanged.
			if (ctx_.log && !route.points.empty() && route.heights.size() == route.points.size())
			{
				const double endX = route.points.back().x;
				const double endZ = route.points.back().z;
				const double endY = route.heights.back();
				const double offXZ = std::hypot(endX - goalX, endZ - goalZ);
				const double offY = yValid ? std::fabs(endY - y) : 0.0;
				if (offXZ > kMoveDivergeXZ || offY > kMoveDivergeY)
				{
					char line[320];
					std::snprintf(line, sizeof(line),
						"move_diverged id=%llu seq=%u click=(%.1f %s %.1f) route_end=(%.1f %.1f %.1f) off_xz=%.1f off_y=%.1f points=%zu approach=%d",
						static_cast<unsigned long long>(id), seq, goalX, yValid ? std::to_string(y).c_str() : "-", goalZ, endX, endY, endZ,
						offXZ, offY, route.points.size(), route.approach ? 1 : 0);
					ctx_.log(line);
				}
			}
			commands.push_back(Command::fighterMoveTo(state::Id{id}, std::move(route.points), std::move(route.heights), seq, queue, route.approach));
		}
		break;
	}
	case 2: // attack
	{
		readIds();
		const std::uint64_t target = r.u64();
		if (r.bad)
			break;
		text = "attack ids=" + joinIds(ids) + " target=" + std::to_string(target);
		movesActor = true;
		if (!checkIds())
			break;
		// A hostile of an imported save may still be a member of a token squad (no Fighter): the order builds its squad (kenshi-k16.6.679).
		const state::Fighter* enemy = sim_.promoteForOrder(target) ? arena->find(target) : nullptr;
		if (enemy == nullptr || enemy->dead)
		{
			reject = "bad_target";
			break;
		}
		// Kenshi lets the player attack anyone outside its own party; team is a combat grouping (worldTeam: every faction not hostile to
		// the player shares team 0), not a party, so a neutral Hub NPC is a legal target (client report, user 2026-11-04). The reason keeps
		// its wire name `same_team` (SIMLINK.md section 11): the attacker's own party, i.e. a player-controlled character.
		if (enemy->playerControlled)
			reject = "same_team";
		if (!reject.empty())
			break;
		for (const std::uint64_t id : ids)
			commands.push_back(Command::fighterAttack(state::Id{id}, state::Id{target}));
		break;
	}
	case 4: // stop / hold
	{
		readIds();
		const std::uint8_t hold = r.u8();
		if (r.bad)
			break;
		text = "stop ids=" + joinIds(ids) + " hold=" + std::to_string(hold);
		if (hold > 1)
		{
			reject = "bad_value";
			break;
		}
		if (!checkIds())
			break;
		for (const std::uint64_t id : ids)
			commands.push_back(Command::fighterStop(state::Id{id}, hold != 0));
		break;
	}
	case 10: // toggle
	{
		readIds();
		const std::uint8_t which = r.u8();
		const std::uint8_t value = r.u8();
		if (r.bad)
			break;
		text = "toggle ids=" + joinIds(ids) + " which=" + std::to_string(which) + " value=" + std::to_string(value);
		if (which == 5) // speed mode: 0 Walk, 1 Jog, 2 Run, 3 Group; above 3 bad_value (SIMLINK.md order 10)
		{
			if (value > 3)
			{
				reject = "bad_value";
				break;
			}
			if (!checkIds())
				break;
			// Group: the order's ids become one group, keyed by the lowest id; another mode leaves it (on apply).
			std::uint64_t group = 0;
			if (value == 3)
				for (const std::uint64_t id : ids)
					group = group == 0 || id < group ? id : group;
			for (const std::uint64_t id : ids)
				commands.push_back(Command::fighterSpeedMode(state::Id{id}, value, group));
			break;
		}
		if (value > 1)
		{
			reject = "bad_value";
			break;
		}
		if (!checkIds())
			break;
		if (which == 6 || which == 7) // passive / hold position (approved by the client 2026-11-16, k16.6.313); never replaces the order
		{
			for (const std::uint64_t id : ids)
				commands.push_back(which == 6 ? Command::fighterPassive(state::Id{id}, value != 0) : Command::fighterHoldPosition(state::Id{id}, value != 0));
			break;
		}
		if (which == 4) // jobs: value = the list's `enabled` (SIMLINK.md section 7, JOBS 34)
		{
			for (const std::uint64_t id : ids)
			{
				JobOrder job;
				job.kind = 10;
				job.character = id;
				job.enabled = value;
				commands.push_back(Command::jobOrder(job));
			}
			break;
		}
		if (which > 2)
		{
			reject = "unsupported"; // ai_off: not in the first cut
			break;
		}
		for (const std::uint64_t id : ids)
		{
			commands.push_back(which == 0 ? Command::fighterBlock(state::Id{id}, value != 0)
				: which == 1 ? Command::fighterSneak(state::Id{id}, value != 0)
				: Command::fighterRun(state::Id{id}, value != 0));
		}
		break;
	}
	case 3: // follow
	{
		readIds();
		const std::uint64_t target = r.u64();
		if (r.bad)
			break;
		text = "follow ids=" + joinIds(ids) + " target=" + std::to_string(target);
		movesActor = true;
		if (!checkIds())
			break;
		const state::Fighter* leader = arena->find(target);
		if (sim_.navigation() == nullptr)
		{
			reject = "unsupported"; // no navmesh, no movement
			break;
		}
		if (leader == nullptr || leader->dead)
		{
			reject = "bad_target";
			break;
		}
		for (const std::uint64_t id : ids)
		{
			if (id == target || arena->followLoops(id, target))
			{
				reject = "bad_target"; // itself, or a loop of followers
				break;
			}
		}
		if (!reject.empty())
			break;
		for (const std::uint64_t id : ids)
			commands.push_back(Command::fighterFollow(state::Id{id}, state::Id{target}));
		break;
	}
	case 20: // drive: id, drive state, heading (SIMLINK.md section 12)
	{
		ids.push_back(r.u64());
		const std::uint8_t mode = r.u8();
		const float heading = r.f32();
		const std::int8_t strafe = r.n - r.at >= 1 ? static_cast<std::int8_t>(r.u8()) : 0; // optional trailing strafe (section 12); absent = 0
		if (r.bad)
			break;
		text = "drive id=" + std::to_string(ids[0]) + " drive=" + std::to_string(mode) + " heading="
			+ std::to_string(heading) + " strafe=" + std::to_string(strafe);
		movesActor = mode != 0 || strafe != 0;
		if (mode > 2 || strafe < -1 || strafe > 1 || !finite(heading) || std::fabs(heading) > 1000.0f) // radians
		{
			reject = "bad_value";
			break;
		}
		if (!checkIds())
			break;
		if (sim_.navigation() == nullptr)
		{
			reject = "unsupported"; // no navmesh, no movement
			break;
		}
		commands.push_back(Command::fighterDrive(state::Id{ids[0]}, mode, heading, seq, strafe));
		// Coalesced per character per tick: counted once (the later orders replace the earlier).
		if (driveCounted_[ids[0]] != next)
		{
			if (rateLimit_ != 0 && ++ordersInWindow_ > rateLimit_)
			{
				refuse(out, "order rate exceeded (" + std::to_string(rateLimit_) + " per second)");
				return false;
			}
			driveCounted_[ids[0]] = next;
		}
		break;
	}
	case 60: // interact: ids, target, action, queue (SIMLINK.md section 13)
	{
		readIds();
		const std::uint64_t target = r.u64();
		const std::uint8_t action = r.u8();
		const std::uint8_t queue = r.u8();
		const std::uint8_t flags = r.n - r.at >= 1 ? r.u8() : 0; // optional trailing flags (section 13); absent = 0
		if (r.bad)
			break;
		text = "interact ids=" + joinIds(ids) + " target=" + std::to_string(target) + " action=" + std::to_string(action)
			+ " queue=" + std::to_string(queue) + " flags=" + std::to_string(flags);
		movesActor = queue == 0;
		talkTo = action == static_cast<std::uint8_t>(InteractAction::Talk) ? target : 0;
		// Flags: loot (5) bit 0 = open; every other bit, and any flag on another action, is bad_value.
		if (action == 0 || action > kLockDoorAction || queue > 1 || (flags & ~kInteractFlagsMask) != 0
			|| (flags != 0 && action != static_cast<std::uint8_t>(InteractAction::Loot)))
		{
			reject = "bad_value";
			break;
		}
		if (!checkIds())
			break;
		if (action == kStealthKoAction)
		{
			// The stealth KO (client-approved 2026-11-10, STEALTH_KO.md): not an interact. The target is a character; queue 1 is bad_value.
			if (queue != 0)
			{
				reject = "bad_value";
				break;
			}
			for (const std::uint64_t id : ids)
			{
				reject = sim_.checkStealthKo(id, target);
				if (!reject.empty())
					break;
			}
			if (reject.empty())
				for (const std::uint64_t id : ids)
					commands.push_back(Command::stealthKo(state::Id{id}, state::Id{target}));
			break;
		}
		if (action >= kPickShacklesAction && action != kLockDoorAction)
		{
			// Pick, cut or break the shackles a character wears (kenshi-h2v.15.1, actions 15..17: the target is the wearer) or a cage's lock (kenshi-k16.5.7, 18..20: the cage key): not an interact; queue 1 is bad_value.
			if (queue != 0)
			{
				reject = "bad_value";
				break;
			}
			for (const std::uint64_t id : ids)
			{
				reject = sim_.checkShackleAction(id, target, action);
				if (!reject.empty())
					break;
			}
			if (reject.empty())
				for (const std::uint64_t id : ids)
					commands.push_back(Command::shackleAction(state::Id{id}, state::Id{target}, action));
			break;
		}
		const MeshNavigator* nav = sim_.navigation();
		if (nav == nullptr)
		{
			reject = "unsupported"; // no navmesh, no movement
			break;
		}
		// Lock door (kenshi-k16.6.816): the Close order that locks the shut door; queue 1 is bad_value. Event 13 names it 21 again.
		if (action == kLockDoorAction && queue != 0)
		{
			reject = "bad_value";
			break;
		}
		const std::uint8_t act = action == kLockDoorAction ? static_cast<std::uint8_t>(InteractAction::Close) : action;
		const std::uint8_t orderFlags = action == kLockDoorAction ? kInteractFlagLock : flags;
		const InteractTarget thing = findInteractTarget(sim_.world().ecs(), arena, target, &sim_);
		if (thing.kind == InteractKind::None)
		{
			reject = "bad_target";
			break;
		}
		for (const std::uint64_t id : ids)
		{
			reject = checkInteract(sim_.world().ecs(), *arena, id, thing, act, orderFlags, ctx_.gameData.get(), &sim_);
			// A queued order starts later: the target's state now (a shut door to close after opening it) proves nothing.
			if (queue != 0 && reject == "not_allowed")
				reject.clear();
			if (!reject.empty())
				break;
		}
		if (!reject.empty())
			break;
		// Every member walks to the use point; the first to arrive acts (a second pick-up finds the item gone).
		// queue 1: the order waits behind the character's running order and queue, and is routed when it starts; the route
		// found here only proves the target is reachable now.
		for (const std::uint64_t id : ids)
		{
			const state::Fighter& fighter = *arena->find(id);
			std::vector<state::GroundPoint> points;
			std::vector<double> heights;
			bool approach = false;
			if (!thing.arrived(fighter))
			{
				const Route route = routeToInteract(*nav, fighter, thing, act, false, arena->data().body(fighter.raceSid));
				if (!route.found)
				{
					reject = "no_route";
					break;
				}
				if (queue == 0)
				{
					points = route.points;
					heights = route.heights;
					approach = route.approach;
				}
			}
			commands.push_back(Command::fighterInteract(state::Id{id}, target, act, seq, std::move(points), std::move(heights), approach, queue != 0, orderFlags));
		}
		break;
	}
	case 61: // drop: id, item record, count (SIMLINK.md section 13)
	{
		const std::uint64_t actor = r.u64();
		const std::string item = r.str();
		const std::uint16_t count = r.u16();
		if (r.bad)
			break;
		ids = {actor};
		text = "drop id=" + std::to_string(actor) + " item=" + item + " count=" + std::to_string(count);
		if (count == 0 || item.empty() || item.size() > 128)
		{
			reject = "bad_value";
			break;
		}
		if (!checkIds())
			break;
		const state::Inventory* held = nullptr;
		sim_.world().each([&](const state::Id& i, const state::Inventory& inventory)
		{
			if (i.value == actor)
				held = &inventory;
		});
		if (held == nullptr || held->carried(item) < count)
		{
			reject = "not_allowed"; // not carried (an equipped item is not droppable yet)
			break;
		}
		commands.push_back(Command::fighterDrop(state::Id{actor}, item, count, seq));
		break;
	}
	case 62: // equip: id, item record, equip (SIMLINK.md section 7)
	{
		const std::uint64_t actor = r.u64();
		const std::string item = r.str();
		const std::uint8_t equip = r.u8();
		if (r.bad)
			break;
		ids = {actor};
		text = "equip id=" + std::to_string(actor) + " item=" + item + " equip=" + std::to_string(equip);
		if (equip > 1 || item.empty() || item.size() > 128)
		{
			reject = "bad_value";
			break;
		}
		if (!checkIds())
			break;
		reject = sim_.checkEquip(actor, item, equip != 0);
		if (reject.empty())
			commands.push_back(Command::fighterEquip(state::Id{actor}, item, equip != 0, seq));
		break;
	}
	case 63: // transfer: id, container, item record, count, take (SIMLINK.md section 7)
	{
		const std::uint64_t actor = r.u64();
		const std::uint64_t container = r.u64();
		const std::string item = r.str();
		const std::uint16_t count = r.u16();
		const std::uint8_t take = r.u8();
		if (r.bad)
			break;
		ids = {actor};
		text = "transfer id=" + std::to_string(actor) + " container=" + std::to_string(container) + " item=" + item
			+ " count=" + std::to_string(count) + " take=" + std::to_string(take);
		if (take > 1 || count == 0 || item.empty() || item.size() > 128)
		{
			reject = "bad_value";
			break;
		}
		if (!checkIds())
			break;
		reject = sim_.checkTransfer(actor, container, item, count, take != 0);
		if (reject.empty())
			commands.push_back(Command::containerTransfer(state::Id{actor}, container, item, count, take != 0, seq));
		break;
	}
	case 64: // move_item: id, version, index, section, x, y [, count] (SIMLINK.md section 7)
	case 65: // arrange: id, section
	case 66: // drop_entry: id, version, index, count
	case 67: // equip_entry: id, version, index, equip
	case 68: // transfer_entry: id, container, version, index, count, take, section, x, y
	{
		InventoryOrder order;
		order.kind = kind;
		order.id = r.u64();
		if (kind == 68)
			order.container = r.u64();
		order.version = InventoryOrder::kAnyVersion; // arrange (65) carries none: a repack of whatever is there
		if (kind != 65)
		{
			order.version = r.u32();
			order.index = r.u16();
		}
		if (kind == 64 || kind == 65 || kind == 68)
		{
			if (kind == 68)
			{
				order.count = r.u16();
				order.flag = r.u8();
			}
			order.section = r.u8();
			if (kind != 65)
			{
				order.x = r.u8();
				order.y = r.u8();
			}
			if (kind == 68 && r.n - r.at >= 8)
				order.source = r.u64(); // optional trailing source container (sale from an owned container, MQ12)
			if (kind == 64 && r.n - r.at >= 2)
				order.count = r.u16(); // optional trailing split count (SIMLINK section 7); absent = the whole stack
		} else if (kind == 66) {
			order.count = r.u16();
		} else {
			order.flag = r.u8();
		}
		if (r.bad)
			break;
		ids = {order.id};
		text = "inventory order " + std::to_string(kind) + " id=" + std::to_string(order.id) + " version=" + std::to_string(order.version)
			+ " index=" + std::to_string(order.index) + " section=" + std::to_string(order.section);
		if ((kind == 66 && order.count == 0) || ((kind == 67 || kind == 68) && order.flag > 1) || (kind == 68 && order.count == 0))
		{
			reject = "bad_value";
			break;
		}
		// 64 and 65 may name a container or a KO'd or dead body (the use-range rule is the simulation's); the rest are for the
		// session's own characters.
		const state::Fighter* named = arena != nullptr ? arena->find(order.id) : nullptr;
		const bool body = named != nullptr && (named->dead || named->unconscious) && (kind == 64 || kind == 65);
		if ((kind >= 66 || named != nullptr) && !body && !checkIds())
			break;
		if (kind == 68 && sim_.findStorage(order.container).trader)
		{
			// A merchant's stock: only the squad of an open trade of this actor (the character it talked to is the range owner).
			const auto open = trades_.find(order.id);
			if (open != trades_.end() && open->second.squad == order.container)
			{
				order.trader = open->second.trader;
				order.window = open->second.window;
			}
		}
		reject = sim_.checkInventoryOrder(order);
		if (reject.empty())
		{
			commands.push_back(Command::inventoryOrder(order));
			whilePaused = appliesWhilePaused(order);
		}
		break;
	}
	case 69: // item_stats: id, version, index (SIMLINK.md ITEM_STATS): a read; answered to this session only, never logged
	{
		const std::uint64_t id = r.u64();
		const std::uint32_t version = r.u32();
		const std::uint16_t index = r.u16();
		if (r.bad || r.at != size)
			break; // truncated / trailing bytes: the generic check below refuses the frame
		// The INVENTORY rule: a player character, a body this session opened, the stock of a trade it has open, or a container / producer
		// inside the detail bounds.
		bool visible = false;
		if (const state::Fighter* fighter = arena != nullptr ? arena->find(id) : nullptr)
		{
			visible = fighter->playerControlled || openLoots_.count(id) != 0;
		} else if (const Simulation::Storage storage = sim_.findStorage(id); storage.entity != 0) {
			if (storage.trader)
			{
				for (const auto& [actor, open] : trades_)
					visible = visible || open.squad == id;
			} else {
				visible = inBounds(sentBounds_, storage.x, storage.z) && inInterest(storage.x, storage.z);
			}
		}
		if (!visible)
		{
			reject = "bad_target";
			break;
		}
		const state::Inventory held = sim_.settledInventory(id);
		const auto flat = flatEntries(held);
		if (held.version != version || index >= flat.size())
		{
			reject = "bad_value";
			break;
		}
		const Simulation::ItemStats stats = sim_.itemStats(*flat[index]);
		control = [this, seq, id, version, index, stats, &out]()
		{
			for (const state::FighterCoverage& part : stats.armour.coverage)
				strings_.intern(4, part.part);
			flushStrings(out);
			writeItemStats(out, sim_.clockTick(), id, version, index, stats);
			ackApplied(out, seq, sim_.clockTick());
		};
		break;
	}
	case 70: // squad_add (no body)
		text = "squad_add";
		reject = sim_.checkSquadOrder(SquadOrder{70});
		if (reject.empty())
			commands.push_back(Command::squadOrder(SquadOrder{70}));
		break;
	case 121: // set_game_option: u8 n, n x {u8 option, u8 value} (SIMLINK GAME_OPTIONS)
	{
		const std::uint8_t n = r.u8();
		std::uint8_t seen = 0;
		bool badValue = n == 0;
		std::vector<Command> entries;
		for (std::uint8_t i = 0; i < n && !r.bad; ++i)
		{
			const std::uint8_t option = r.u8();
			const std::uint8_t value = r.u8();
			if (r.bad)
				break;
			if (option > 1 || value > 1 || (seen & (1 << option)) != 0)
				badValue = true;
			seen |= static_cast<std::uint8_t>(1 << (option & 7));
			entries.push_back(Command::setGameOption(option, value != 0));
		}
		if (r.bad)
			break;
		text = "set_game_option n=" + std::to_string(n);
		whilePaused = true;
		if (sim_.playerFactionSid().empty())
			reject = "not_allowed";
		else if (badValue)
			reject = "bad_value";
		else
			commands = std::move(entries);
		break;
	}
	case 85: // faction_rename: name
	{
		SquadOrder order;
		order.kind = 85;
		order.name = r.str();
		if (r.bad)
			break;
		text = "faction_rename name=" + order.name;
		reject = order.name.size() > 4 * kSquadNameMax ? std::string("bad_name") : sim_.checkSquadOrder(order);
		if (reject.empty())
			commands.push_back(Command::squadOrder(order));
		break;
	}
	case 71: // squad_delete: squadId
	case 72: // squad_rename: squadId, name
	case 74: // squad_reorder: squadId, newIndex
	{
		SquadOrder order;
		order.kind = kind;
		order.squad = r.u64();
		if (kind == 72)
			order.name = r.str();
		else if (kind == 74)
			order.index = r.u16();
		if (r.bad)
			break;
		text = "squad order " + std::to_string(kind) + " squad=" + std::to_string(order.squad);
		if (kind == 72 && order.name.size() > 4 * kSquadNameMax)
		{
			reject = "bad_value"; // far past 32 characters in any encoding
			break;
		}
		reject = sim_.checkSquadOrder(order);
		if (reject.empty())
			commands.push_back(Command::squadOrder(order));
		break;
	}
	case 73: // squad_move: char, squadId, index, swap
	case 75: // dismiss: char
	{
		SquadOrder order;
		order.kind = kind;
		order.character = r.u64();
		if (kind == 73)
		{
			order.squad = r.u64();
			order.index = r.u16();
			order.swap = r.u8();
		}
		if (r.bad)
			break;
		ids = {order.character};
		text = "squad order " + std::to_string(kind) + " char=" + std::to_string(order.character) + " squad=" + std::to_string(order.squad)
			+ " index=" + std::to_string(order.index) + " swap=" + std::to_string(order.swap);
		if (kind == 73 && order.swap > 1)
		{
			reject = "bad_value";
			break;
		}
		if (!checkIds())
			break;
		reject = sim_.checkSquadOrder(order);
		if (reject.empty())
			commands.push_back(Command::squadOrder(order));
		break;
	}
	case 76: // job_add: ids, type, target
	{
		readIds();
		JobOrder order;
		order.kind = 76;
		order.type = r.u16();
		order.target = r.u64();
		if (r.bad)
			break;
		text = "job_add ids=" + joinIds(ids) + " type=" + std::to_string(order.type) + " target=" + std::to_string(order.target);
		if (!checkIds())
			break;
		for (const std::uint64_t id : ids)
		{
			order.character = id;
			reject = sim_.checkJobOrder(order);
			if (!reject.empty())
				break;
		}
		if (!reject.empty())
			break;
		for (const std::uint64_t id : ids)
		{
			order.character = id;
			commands.push_back(Command::jobOrder(order));
		}
		break;
	}
	case 77: // job_remove: id, jobId
	case 78: // job_move: id, jobId, index
	{
		JobOrder order;
		order.kind = kind;
		order.character = r.u64();
		order.jobId = r.u32();
		if (kind == 78)
			order.index = r.u16();
		if (r.bad)
			break;
		ids = {order.character};
		text = "job order " + std::to_string(kind) + " char=" + std::to_string(order.character) + " jobId=" + std::to_string(order.jobId)
			+ " index=" + std::to_string(order.index);
		if (!checkIds())
			break;
		reject = sim_.checkJobOrder(order);
		if (reject.empty())
			commands.push_back(Command::jobOrder(order));
		break;
	}
	case 79: // prospect: id (SIMLINK.md; the client picks the one character)
	{
		const std::uint64_t id = r.u64();
		if (r.bad)
			break;
		ids = {id};
		text = "prospect id=" + std::to_string(id);
		if (!checkIds())
			break;
		reject = sim_.checkProspect(id);
		if (reject.empty())
			commands.push_back(Command::prospect(state::Id{id}));
		break;
	}
	case 90: // build_check: record, x, y, z, qw, qx, qy, qz, floor (BUILDINGS_SLICE.md section 10): a dry run, changes nothing
	case 91: // build_place: the same body; one per ghost at Confirm
	{
		BuildOrder order;
		order.record = r.str();
		order.x = r.f32();
		order.y = r.f32();
		order.z = r.f32();
		order.qw = r.f32();
		order.qx = r.f32();
		order.qy = r.f32();
		order.qz = r.f32();
		order.floor = r.u8();
		if (r.bad)
			break;
		text = std::string(kind == 90 ? "build_check " : "build_place ") + order.record + " x=" + std::to_string(order.x)
			+ " y=" + std::to_string(order.y) + " z=" + std::to_string(order.z) + " q=" + std::to_string(order.qw) + "," + std::to_string(order.qx)
			+ "," + std::to_string(order.qy) + "," + std::to_string(order.qz) + " floor=" + std::to_string(order.floor);
		if (order.record.empty() || order.record.size() > kMaxClientString)
		{
			reject = "bad_value";
			break;
		}
		reject = sim_.checkBuildPlace(order);
		if (!reject.empty())
			break;
		if (kind == 90)
			simulated = false;
		else
			commands.push_back(Command::buildPlace(order));
		break;
	}
	case 94: // build_pause: building, paused
	{
		const std::uint64_t building = r.u64();
		const std::uint8_t paused = r.u8();
		if (r.bad)
			break;
		text = "build_pause building=" + std::to_string(building) + " paused=" + std::to_string(paused);
		if (paused > 1)
		{
			reject = "bad_value";
			break;
		}
		reject = sim_.checkBuildPause(building);
		if (reject.empty())
			commands.push_back(Command::buildPause(building, paused != 0));
		break;
	}
	case 30:
	case 31:
	case 32:
		r.u64();
		r.u64();
		if (!r.bad)
			reject = "unsupported";
		break;
	case 40: // pause
	{
		const std::uint8_t paused = r.u8();
		if (r.bad)
			break;
		simulated = false;
		text = "pause paused=" + std::to_string(paused);
		if (paused > 1)
		{
			reject = "bad_value";
			break;
		}
		apply = [this, paused]() { ctx_.paused = paused != 0; ctx_.pausedUntilOrder = false; };
		break;
	}
	case 41: // set speed
	{
		const float speed = r.f32();
		if (r.bad)
			break;
		simulated = false;
		text = "speed speed=" + std::to_string(speed);
		if (!finite(speed) || speed <= 0.0f || speed > kMaxSpeed)
		{
			reject = "bad_value";
			break;
		}
		apply = [this, speed]()
		{
			ctx_.speed = speed;
			sim_.setGameSpeed(speed); // the decision cadences (k16.6.633); a load keeps it: speed is a session setting
			if (ctx_.pausedUntilOrder)
			{
				ctx_.paused = false; // a --paused start ends at the client's first speed order
				ctx_.pausedUntilOrder = false;
			}
		};
		break;
	}
	case 80: // save_game: name
	case 81: // load_game: name, resetPositions
	case 82: // delete_save: name
	case 83: // list_saves
	{
		std::string name;
		if (kind != 83)
			name = r.str();
		if (kind == 81)
			r.u8(); // resetPositions: accepted and ignored (UNSURE what the original's checkbox maps to)
		if (r.bad)
			break;
		if (r.at != size)
			break; // trailing bytes: the generic check below refuses the frame
		if (ctx_.saveDir.empty())
		{
			reject = "unsupported";
			break;
		}
		if (!admin_)
		{
			reject = "not_allowed";
			break;
		}
		if (kind != 83 && !(reject = checkSaveName(name)).empty())
			break;
		control = [this, kind, seq, name, &out]() { runSaveControl(kind, seq, name, out); };
		break;
	}
	case 84: // new_world: no body
	{
		if (r.bad || r.at != size)
			break; // trailing bytes: the generic check below refuses the frame
		if (!admin_)
		{
			reject = "not_allowed";
			break;
		}
		if (!sim_.hasPristine())
		{
			reject = "unsupported";
			break;
		}
		control = [this, seq, &out]() { runNewWorld(seq, out); };
		break;
	}
	case 88: // reply: id, speaker, menuSeq, option (DIALOGUE.md 3.5 / 13)
	case 89: // end_talk: id, speaker
	{
		const std::uint64_t id = r.u64();
		const std::uint64_t speaker = r.u64();
		const std::uint32_t menuSeq = kind == 88 ? r.u32() : 0;
		const std::uint8_t option = kind == 88 ? r.u8() : 0;
		if (r.bad)
			break;
		text = (kind == 88 ? "reply id=" : "end_talk id=") + std::to_string(id) + " speaker=" + std::to_string(speaker)
			+ (kind == 88 ? " seq=" + std::to_string(menuSeq) + " option=" + std::to_string(option) : std::string());
		const state::Fighter* who = arena != nullptr ? arena->find(id) : nullptr;
		if (who == nullptr)
			reject = "bad_target";
		else if (!who->playerControlled)
			reject = "not_allowed";
		else
			reject = kind == 88 ? sim_.checkDialogueReply(id, speaker, menuSeq, option) : sim_.checkEndConversation(id, speaker);
		if (!reject.empty())
			break;
		commands.push_back(kind == 88 ? Command::dialogueReply(state::Id{id}, state::Id{speaker}, menuSeq, option)
			: Command::endConversation(state::Id{id}, state::Id{speaker}, 4));
		apply = [this, speaker]() { releaseMenu(speaker); }; // the freeze lifts; the command applies on the next tick
		break;
	}
	case 86: // trade_close: no body (SIMLINK TRADE)
	{
		if (r.bad || r.at != size)
			break; // trailing bytes: the generic check below refuses the frame
		simulated = false;
		text = "trade_close";
		apply = [this]()
		{
			while (!trades_.empty())
				closeTrade(trades_.begin()->first, 0);
		};
		break;
	}
	case 50: // set interest
	{
		const float x = r.f32();
		const float z = r.f32();
		const float radius = r.f32();
		if (r.bad)
			break;
		simulated = false;
		text = "set_interest x=" + std::to_string(x) + " z=" + std::to_string(z) + " radius=" + std::to_string(radius);
		if (!finite(x) || !finite(z) || std::fabs(x) > kworld::kWorldExtent || std::fabs(z) > kworld::kWorldExtent
			|| !finite(radius) || radius <= 0.0f || radius > kMaxInterestRadius)
		{
			reject = "bad_value";
			break;
		}
		apply = [this, x, z, radius]()
		{
			hasInterest_ = true;
			interestX_ = x;
			interestZ_ = z;
			interestR_ = radius;
		};
		break;
	}
	case 87: // focus: the inspect focus (SIMLINK.md CHAR_STATS); session control, not logged
	{
		const std::uint64_t id = r.u64();
		if (r.bad)
			break;
		simulated = false;
		text = "focus id=" + std::to_string(id);
		bool producer = false;
		if (id != 0)
		{
			const state::Fighter* fighter = sim_.combat() != nullptr ? sim_.combat()->find(id) : nullptr;
			const std::optional<Simulation::BuildingView> node = fighter == nullptr ? sim_.producerView(id) : std::nullopt;
			producer = node.has_value();
			if (fighter != nullptr ? !inBounds(sentBounds_, fighter->x, fighter->z) : !producer || !inBounds(sentBounds_, node->x, node->z))
			{
				reject = "bad_target";
				break;
			}
		}
		apply = [this, id, producer]()
		{
			if (focus_ != id && focus_ != 0)
				charStatsSent_.erase(focus_); // a fresh frame when it is focused again
			focus_ = producer ? 0 : id;
			focusProducer_ = producer ? id : 0;
			producerSent_.reset(); // the new focus gets its record at once
		};
		break;
	}
	case 99: // menu_query: id, target (SIMLINK.md MENU_ENTRIES): a read; answered to this session only, never logged
	{
		const std::uint64_t id = r.u64();
		const std::uint64_t target = r.u64();
		if (r.bad || r.at != size)
			break; // truncated / trailing bytes: the generic check below refuses the frame
		const state::Fighter* who = arena != nullptr ? arena->find(id) : nullptr;
		const bool inView = arena != nullptr && arena->find(target) != nullptr && known_.count(target) != 0;
		// A cage key (kenshi-k16.5.7): a building some character in the session's view is caged in; its bits are 16..18 (SIMLINK.md MENU_ENTRIES).
		bool cageInView = false;
		if (arena != nullptr && !inView && arena->find(target) == nullptr)
			for (const CombatArena::Member& m : arena->members())
				if (m.fighter.cageBuilding == target && known_.count(m.id) != 0)
					cageInView = true;
		// A door (kenshi-k16.6.816): bits 19 / 20, the gates of order 60 actions 21 / 11.
		const bool doorTarget = arena != nullptr && !inView && !cageInView && arena->find(target) == nullptr
			&& findInteractTarget(sim_.world().ecs(), arena, target, &sim_).kind == InteractKind::Door;
		if (who == nullptr || !who->playerControlled || (!inView && !cageInView && !doorTarget))
			reject = "bad_target";
		else if (id == target)
			reject = "bad_value";
		if (!reject.empty())
			break;
		float chance = -1.0f;
		const std::uint32_t entries = doorTarget ? sim_.doorMenuEntries(id, target) : cageInView ? sim_.cageMenuEntries(id, target, chance) : sim_.menuEntries(id, target);
		control = [this, seq, id, target, entries, chance, &out]()
		{
			writeMenuEntries(out, sim_.clockTick(), seq, id, target, entries, chance);
			ackApplied(out, seq, sim_.clockTick());
		};
		break;
	}
	case 120: // profile_query: no body (SIMLINK.md PROFILE): admin only, a read answered to this session, never logged
	{
		if (r.bad || r.at != size)
			break; // trailing bytes: the generic check below refuses the frame
		if (!admin_)
		{
			reject = "not_allowed";
			break;
		}
#if KSIM_PROFILE
		control = [this, seq, &out]()
		{
			writeProfile(out);
			ackApplied(out, seq, sim_.clockTick());
		};
#else
		reject = "unsupported"; // built without KSIM_PROFILE
#endif
		break;
	}
	case 98: // set_progress_level: level (TASK_PROGRESS rows; session control, not logged)
	{
		const std::uint8_t level = r.u8();
		if (r.bad)
			break;
		simulated = false;
		text = "set_progress_level level=" + std::to_string(level);
		if (level > 3)
		{
			reject = "bad_value";
			break;
		}
		apply = [this, level]()
		{
			progressLevel_ = level;
			taskForce_ = true;
		};
		break;
	}
	case 51: // set view
	{
		const float x = r.f32();
		const float z = r.f32();
		if (r.bad)
			break;
		text = "set_view x=" + std::to_string(x) + " z=" + std::to_string(z);
		if (!finite(x) || !finite(z) || std::fabs(x) > kworld::kWorldExtent || std::fabs(z) > kworld::kWorldExtent)
		{
			reject = "bad_value";
			break;
		}
		hasView_ = true; // the session's camera: the wind tail and the re-apply after a reset read it, not the world singleton
		viewX_ = x;
		viewZ_ = z;
		commands.push_back(Command::setView(x, z));
		break;
	}
	case 52: // set camera follow: id u64 (0 = none)
	{
		if (size - r.at < 8) // a short body is an answered rejection, not a kick
		{
			text = "set_camera_follow (short body)";
			reject = "bad_value";
			break;
		}
		const std::uint64_t id = r.u64();
		if (r.bad)
			break;
		text = "set_camera_follow id=" + std::to_string(id);
		if (id != 0)
		{
			if (!sim_.liveCharacter(id)) // passive: nothing is promoted; the arena protects it while it is in the arena
			{
				reject = "bad_target";
				break;
			}
		}
		follow_ = id;
		commands.push_back(Command::setCameraFollow(key_, id));
		break;
	}
	default:
		// The frame boundary is known, so an unknown kind is an answerable rejection.
		text = "unknown kind " + std::to_string(kind);
		reject = "unknown_order";
		break;
	}

	if (r.bad || (reject.empty() && r.at != size))
	{
		refuse(out, "malformed order (kind " + std::to_string(kind) + ", " + std::to_string(size) + " bytes)");
		return false;
	}
	if (!reject.empty())
	{
		ackRejected(out, seq, reject);
		return true;
	}

	if (control)
	{
		control(); // between ticks, like pause: the server handles a frame between two ticks
		return true;
	}
	if (apply)
		apply();
	if (movesActor && ctx_.tradeCloseOnMove)
	{
		for (const std::uint64_t id : ids)
		{
			const auto open = trades_.find(id);
			if (open == trades_.end() || open->second.trader != talkTo)
				closeTrade(id, 1);
		}
	}
	if (simulated)
	{
		const bool applyNow = whilePaused && ctx_.pausedNow();
		for (Command& command : commands)
		{
			command.tick = next;
			if (applyNow)
			{
				// Applied now, so it goes in the log ahead of the commands already logged for `next` (they were queued and run at the start of that
				// tick) and after the earlier ones applied now: a replay enqueues in log order and applies them in the order they ran live.
				auto at = ctx_.orderLog.end();
				while (at != ctx_.orderLog.begin() && std::prev(at)->tick == next && !std::prev(at)->immediate)
					--at;
				sim_.applyNow(command);
				ctx_.orderLog.insert(at, OrderLogEntry{next, seq, kind, true, command, text, true});
			} else {
				sim_.enqueue(command);
				ctx_.orderLog.push_back(OrderLogEntry{next, seq, kind, true, command, text});
			}
		}
		ackApplied(out, seq, next);
		if (applyNow)
		{
			// No tick will run: the frames that changed go out now (the money, both inventories, TRADE, and CHAR_STATS' encumbrance,
			// whose 1 Hz slot a paused clock never reaches: kenshi-k16.20.15).
			charStatsNow_ = true;
			sendTick(out, false);
			charStatsNow_ = false;
		}
	} else {
		ctx_.orderLog.push_back(OrderLogEntry{now, seq, kind, false, {}, text});
		ackApplied(out, seq, now);
		if (kind == 40 || kind == 41)
			writeClock(out, now); // "on change"
		if (kind == 87 && ctx_.pausedNow())
		{
			// A paused clock reaches no tick slot: the new focus's frames (CHAR_STATS, or the producer's BUILDING row) go out now, for the paused tick.
			charStatsNow_ = true;
			sendTick(out, false);
			charStatsNow_ = false;
		}
	}
	return true;
}

void Session::writeInteractable(RecordWriter& out, std::uint64_t tick, const InterRecord& record)
{
	out.begin(kInteractable);
	out.u64("tick", tick);
	out.u64("id", record.id);
	out.u8("kind", record.kind);
	out.strid("record", record.record.empty() ? 0 : sid(record.record));
	out.f32("x", record.pos[0]);
	out.f32("y", record.pos[1]);
	out.f32("z", record.pos[2]);
	out.f32("useX", record.use[0]);
	out.f32("useY", record.use[1]);
	out.f32("useZ", record.use[2]);
	out.u8("state", record.state);
	out.f32("health", record.health);
	out.f32("maxHealth", record.maxHealth);
	out.u16("count", record.count);
	out.u32("stateVersion", record.version);
	if (record.kind == kInteractableProducer)
	{
		out.strid("output", record.output.empty() ? 0 : sid(record.output));
		out.u16("cap", record.cap);
		out.u8("operators", record.operators);
		out.u8("maxOperators", record.maxOperators);
		out.f32("progress", record.progress);
		out.f32("rate", record.rate);
		out.u64("progressTick", tick);
	}
	// The ownership tail (additive, 2026-11-04, client-approved): every kind, after the kind's own tail.
	out.strid("owner", record.owner.empty() ? 0 : sid(record.owner));
	out.u16("lockLevel", record.lockLevel);
	out.strid("model", record.model.empty() ? 0 : sid(record.model));
	out.f32("yaw", record.yaw);
	// The bed tail (kind 5, client-approved 2026-11-06): the owning building, the furniture BUILDING record and the occupant.
	if (record.kind == kInteractableBed)
	{
		out.u64("building", record.building);
		out.strid("furniture", record.furniture.empty() ? 0 : sid(record.furniture));
		out.u64("occupant", record.occupant);
	}
	// The machine tail (kind 12, client-approved 2026-11-22, kenshi-k16.5.25): kind, state (again: the draw / pick), input and output; a turret's key last.
	if (record.kind == kInteractableMachine)
	{
		out.u8("machineKind", record.machineKind);
		out.u8("machineState", record.state);
		out.strid("input", record.input.empty() ? 0 : sid(record.input));
		out.strid("output", record.output.empty() ? 0 : sid(record.output));
		if (record.machineKind == static_cast<std::uint8_t>(machines::Kind::Turret))
			out.u64("turret", record.building);
	}
	// The container tail (kind 3, client-approved 2026-11-13): which building and layout piece it is, so the client draws no mesh of its own.
	if (record.kind == kInteractableContainer)
	{
		out.u64("building", record.building);
		out.u16("piece", record.piece);
	}
	// The severed limb's tint (kind 2 with item function 0x11, client-approved 2026-11-11): skin rgb, blood rgb, scale xyz.
	if (record.limbTint)
	{
		out.f32("skinR", record.skin[0]);
		out.f32("skinG", record.skin[1]);
		out.f32("skinB", record.skin[2]);
		out.f32("bloodR", record.blood[0]);
		out.f32("bloodG", record.blood[1]);
		out.f32("bloodB", record.blood[2]);
		out.f32("scaleX", record.scale[0]);
		out.f32("scaleY", record.scale[1]);
		out.f32("scaleZ", record.scale[2]);
	}
	out.end();
}

void Session::writeMedical(RecordWriter& out, std::uint64_t tick, const CombatArena::Member& m)
{
	const state::Fighter& fighter = m.fighter;
	out.begin(kMedical);
	out.u64("tick", tick);
	out.u64("id", m.id);
	out.f32("blood", f(fighter.blood));
	out.list("parts", fighter.parts.size(), 8);
	for (const state::FighterPart& p : fighter.parts)
	{
		out.item();
		out.strid("part", strings_.intern(4, p.name));
		out.f32("health", f(p.health));
		out.f32("stun", f(p.stun));
	}
	out.listEnd();
	// Trailing (additive): bit i = row i is a severed limb. Always written, even 0 (a frame without it is an old server).
	std::uint16_t severed = 0;
	for (std::size_t i = 0; i < fighter.parts.size() && i < 16; ++i)
	{
		if (fighter.parts[i].severed())
			severed |= static_cast<std::uint16_t>(1u << i);
	}
	out.u16("severed", severed);
	// The panel's tail (additive, 2026-11-04; kinspect findings/medical_bars.md): the Blood row's scale and arrows, Hunger, its arrow
	// flags, and per row the slat's scale (max - wear), the bandage pool, the splint and the limb state (0 / 1: no prosthetics, no
	// crushing yet) with the robot bit.
	out.f32("bloodMax", f(sim_.combat()->rules().maxBlood(fighter)));
	out.f32("bleed", f(fighter.bleeding));
	out.f32("hunger", f(fighter.hunger));
	out.u8("flags", static_cast<std::uint8_t>((fighter.foodCharges > 0.0 ? 1 : 0) | (fighter.asleep ? 2 : 0)
		| (sim_.portraitHurt(m.id, fighter) ? 4 : 0) | (sim_.lyingInBed(m.id, fighter) ? 8 : 0)));
	out.list("partTail", fighter.parts.size(), 8);
	for (const state::FighterPart& p : fighter.parts)
	{
		out.item();
		out.f32("max", f(p.maxHealth - p.wear));
		out.f32("bandage", f(p.pool));
		out.f32("splint", f(p.bonus));
		out.u8("limb", static_cast<std::uint8_t>((p.severed() ? 1 : 0) | (p.robotic ? 4 : 0)));
	}
	out.listEnd();
	out.end();
}

namespace
{

// The wire's entry list: `items`, then the worn backpack's contents (INVENTORY entry index = position here).
std::vector<const state::Inventory::Entry*> flatEntries(const state::Inventory& inventory)
{
	std::vector<const state::Inventory::Entry*> flat;
	for (const state::Inventory::Entry& entry : inventory.items)
		flat.push_back(&entry);
	if (const state::Inventory::Entry* bag = inventory.wornBackpack())
	{
		for (const state::Inventory::Entry& entry : bag->contents)
			flat.push_back(&entry);
	}
	return flat;
}

} // namespace

void Session::writeInventory(RecordWriter& out, std::uint64_t tick, const InvRecord& record)
{
	const auto flat = flatEntries(record.inventory);
	out.begin(kInventory);
	out.u64("tick", tick);
	out.u64("id", record.id);
	out.u32("version", record.inventory.version);
	out.list("items", flat.size(), 16);
	for (const state::Inventory::Entry* entry : flat)
	{
		out.item();
		out.strid("item", sid(entry->itemSid));
		out.u32("count", entry->count);
		out.u8("slot", entry->slot);
	}
	out.listEnd();
	out.end();
}

// ITEM_QUALITY follows an INVENTORY record of the same version: the generated quality of its weapon and armour entries.
void Session::writeItemQuality(RecordWriter& out, std::uint64_t tick, const InvRecord& record)
{
	const auto flat = flatEntries(record.inventory);
	std::size_t n = 0;
	for (const state::Inventory::Entry* entry : flat)
		n += entry->quality.set();
	if (n == 0)
		return;
	out.begin(kItemQuality);
	out.u64("tick", tick);
	out.u64("id", record.id);
	out.u32("version", record.inventory.version);
	out.list("items", n, 16);
	for (std::size_t i = 0; i < flat.size(); ++i)
	{
		const state::ItemQuality& q = flat[i]->quality;
		if (!q.set())
			continue;
		out.item();
		out.u16("index", static_cast<std::uint16_t>(i));
		out.strid("maker", q.maker.empty() ? 0 : sid(q.maker));
		out.strid("model", q.model.empty() ? 0 : sid(q.model));
		out.f32("level", static_cast<float>(q.level));
		out.u8("grade", q.grade);
	}
	out.listEnd();
	out.end();
}

// MENU_ENTRIES (SIMLINK.md MENU_ENTRIES): the answer to order 99, to the asking session only.
void Session::writeMenuEntries(RecordWriter& out, std::uint64_t tick, std::uint32_t clientSeq, std::uint64_t id, std::uint64_t target, std::uint32_t entries, float pickChance)
{
	out.begin(kMenuEntries);
	out.u64("tick", tick);
	out.u32("clientSeq", clientSeq);
	out.u64("id", id);
	out.u64("target", target);
	out.u32("entries", entries);
	if ((entries & kMenuPickCage) != 0)
		out.f32("pickChance", f(pickChance)); // a cage's pick lock bit: the actor's chance per roll, 0..0.9 (appended; a reader takes it only when present)
	out.end();
}

// CAGES (SIMLINK.md CAGES, kenshi-k16.5.7): the full list of the caged characters in the session's view; `ids[i]` is row i's character.
void Session::writeCages(RecordWriter& out, std::uint64_t tick, const std::vector<Simulation::CageView>& cages, const std::vector<std::uint64_t>& ids)
{
	out.begin(kCages);
	out.u64("tick", tick);
	out.list("cages", cages.size(), 16);
	for (std::size_t i = 0; i < cages.size(); ++i)
	{
		const Simulation::CageView& v = cages[i];
		out.item();
		out.u64("id", ids[i]);
		out.u64("cage", v.cage);
		out.strid("record", strings_.intern(1, v.record));
		out.u8("flags", v.locked ? 1 : 0);
		out.u16("level", static_cast<std::uint16_t>(std::clamp(v.level, 0.0, 65535.0)));
		out.f32("hardness", f(v.hardness));
		out.f32("maxHardness", f(v.maxHardness));
	}
	out.listEnd();
	out.end();
}

#if KSIM_PROFILE
// PROFILE (SIMLINK.md PROFILE): the window summary of the per-system tick timings, the answer to order 120, to the asking session only.
void Session::writeProfile(RecordWriter& out)
{
	const profile::Profiler& profiler = sim_.profiler();
	const profile::Summary summary = profiler.summary();
	out.begin(kProfile);
	out.u64("tick", sim_.clockTick());
	out.u32("window", static_cast<std::uint32_t>(summary.ticks));
	out.f32("tickMeanUs", static_cast<float>(summary.tickMeanUs));
	out.f32("tickP50Us", static_cast<float>(summary.tickP50Us));
	out.f32("tickP99Us", static_cast<float>(summary.tickP99Us));
	out.f32("tickMaxUs", static_cast<float>(summary.tickMaxUs));
	out.f32("tickBudgetUs", static_cast<float>(Simulation::kTickSeconds * 1.0e6));
	out.f32("characters", static_cast<float>(summary.characters));
	out.f32("squads", static_cast<float>(summary.squads));
	out.f32("pathQueries", static_cast<float>(summary.pathQueries));
	out.f32("pathUs", static_cast<float>(summary.pathUs));
	out.u16("n", static_cast<std::uint16_t>(profile::kSections));
	for (std::size_t i = 0; i < profile::kSections; ++i)
	{
		out.str("name", profile::kSectionNames[i]);
		out.f32("meanUs", static_cast<float>(summary.meanUs[i]));
		out.f32("maxUs", static_cast<float>(summary.maxUs[i]));
		out.u64("totalNs", profiler.totals().ns[i]);
	}
	out.end();
}
#endif

// ITEM_STATS (SIMLINK.md ITEM_STATS): the numbers of one INVENTORY entry, then the common block. The i32 melee bonuses go out as
// their two's-complement u32 (the writer has no i32).
void Session::writeItemStats(RecordWriter& out, std::uint64_t tick, std::uint64_t id, std::uint32_t version, std::uint16_t index, const Simulation::ItemStats& stats)
{
	out.begin(kItemStats);
	out.u64("tick", tick);
	out.u64("id", id);
	out.u32("version", version);
	out.u16("index", index);
	out.u8("kind", stats.kind);
	if (stats.kind == 1)
	{
		const state::FighterArmour& a = stats.armour;
		out.f32("blunt", f(a.blunt));
		out.f32("cut", f(a.cut));
		out.f32("cutEfficiency", f(a.cutIntoStun)); // UNSURE: that this is the tooltip row "cut efficiency"
		out.f32("harpoon", f(a.pierce)); // UNSURE: that "harpoon" is the pierce resist; the table is 0..90, not a fraction
		const std::size_t n = std::min<std::size_t>(a.coverage.size(), 255);
		out.u8("nCoverage", static_cast<std::uint8_t>(n));
		for (std::size_t i = 0; i < n; ++i)
		{
			out.strid("part", strings_.intern(4, a.coverage[i].part));
			out.f32("fraction", f(a.coverage[i].coverage));
		}
		out.f32("weight", stats.armourWeight); // UNSURE: no manufacturer / material factor is modelled for armour
		out.f32("combatSpeedMult", f(a.combatSpeedMult));
		out.f32("dodgeMult", f(a.dodgeMult));
		out.f32("damageOutputMult", f(a.damageOutputMult));
		out.f32("attackBonus", f(a.attackBonus));
		out.f32("defenceBonus", f(a.defenceBonus));
		out.f32("unarmedBonus", f(a.unarmedBonus));
		out.u8("armourClass", stats.armourClass);
		out.u8("material", stats.material);
		// (g) UNSURE: the exe's runtime values after its material / class scaling; the sim sends the record's (stealth clamped < 0 -> 1).
		out.f32("stealthMult", stats.stealthMult);
		out.f32("athleticsMult", stats.athleticsMult);
		out.f32("combatSpeedMultRuntime", f(a.combatSpeedMult));
		out.f32("dexterityMult", stats.dexterityMult);
		out.u32("meleeAttack", static_cast<std::uint32_t>(static_cast<std::int32_t>(a.attackBonus))); // i32
		out.u32("meleeDefence", static_cast<std::uint32_t>(static_cast<std::int32_t>(a.defenceBonus))); // i32
	} else if (stats.kind == 2) {
		const state::FighterWeapon& w = stats.weapon;
		out.f32("cutDamage", f(w.cut1)); // UNSURE: base (skill 0) value; the tooltip may print the skill-99 value or a range
		out.f32("bluntDamage", f(w.blunt1));
		out.f32("cutMult", f(w.cutMult));
		out.f32("bluntMult", f(w.bluntMult));
		out.f32("bleed", f(w.bleedMult));
		out.f32("armourPenetration", f(w.armourPenetration));
		out.f32("attackMod", f(w.attackMod));
		out.f32("defenceMod", f(w.defenceMod));
		out.f32("weight", f(w.weight));
		out.f32("length", f(w.length));
		out.u8("weaponType", static_cast<std::uint8_t>(w.weaponType));
	}
	out.f32("value", stats.value); // UNSURE: the sim does not price quality or condition
	out.f32("sellValue", -1.0f); // not modelled (MINE_SELL_SLICE MQ6)
	out.u8("grade", stats.quality.set() ? stats.quality.grade : state::ItemQuality::kNoGrade);
	out.f32("level", stats.quality.set() ? static_cast<float>(stats.quality.level) : -1.0f);
	out.end();
}

// ITEM_LOCK follows ITEM_GRID (SIMLINK.md section 6, kenshi-h2v.15.1): the lock state of the worn shackles; no entries when none are worn.
void Session::writeItemLock(RecordWriter& out, std::uint64_t tick, const InvRecord& record)
{
	const Simulation::ShackleLockView view = sim_.shackleLock(record.id);
	const auto flat = flatEntries(record.inventory);
	std::size_t index = 0;
	while (index < flat.size() && flat[index]->slot != state::Inventory::kArmourBase + 9)
		++index;
	const std::size_t count = view.worn && index < flat.size() ? 1 : 0;
	out.begin(kItemLock);
	out.u64("tick", tick);
	out.u64("id", record.id);
	out.u32("version", record.inventory.version);
	out.list("locks", count, 16);
	if (count != 0)
	{
		out.item();
		out.u16("index", static_cast<std::uint16_t>(index));
		out.u8("flags", static_cast<std::uint8_t>((view.locked ? 1 : 0) | (view.canCut ? 2 : 0) | (view.canBreak ? 4 : 0)));
		out.u16("level", static_cast<std::uint16_t>(view.level));
		out.f32("chance", f(view.chance));
		out.f32("hardness", f(view.hardness));
		out.f32("maxHardness", f(view.maxHardness));
	}
	out.listEnd();
	out.end();
}

// ITEM_GRID follows ITEM_QUALITY (SIMLINK.md section 6): the grids this inventory has now, then every entry's section and cell
// (255 = equipped), material and colour.
void Session::writeItemGrid(RecordWriter& out, std::uint64_t tick, const InvRecord& record)
{
	const auto flat = flatEntries(record.inventory);
	struct GridSize
	{
		std::uint8_t section, w, h;
	};
	std::vector<GridSize> grids{{0, record.inventory.gridW, record.inventory.gridH}};
	if (const state::Inventory::Entry* bag = record.inventory.wornBackpack(); bag != nullptr && ctx_.gameData != nullptr)
	{
		const grid::Size size = grid::Rules(*ctx_.gameData).bagSize(bag->itemSid);
		if (size.w > 0 && size.h > 0)
			grids.push_back({1, static_cast<std::uint8_t>(std::min(size.w, 255)), static_cast<std::uint8_t>(std::min(size.h, 255))});
	}
	out.begin(kItemGrid);
	out.u64("tick", tick);
	out.u64("id", record.id);
	out.u32("version", record.inventory.version);
	out.list("grids", grids.size(), 8);
	for (const GridSize& g : grids)
	{
		out.item();
		out.u8("section", g.section);
		out.u8("w", g.w);
		out.u8("h", g.h);
	}
	out.listEnd();
	out.list("items", flat.size(), 16);
	for (std::size_t i = 0; i < flat.size(); ++i)
	{
		const state::Inventory::Entry& entry = *flat[i];
		const bool equipped = entry.slot != state::Inventory::kCarried;
		out.item();
		out.u16("index", static_cast<std::uint16_t>(i));
		out.u8("section", equipped ? state::Inventory::Entry::kEquipped : (i < record.inventory.items.size() ? 0 : 1));
		out.u8("x", equipped ? 0 : entry.x);
		out.u8("y", equipped ? 0 : entry.y);
		const std::string& material = entry.materialSid();
		out.strid("material", material.empty() ? 0 : sid(material));
		out.strid("colour", entry.colour.empty() ? 0 : sid(entry.colour));
	}
	out.listEnd();
	// Steal tail (kenshi-k16.6.392): one u8 per item above, in the same order, after the list so an older decoder stops before it.
	for (std::size_t i = 0; i < flat.size(); ++i)
		out.u8("steal", i < record.steal.size() ? record.steal[i] : Simulation::kStealNoRoll);
	out.end();
}

std::vector<std::uint8_t> Session::stealTail(std::uint64_t actor, const InvRecord& record) const
{
	std::vector<std::uint8_t> percents;
	for (const state::Inventory::Entry* entry : flatEntries(record.inventory))
		percents.push_back(sim_.stealPercent(actor, record.id, entry->itemSid));
	return percents;
}

std::uint32_t Session::navMeshId()
{
	if (navMeshId_ == 0)
	{
		const knav::NavMesh& mesh = sim_.navigation()->mesh();
		std::uint64_t h = 1469598103934665603ULL;
		const auto mix = [&h](std::uint64_t v) { h = (h ^ v) * 1099511628211ULL; };
		for (const knav::BakedTile& tile : mesh.tiles())
		{
			mix(static_cast<std::uint64_t>(static_cast<std::int64_t>(tile.x)));
			mix(static_cast<std::uint64_t>(static_cast<std::int64_t>(tile.z)));
			mix(tile.data.size());
		}
		navMeshId_ = static_cast<std::uint32_t>(h ^ (h >> 32));
		if (navMeshId_ == 0)
			navMeshId_ = 1;
	}
	return navMeshId_;
}

void Session::writeNavParams(RecordWriter& out, std::uint64_t tick)
{
	const knav::NavMesh::DetourParams p = sim_.navigation()->mesh().detourParams();
	(void) tick;
	out.begin(kNavParams); // no tick field: SIMLINK.md section 12 lists none for 43..45
	out.u32("meshId", navMeshId());
	out.u32("detourVersion", p.version);
	out.f32("originX", p.origin[0]);
	out.f32("originY", p.origin[1]);
	out.f32("originZ", p.origin[2]);
	out.f32("tileWidth", p.tileWidth);
	out.f32("tileHeight", p.tileHeight);
	out.u32("maxTiles", p.maxTiles);
	out.u32("maxPolys", p.maxPolys);
	out.end();
}

void Session::writeNavTile(RecordWriter& out, std::uint64_t tick, const NavSend& tile)
{
	(void) tick;
	out.begin(kNavTile);
	out.u32("meshId", navMeshId());
	out.u32("tx", static_cast<std::uint32_t>(tile.x));
	out.u32("tz", static_cast<std::uint32_t>(tile.z));
	out.u32("layer", 0);
	out.u32("version", tile.version); // the tile's counter on the server (door repaints and rebuilds), 1 as baked
	out.u32("size", static_cast<std::uint32_t>(tile.blob.size()));
	if (!tile.blob.empty())
		out.bytes("data", tile.blob.data(), tile.blob.size());
	out.end();
}

void Session::writeNavPolyFlags(RecordWriter& out, const NavPatchSend& send)
{
	out.begin(kNavPolyFlags);
	out.u32("meshId", navMeshId());
	out.u32("tx", static_cast<std::uint32_t>(send.x));
	out.u32("tz", static_cast<std::uint32_t>(send.z));
	out.u32("layer", 0);
	out.u32("version", send.patch->version); // the tile's version after the patch
	out.list("polys", send.patch->polys.size(), 16);
	for (const knav::NavMesh::PolyPaint& paint : send.patch->polys)
	{
		out.item();
		out.u16("poly", paint.poly);
		out.u16("flags", paint.flags);
		out.u8("area", paint.area);
	}
	out.listEnd();
	out.end();
}

void Session::writeClock(RecordWriter& out, std::uint64_t tick)
{
	const GameTime time = gameTimeAt(tick);
	const double minutes = static_cast<double>(tick) / kTicksPerGameMinute;
	out.begin(kClock);
	out.u64("tick", tick);
	out.u32("day", static_cast<std::uint32_t>(time.day));
	out.f32("hourOfDay", f(std::fmod(minutes, 1440.0) / 60.0));
	out.f32("gameSpeed", ctx_.speed);
	clockPausedSent_ = ctx_.pauseBits();
	out.u8("paused", clockPausedSent_);
	out.end();
}

void Session::writeWeather(RecordWriter& out, std::uint64_t tick)
{
	const std::vector<WeatherView> views = sim_.weather()->views(sim_.world().ecs(), tick);
	const auto strid = [&](const std::string& text) { return text.empty() ? 0u : sid(text); };
	out.begin(kWeather);
	out.u64("tick", tick);
	out.list("regions", views.size(), 16);
	for (const WeatherView& view : views)
	{
		out.item();
		out.strid("biomeGroup", strid(view.group));
		out.strid("weather", strid(view.weather));
		out.strid("previous", strid(view.previous));
		out.f32("fogBlend", f(view.fogBlend));
		out.f32("skyBlend", f(view.skyBlend));
		out.f32("strength", f(view.strength));
	}
	out.listEnd();
	// Optional trailing pair (SIMLINK "WEATHER"): the wind where this session's camera is (order 51), the group from the areas map.
	// Absent when the session never sent a view or the point lies in no region; an old client skips it by the frame length. The
	// session's own camera, not the world's state::ViewFocus: a load or a new world removes that, and another session may own it.
	if (hasView_)
	{
		const std::string group = sim_.weather()->groupAt(viewX_, viewZ_);
		if (!group.empty())
		{
			const flecs::world& ecs = sim_.world().ecs();
			out.f32("windSpeed", f(sim_.weather()->windOf(ecs, group)));
			out.f32("windYaw", f(sim_.weather()->windYawOf(ecs, group)));
		}
	}
	out.end();
}

std::vector<Session::BeamsOut> Session::beamsDue(std::uint64_t tick, bool initial, const CombatArena* arena)
{
	std::vector<BeamsOut> out;
	const BeamField* field = sim_.beams();
	if (field == nullptr)
		return out;
	const std::vector<BeamGroup>& groups = field->groups();
	if (beamEpochSent_.size() != groups.size())
	{
		beamEpochSent_.assign(groups.size(), 0);
		beamsSent_.assign(groups.size(), {});
		beamsForget_ = true;
	}
	// The reference points: the session's camera and each player character.
	std::vector<std::pair<double, double>> refs;
	if (hasView_)
		refs.emplace_back(viewX_, viewZ_);
	if (arena != nullptr)
	{
		for (const CombatArena::Member& m : arena->members())
		{
			if (m.fighter.playerControlled && !m.fighter.dead)
				refs.emplace_back(m.fighter.x, m.fighter.z);
		}
	}
	for (std::size_t g = 0; g < groups.size(); ++g)
	{
		const BeamGroup& group = groups[g];
		std::unordered_map<std::uint32_t, std::uint64_t>& sent = beamsSent_[g];
		for (auto it = sent.begin(); it != sent.end();)
			it = it->second <= tick ? sent.erase(it) : std::next(it);
		BeamsOut entry;
		entry.group = g;
		entry.reset = initial || beamsForget_ || group.epoch != beamEpochSent_[g];
		if (entry.reset)
		{
			sent.clear();
			beamEpochSent_[g] = group.epoch;
		}
		// Every live beam of the group goes to a session inside the zone or within view range of it (h2v.14.14); none to one farther out.
		const bool watching = std::any_of(refs.begin(), refs.end(), [&](const std::pair<double, double>& ref)
			{ return field->nearZone(group.colour, ref.first, ref.second, kBeamRange); });
		for (const BeamSlot& slot : group.slots)
		{
			for (const Beam* beam : {&slot.current, &slot.next})
			{
				if (beam->effect == nullptr || beam->spawnTick > tick + kBeamLeadTicks || beam->endTick <= tick || sent.contains(beam->id))
					continue;
				if (!watching)
					continue;
				sent.emplace(beam->id, beam->endTick);
				entry.beams.push_back(beam);
			}
		}
		if (entry.reset || !entry.beams.empty())
			out.push_back(std::move(entry));
	}
	beamsForget_ = false;
	return out;
}

void Session::writeBeams(RecordWriter& out, std::uint64_t tick, const std::vector<BeamsOut>& groups)
{
	const auto strid = [&](const std::string& text) { return text.empty() ? 0u : sid(text); };
	out.begin(kBeams);
	out.u64("tick", tick);
	out.list("groups", groups.size(), 16);
	for (const BeamsOut& entry : groups)
	{
		out.item();
		out.strid("group", strid(sim_.weather()->model().groups()[entry.group].sid));
		out.u8("flags", entry.reset ? 1 : 0);
		out.list("beams", entry.beams.size(), 16);
		for (const Beam* beam : entry.beams)
		{
			out.item();
			out.u32("id", beam->id);
			out.strid("effect", strid(beam->effect->sid));
			out.u64("spawnTick", beam->spawnTick);
			out.u64("endTick", beam->endTick);
			out.u16("visibleTicks", beam->visibleTicks);
			out.f32("x", beam->x);
			out.f32("z", beam->z);
			out.f32("speed", beam->speed);
			out.f32("radius", beam->radius);
			out.f32("strength", beam->strength);
			out.strid("particles", strid(beam->effect->particles));
			out.strid("sound", strid(beam->effect->sound));
			out.list("segments", beam->segments.size(), 16);
			for (const BeamSegment& segment : beam->segments)
			{
				out.item();
				out.u16("ticks", segment.ticks);
				out.f32("yaw", segment.yaw);
			}
			out.listEnd();
		}
		out.listEnd();
	}
	out.listEnd();
	out.end();
}

void Session::writePlayerSquads(RecordWriter& out, std::uint64_t tick, const PlayerSquadsView& view)
{
	out.begin(kPlayerSquads);
	out.u64("tick", tick);
	out.u64("faction", view.faction);
	out.strid("factionName", strings_.intern(2, view.factionName));
	out.u16("factionCount", view.factionCount);
	out.u16("maxFaction", view.limits.faction);
	out.u16("maxSquad", view.limits.squad);
	out.u16("maxSquads", view.limits.squads);
	out.list("squads", view.squads.size(), 16);
	for (const PlayerSquadsView::Squad& squad : view.squads)
	{
		out.item();
		out.u64("squadId", squad.id);
		out.str("name", squad.name);
		out.list("members", squad.members.size(), 16);
		for (const std::uint64_t member : squad.members)
		{
			out.item();
			out.u64("member", member);
		}
		out.listEnd();
	}
	out.listEnd();
	out.u64("money", static_cast<std::uint64_t>(view.money)); // i64 on the wire, as in SAVES
	out.end();
}

void Session::writeUnlocks(RecordWriter& out, std::uint64_t tick, const std::vector<std::string>& research, const std::vector<std::string>& buildings)
{
	out.begin(kUnlocks);
	out.u64("tick", tick);
	out.list("research", research.size(), 16);
	for (const std::string& sid : research)
	{
		out.item();
		out.strid("research", strings_.intern(1, sid));
	}
	out.listEnd();
	out.list("buildings", buildings.size(), 16);
	for (const std::string& sid : buildings)
	{
		out.item();
		out.strid("building", strings_.intern(1, sid));
	}
	out.listEnd();
	out.end();
}

// RESEARCH_STATE (SIMLINK.md RESEARCH_STATE): sent with UNLOCKS. Without a player faction: tech level 1 and empty lists. A row that is Completed but is
// not in the faction's own list was finished silently (CRAFTING_SLICE 9.2): reason 4.
void Session::writeResearchState(RecordWriter& out, std::uint64_t tick, bool hasFaction, const std::vector<std::string>& completed)
{
	const research::Snapshot none;
	const research::Snapshot& snapshot = hasFaction ? sim_.researchState() : none;
	const std::set<std::string> own(completed.begin(), completed.end());
	out.begin(kResearchState);
	out.u64("tick", tick);
	out.u8("techLevel", static_cast<std::uint8_t>(snapshot.techLevel));
	out.list("records", snapshot.rows.size(), 16);
	for (const research::Row& row : snapshot.rows)
	{
		out.item();
		out.strid("research", strings_.intern(1, row.sid));
		out.u8("state", static_cast<std::uint8_t>(row.state));
		const bool silent = row.state == research::State::Completed && own.count(row.sid) == 0;
		out.u8("reason", silent ? 4 : static_cast<std::uint8_t>(row.reason));
	}
	out.listEnd();
	out.u8("nKinds", static_cast<std::uint8_t>(research::kKindCount));
	for (std::size_t kind = 0; kind < research::kKindCount; ++kind)
	{
		out.u8("kind", static_cast<std::uint8_t>(kind + 1));
		out.list("unlocked", snapshot.unlocked[kind].size(), 16);
		for (const std::string& sid : snapshot.unlocked[kind])
		{
			out.item();
			out.strid("ref", strings_.intern(1, sid));
		}
		out.listEnd();
	}
	out.end();
}

void Session::writeProspect(RecordWriter& out, std::uint64_t tick, const prospect::Result& result)
{
	out.begin(kProspect);
	out.u64("tick", tick);
	out.u64("id", result.id);
	out.f32("x", result.x);
	out.f32("z", result.z);
	out.f32("altitude", result.altitude);
	out.f32("science", result.science);
	out.f32("r", result.r);
	out.strid("zone", result.zone.empty() ? 0 : strings_.intern(2, result.zone));
	out.f32("wind", result.wind);
	out.list("env", result.env.size(), 8);
	for (const auto& [cls, fraction] : result.env)
	{
		out.item();
		out.u8("class", cls);
		out.f32("fraction", fraction);
	}
	out.listEnd();
	out.list("resources", result.resources.size(), 8);
	for (const auto& [resource, value] : result.resources)
	{
		out.item();
		out.u8("resId", resource);
		out.f32("value", value);
	}
	out.listEnd();
	out.u16("n", result.n);
	out.bytes("grey", result.grey.data(), result.grey.size());
	out.bytes("water", result.water.data(), result.water.size());
	out.bytes("fertility", result.fertility.data(), result.fertility.size());
	out.bytes("stone", result.stone.data(), result.stone.size());
	out.list("deposits", result.deposits.size(), 16);
	for (const prospect::Deposit& deposit : result.deposits)
	{
		out.item();
		out.u8("resId", deposit.resource);
		out.f32("x", deposit.x);
		out.f32("z", deposit.z);
		out.f32("richness", deposit.richness);
	}
	out.listEnd();
	out.end();
}

// GAME_OPTIONS (SIMLINK.md GAME_OPTIONS, kenshi-k16.6.797): 21 bytes, to every session.
void Session::writeGameOptions(RecordWriter& out, std::uint64_t tick, const GameOptionsView& view)
{
	out.begin(kGameOptions);
	out.u64("tick", tick);
	out.u8("flags", view.flags);
	out.f32("chanceOfDeath", view.chanceOfDeath);
	out.f32("damageScale", view.damageScale);
	out.f32("hungerTime", view.hungerTime);
	out.end();
}

void Session::writeTownState(RecordWriter& out, std::uint64_t tick, const std::vector<Simulation::TownStateRow>& rows)
{
	out.begin(kTownState);
	out.u64("tick", tick);
	out.list("towns", rows.size(), 16);
	for (const Simulation::TownStateRow& row : rows)
	{
		out.item();
		const auto strid = [&](const std::string& sid) { return sid.empty() ? 0u : strings_.intern(1, sid); };
		out.strid("town", strid(row.town));
		out.strid("owner", strid(row.owner));
		out.u8("flags", row.flags);
		out.u16("live", row.live);
		out.u32("residentsMaxCur", 0);
		out.u32("residentsMaxDes", 0);
		out.u32("residentsActual", static_cast<std::uint32_t>(row.residentsActual));
		out.u32("roamingMaxCur", 0);
		out.u32("roamingMaxDes", 0);
		out.u32("roamingPop", static_cast<std::uint32_t>(row.roamingPop));
		out.u32("roamingSquads", static_cast<std::uint32_t>(row.roamingSquads));
		out.list("residentFactions", row.residentFactions.size(), 8);
		for (const std::string& sid : row.residentFactions)
		{
			out.item();
			out.strid("faction", strid(sid));
		}
		out.listEnd();
		out.list("hostile", row.hostile.size(), 8);
		for (const std::string& sid : row.hostile)
		{
			out.item();
			out.strid("faction", strid(sid));
		}
		out.listEnd();
	}
	out.listEnd();
	out.end();
}

void Session::writeFactionRelations(RecordWriter& out, std::uint64_t tick, const std::vector<Simulation::FactionRelationRow>& rows)
{
	out.begin(kFactionRelations);
	out.u64("tick", tick);
	out.list("factions", rows.size(), 16);
	for (const Simulation::FactionRelationRow& row : rows)
	{
		out.item();
		out.strid("faction", strings_.intern(1, row.sid));
		out.str("name", row.name);
		out.f32("value", static_cast<float>(row.value));
	}
	out.listEnd();
	out.end();
}

void Session::writeTaskProgress(RecordWriter& out, std::uint64_t tick, const std::vector<Simulation::TaskProgressRow>& rows)
{
	out.begin(kTaskProgress);
	out.u64("tick", tick);
	out.list("tasks", rows.size(), 16);
	for (const Simulation::TaskProgressRow& row : rows)
	{
		out.item();
		out.u64("char", row.id);
		out.u8("kind", row.kind);
		out.f32("progress", row.progress);
		out.f32("rate", row.rate);
		out.f32("value", row.value);
	}
	out.listEnd();
	// The tail, in row order: flags (bit 0 the original shows the bar, bit 1 the character is the player's), then what is worked on.
	for (const Simulation::TaskProgressRow& row : rows)
		out.u8("flags", row.flags);
	for (const Simulation::TaskProgressRow& row : rows)
		out.u64("target", row.target);
	out.end();
}

void Session::writeDiscovered(RecordWriter& out, std::uint64_t tick, const std::vector<Simulation::DiscoveredPlace>& places)
{
	out.begin(kDiscovered);
	out.u64("tick", tick);
	out.list("places", places.size(), 16);
	for (const Simulation::DiscoveredPlace& place : places)
	{
		out.item();
		out.strid("town", strings_.intern(1, place.sid));
		out.u8("visited", place.visited ? 1 : 0);
	}
	out.listEnd();
	out.end();
}

void Session::writeBuildings(RecordWriter& out, std::uint64_t tick, const std::vector<Simulation::BuildingView>& rows)
{
	out.begin(kBuilding);
	out.u64("tick", tick);
	out.list("buildings", rows.size(), 16);
	for (const Simulation::BuildingView& v : rows)
	{
		out.item();
		out.u64("id", v.id);
		out.strid("record", strings_.intern(1, v.record));
		out.u64("owner", v.owner);
		out.f32("x", static_cast<float>(v.x));
		out.f32("y", static_cast<float>(v.y));
		out.f32("z", static_cast<float>(v.z));
		out.f32("qw", static_cast<float>(v.qw));
		out.f32("qx", static_cast<float>(v.qx));
		out.f32("qy", static_cast<float>(v.qy));
		out.f32("qz", static_cast<float>(v.qz));
		out.u8("floor", v.floor);
		out.u8("status", v.status);
		out.u32("flags", v.flags);
		out.f32("progress", static_cast<float>(v.progress));
		out.f32("rate", static_cast<float>(v.rate));
		out.f32("hp", static_cast<float>(v.hp));
		out.f32("maxHp", static_cast<float>(v.maxHp));
		out.list("materials", v.mats.size(), 8);
		for (const state::BuildMaterial& m : v.mats)
		{
			out.item();
			out.strid("item", strings_.intern(1, m.itemSid));
			out.u16("have", static_cast<std::uint16_t>(std::min<std::uint32_t>(m.have, 65535)));
			out.u16("need", static_cast<std::uint16_t>(std::min<std::uint32_t>(m.need, 65535)));
		}
		out.listEnd();
		// Phases 3-6 fill these (power, workers, efficiency); neutral until then.
		out.f32("powerValue", static_cast<float>(v.powerValue));
		out.f32("powerSupplied", 0.0f);
		out.f32("powerDemand", 0.0f);
		out.f32("stored", 0.0f);
		out.f32("capacity", 0.0f);
		out.u8("workers", v.workers);
		out.u8("maxWorkers", v.maxWorkers);
		out.f32("efficiency", static_cast<float>(v.efficiency));
		out.f32("maxEfficiency", static_cast<float>(v.maxEfficiency));
		out.f32("quality", static_cast<float>(v.quality)); // a producer's richness (the focus panel); 1.0 for a player building
		out.u64("town", v.town);
		out.u32("version", v.version);
	}
	out.listEnd();
	out.end();
}

void Session::writeJobs(RecordWriter& out, std::uint64_t tick, const std::vector<std::pair<std::uint64_t, JobsView>>& lists)
{
	out.begin(kJobs);
	out.u64("tick", tick);
	out.list("characters", lists.size(), 16);
	for (const auto& [id, view] : lists)
	{
		out.item();
		out.u64("id", id);
		out.u8("enabled", view.enabled ? 1 : 0);
		out.list("rows", view.rows.size(), 16);
		for (const state::JobRow& row : view.rows)
		{
			out.item();
			out.u32("jobId", row.jobId);
			out.u16("type", row.type);
			out.u64("target", row.target);
			out.u8("active", view.enabled || jobRunsWhenOff(row.type) ? 1 : 0);
		}
		out.listEnd();
	}
	out.listEnd();
	out.end();
}

void Session::writeSquads(RecordWriter& out, std::uint64_t tick)
{
	const flecs::world& ecs = sim_.world().ecs();
	std::unordered_map<flecs::entity_t, std::uint16_t> members;
	sim_.world().each([&](flecs::entity character, const state::CharacterInstance&)
	{
		const flecs::entity squad = character.target<state::MemberOf>();
		if (squad.is_valid())
			++members[squad.id()];
	});
	// A dismissed full-detail character is a Fighter in a squad of its own.
	sim_.world().each([&](flecs::entity character, const state::Fighter&)
	{
		const flecs::entity squad = character.target<state::MemberOf>();
		if (squad.is_valid())
			++members[squad.id()];
	});
	struct Row
	{
		std::uint64_t id;
		flecs::entity_t entity;
	};
	std::vector<Row> rows;
	sim_.world().each([&](flecs::entity squad, const state::SquadInstance&, const state::Position&)
	{
		rows.push_back({squad.get<state::Id>()->value, squad.id()});
	});
	std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.id < b.id; });
	// The player squads follow (their leader's position; the faction's template sid); the limit counts them too.
	const PlayerSquadsView player = sim_.playerSquads();
	if (ctx_.squadLimit != 0 && rows.size() + player.squads.size() > ctx_.squadLimit)
		rows.resize(ctx_.squadLimit > player.squads.size() ? ctx_.squadLimit - player.squads.size() : 0);
	out.begin(kSquads);
	out.u64("tick", tick);
	out.list("squads", rows.size() + player.squads.size(), 32);
	for (const Row& row : rows)
	{
		const flecs::entity squad(ecs.c_ptr(), row.entity);
		const state::SquadInstance& instance = *squad.get<state::SquadInstance>();
		const state::Position& position = *squad.get<state::Position>();
		std::uint32_t faction = 0;
		if (instance.faction.id != 0)
		{
			faction = sid(flecs::entity(ecs.c_ptr(), instance.faction.id).get<state::FactionState>()->templateSid);
		}
		const auto found = members.find(row.entity);
		out.item();
		out.u64("id", row.id);
		out.strid("faction", faction);
		out.f32("x", f(position.x));
		out.f32("z", f(position.z));
		out.u16("members", found != members.end() ? found->second : std::uint16_t{0});
	}
	for (const PlayerSquadsView::Squad& squad : player.squads)
	{
		const state::Fighter* leader = squad.members.empty() || sim_.combat() == nullptr ? nullptr : sim_.combat()->find(squad.members.front());
		out.item();
		out.u64("id", squad.id);
		out.strid("faction", sid(player.factionSid));
		out.f32("x", leader != nullptr ? f(leader->x) : 0.0f);
		out.f32("z", leader != nullptr ? f(leader->z) : 0.0f);
		out.u16("members", static_cast<std::uint16_t>(squad.members.size()));
	}
	out.listEnd();
	out.end();
}

std::string Session::lookSignature(const state::Fighter& fighter, const std::string& sheathed) const
{
	std::string signature = fighter.weapon.name + "~" + sheathed;
	for (const state::FighterArmour& a : fighter.armour)
		signature += "|" + a.name;
	return signature;
}

std::uint32_t Session::lookVersion(const CombatArena::Member& m)
{
	Look& look = looks_[m.id];
	std::string signature = lookSignature(m.fighter, sim_.sheathedWeaponOf(m.id));
	// The squad and the faction ride in CHAR_APPEARANCE: a change resends it.
	if (const std::uint64_t squad = squadIdOf(m.id))
		signature += "#" + std::to_string(squad);
	if (const auto found = ctx_.looks.find(m.id); found != ctx_.looks.end())
		signature += "@" + found->second.factionSid;
	// The body's bulk rides in CHAR_APPEARANCE ("muscle"): whole stat levels, so one resend per level, not per XP tick.
	signature += "!" + std::to_string(static_cast<long>(std::floor(sim_.bodyBulkStat(m.id))));
	// "skinny" (humans only): hunger quantised to 1/20 of the key, so a drain does not resend every tick.
	if (!m.fighter.animal)
		signature += "~" + std::to_string(std::lround(Simulation::bodySkinny(m.fighter.hunger) * 20.0));
	if (look.version == 0 || look.signature != signature)
	{
		look.signature = signature;
		++look.version;
	}
	return look.version;
}

void Session::noteMissingItem(std::uint64_t id, const char* recordType, const std::string& name)
{
	if (missingItemLogged_.emplace(id, recordType + std::string(":") + name).second && ctx_.log)
		ctx_.log("simlink: character " + std::to_string(id) + " has no " + recordType + " record named '" + name + "': sent without it");
}

void Session::writeAppearance(RecordWriter& out, std::uint64_t tick, const CombatArena::Member& m, std::uint32_t version)
{
	const state::Fighter& fighter = m.fighter;
	CharLook look;
	if (const auto found = ctx_.looks.find(m.id); found != ctx_.looks.end())
		look = found->second;
	const kdata::GameData& gd = *ctx_.gameData;

	// Equipment: armour pieces by ARMOUR record name (the last record of that name wins, as in
	// the merged data), then the weapon in the hand unless it is fists.
	struct Item
	{
		std::uint32_t item;
		std::uint8_t section;
	};
	std::vector<Item> items;
	const auto findByName = [&](const char* recordType, const std::string& name) -> const kdata::Record*
	{
		const kdata::Record* found = nullptr;
		for (const kdata::Record* r : gd.byType(kworld::itemTypeByName(recordType)))
		{
			if (r->name == name)
				found = r;
		}
		return found;
	};
	for (const state::FighterArmour& a : fighter.armour)
	{
		const kdata::Record* record = findByName("ARMOUR", a.name);
		if (record == nullptr)
		{
			noteMissingItem(m.id, "ARMOUR", a.name); // one bad item must not kill the snapshot: the character goes without it
			continue;
		}
		const int section = sectionOfSlot(record->intOr("slot", 7));
		if (section >= 0)
			items.push_back({sid(record->stringID), static_cast<std::uint8_t>(section)});
	}
	if (fighter.weapon.weaponType != 5)
	{
		const kdata::Record* record = findByName("WEAPON", fighter.weapon.name);
		if (record == nullptr)
			noteMissingItem(m.id, "WEAPON", fighter.weapon.name);
		else
			items.push_back({sid(record->stringID), 9});
	} else if (const std::string sheathed = sim_.sheathedWeaponOf(m.id); !sheathed.empty()) {
		// The weapon on the back (task 7): the item stays on the wire so the client can attach it; kFlagDrawn is what says it is not in the hand.
		// Always section 0 (back): the sim keeps no hip / back2 choice (the original picks back, then hip, Task_EquipBestWeapon).
		const kdata::Record* record = gd.find(sheathed);
		if (record != nullptr)
			items.push_back({sid(record->stringID), 0});
	}

	out.begin(kAppearance);
	out.u64("tick", tick);
	out.u64("id", m.id);
	out.u64("squadId", squadIdOf(m.id));
	out.strid("faction", look.factionSid.empty() ? 0 : sid(look.factionSid));
	out.strid("race", sid(fighter.raceSid));
	// A character the creator made (NEW_GAME with Specs) carries its own look; everyone else the scenario's.
	const state::FighterLook& made = fighter.look;
	out.u8("sex", made.custom ? made.female : (made.female ? 1 : look.sex));
	out.u16("headIndex", made.custom ? made.headIndex : look.head);
	out.strid("hair", made.hair.empty() ? 0 : sid(made.hair));
	out.strid("beard", made.beard.empty() ? 0 : sid(made.beard));
	out.strid("hairColour", made.hairColour.empty() ? 0 : sid(made.hairColour));
	out.f32("skinTone", static_cast<float>(made.skin));
	// body: the creator's slider values by name, then "Idle stance" (1..N, the creator's stance slider; no field of its own).
	out.list("body", (made.custom ? made.sliders.size() + 1 : 0) + 1 + (fighter.animal ? 0 : 1), 8);
	if (made.custom)
	{
		for (const state::LookSlider& slider : made.sliders)
		{
			out.item();
			out.strid("key", strings_.intern(4, slider.name));
			out.f32("value", static_cast<float>(slider.value));
		}
		out.item();
		out.strid("key", strings_.intern(4, "Idle stance"));
		out.f32("value", static_cast<float>(made.stance));
	}
	// "muscle" for every character: the unclamped bulk m of rwh.240 section 3, from the whole stat level (the version bumps per level).
	out.item();
	out.strid("key", strings_.intern(4, "muscle"));
	out.f32("value", static_cast<float>(sim_.bodyMuscle(m.id)));
	if (!fighter.animal) // an animal has no physique (AppearanceAnimal)
	{
		out.item();
		out.strid("key", strings_.intern(4, "skinny"));
		out.f32("value", static_cast<float>(Simulation::bodySkinny(fighter.hunger)));
	}
	out.listEnd();
	out.list("items", items.size(), 8);
	for (const Item& item : items)
	{
		out.item();
		out.strid("item", item.item);
		out.strid("spec", 0);
		out.strid("colour", 0);
		out.u8("section", item.section);
	}
	out.listEnd();
	out.u16("crossbowAmmo", 0);
	out.u8("hiddenLimbs", 0);
	out.u32("appearanceVersion", version);
	out.strid("name", fighter.name.empty() ? 0 : sid(fighter.name)); // protocol 4
	out.u32("portraitSerial", fighter.portraitSerial); // protocol 5
	out.end();
}

void Session::writeDynamic(RecordWriter& out, std::uint64_t tick, const CombatArena::Member& m, std::uint32_t version,
	const std::set<std::int32_t>& playerTeams, bool lowDetail)
{
	const state::Fighter& fighter = m.fighter;
	std::uint32_t flags = lowDetail ? static_cast<std::uint32_t>(kFlagLowDetail) : 0u;
	if (sim_.combatMode(fighter))
		flags |= kFlagCombat;
	if (fighter.weapon.weaponType != 5) // the weapon in the hand: task 6 puts it there, task 7 (Inventory::kSheathed) takes it out (rwh.105)
		flags |= kFlagDrawn;
	if (fighter.unconscious)
		flags |= kFlagKO;
	if (fighter.dead)
		flags |= kFlagDead;
	if (fighter.down || fighter.unconscious || fighter.dead)
		flags |= kFlagProne;
	if (fighter.playerControlled)
		flags |= kFlagPlayer;
	if (fighter.blockMode)
		flags |= kFlagBlockMode;
	if (fighter.sneak)
		flags |= kFlagStealth;
	// A fighter strolling on a non-urgent goal moves at the walk speed (gaitOf), so the wire says Walk: the client picks its clip from these.
	const std::uint32_t wireMode = fighter.strolling ? 0u : static_cast<std::uint32_t>(fighter.moveMode) & 3u;
	if (wireMode != 0)
		flags |= kFlagRun;
	flags |= wireMode << kFlagSpeedModeShift;
	if (fighter.hold)
		flags |= kFlagHold;
	if (fighter.passive)
		flags |= kFlagPassive;
	if (fighter.holdPosition)
		flags |= kFlagHoldPosition;
	// combat.cpp enemy(): a different team, not dead. Every player character shares the one player faction today (MULTIPLAYER: one
	// per user later), so the session's player teams are those of all player characters.
	if (!fighter.playerControlled && !fighter.dead && !playerTeams.empty() && !playerTeams.contains(fighter.team))
		flags |= kFlagHostile;
	if (sim_.seated(m.id))
		flags |= kFlagSeated;
	if (fighter.asleep)
		flags |= kFlagAsleep;
	if (fighter.indoors)
		flags |= kFlagIndoors;
	if (fighter.carrying != 0)
		flags |= kFlagCarryL; // the lift sets the hand name "left" (GATES_CARRY_STATIC.md 4.3); the right-hand variant is not modelled
	// y is the navmesh surface height when the server has a navmesh; else the client ground-snaps.
	const bool yValid = sim_.navigation() != nullptr;
	if (yValid)
		flags |= kFlagYValid;

	std::uint32_t action = 0;
	std::uint64_t actionStart = 0;
	float actionSpeed = 1.0f;
	bool aimTail = false;
	if (fighter.technique >= 0 && !fighter.techniqueFinished && !fighter.dead)
	{
		const Technique& t = sim_.combat()->data().techniques[static_cast<std::size_t>(fighter.technique)];
		action = strings_.intern(3, t.animName);
		actionSpeed = f(fighter.clipSpeed);
		const double perTick = fighter.clipSpeed * kTickSeconds;
		const auto elapsed = static_cast<std::uint64_t>(perTick > 0.0 ? std::llround(fighter.clipTime / perTick) : 0);
		actionStart = tick > elapsed ? tick - elapsed : 0;
		// A technique with a clip is the case integrate() turns the clip's root motion by the aim (Fighter::aimYaw); an animal
		// technique (no clip) has neither, so it sends no aim.
		aimTail = t.clip >= 0;
	}
	std::uint8_t actionFlags = 0; // bit 0 loop: the client repeats the clip until `action` changes
	if (lowDetail)
	{
		action = 0;
		aimTail = false;
	}
	if (action == 0 && !lowDetail && !fighter.dead && !fighter.unconscious)
	{
		if (const std::optional<Simulation::OperateAction> operate = sim_.operateAction(m.id))
		{
			action = strings_.intern(3, operate->clip);
			actionStart = operate->start;
			actionFlags = 1;
		}
	}
	if (action == 0 && !lowDetail && !fighter.dead && !fighter.unconscious)
	{
		if (const std::optional<Simulation::CaptureAction> capture = sim_.captureAction(m.id))
		{
			action = strings_.intern(3, capture->clip);
			actionStart = capture->start;
			actionFlags = capture->loop ? 1 : 0;
		}
	}
	if (aimTail)
		flags |= kFlagAimTail;
	if (fighter.mount.mounted())
		flags |= kFlagMounted;
	if (fighter.driveStrafe < 0)
		flags |= kFlagStrafeLeft;
	else if (fighter.driveStrafe > 0)
		flags |= kFlagStrafeRight;

	out.begin(kDynamic);
	out.u64("tick", tick);
	out.u64("id", m.id);
	out.f32("x", f(fighter.x));
	out.f32("y", yValid ? f(fighter.y) : 0.0f); // the navmesh point (or the pose y): the client places the body (SIMLINK.md CHAR_DYNAMIC)
	out.f32("z", f(fighter.z));
	out.f32("yaw", f(fighter.yaw));
	// The body's actual motion over the step, not the commanded speed (SIMLINK "CHAR_DYNAMIC"): a push, a separation nudge or a
	// start that jumps to speed moves the body, and the client animates the legs from these two alone.
	out.f32("fwdSpeed", f(m.stepFwd));
	out.f32("latSpeed", f(m.stepLat));
	out.u32("flags", flags);
	out.u64("carriedBy", fighter.carriedBy);
	out.f32("legR", f(legPercent(fighter, "Right Leg")));
	out.f32("legL", f(legPercent(fighter, "Left Leg")));
	out.strid("action", action);
	out.u64("actionStart", actionStart);
	out.f32("actionSpeed", actionSpeed);
	out.u8("drive", static_cast<std::uint8_t>(fighter.drive));
	out.f32("heading", f(fighter.driveHeading));
	out.u32("driveSeq", fighter.driveSeq);
	out.u64("building", fighter.building);
	out.i16("floor", fighter.floor);
	out.u32("appearanceVersion", version);
	out.u8("actionFlags", actionFlags);
	if ((flags & (kFlagSeated | kFlagAsleep)) != 0)
	{
		// The pose tail: the body's snap point, facing and clip (a seated sleeper cannot happen: sleep ends the RELAX goal).
		const std::optional<Simulation::Pose> pose = sim_.pose(m.id);
		out.f32("poseX", f(pose ? pose->x : fighter.x));
		out.f32("poseY", f(pose ? pose->y : fighter.y));
		out.f32("poseZ", f(pose ? pose->z : fighter.z));
		out.f32("poseYaw", f(pose ? pose->yaw : fighter.yaw));
		out.strid("poseClip", strings_.intern(3, pose ? pose->clip : std::string(fighter.asleep ? routines::kClipSleepOnFloor : routines::kClipSittingChair)));
	} else if ((flags & kFlagProne) != 0) {
		// The rest tail (task BG; client-approved 2026-11-06, PHYSICS.md 2-3): while the body lies prone (flag bit 5: down,
		// KO'd or dead) x, y, z above are its rest point and this is the lying direction, head to feet, radians, the character
		// yaw convention (toward (sin yaw, cos yaw), 0 = +Z). Seeded per ragdoll start; it and the pose tail never coexist.
		out.f32("restYaw", f(fighter.restYaw));
	}
	if (aimTail)
	{
		// The aim tail (client-approved 2026-11-07): the look hand the swing's clip root is turned by, same convention as `yaw`
		// (radians, 0 = +Z). After every other conditional tail, so a client that predates it stops before these 4 bytes.
		out.f32("aimYaw", f(fighter.aimYaw));
	}
	if (fighter.mount.mounted())
	{
		// The mounted tail (client-approved 2026-11-10): the turret being manned, the very end of the record.
		out.u64("turret", fighter.mount.building);
	}
	out.end();
}

void Session::writeCharStats(RecordWriter& out, std::uint64_t tick, std::uint64_t id, const Simulation::CharStats& stats)
{
	out.begin(kCharStats);
	out.u64("tick", tick);
	out.u64("id", id);
	out.u8("state", stats.state);
	out.u8("lockLevel", stats.lockLevel);
	out.strid("goal", sid(stats.goal));
	out.f32("encumbrance", stats.encumbrance);
	out.u8("extraKind", stats.extraKind);
	out.u8("extraSkill", stats.extraSkill);
	out.f32("extra", stats.extra);
	out.u8("weaponClass", stats.weaponClass);
	out.list("skills", stats.skills.size(), 8);
	for (const Simulation::CharStats::Skill& skill : stats.skills)
	{
		out.item();
		out.u8("skill", skill.id);
		out.f32("value", skill.value);
		out.f32("bonus", skill.bonus);
	}
	out.listEnd();
	out.f32("athleticsXp", stats.athleticsXp);
	out.f32("strengthXp", stats.strengthXp);
	out.f32("toughnessXp", stats.toughnessXp);
	out.f32("koPoint", stats.koPoint);
	out.f32("runSpeed", stats.runSpeed);
	out.f32("runPenalty", stats.runPenalty);
	out.f32("hungerRate", stats.hungerRate);
	out.f32("light", stats.light);
	out.list("bounties", stats.bounties.size(), 8);
	for (const auto& [faction, amount] : stats.bounties)
	{
		out.item();
		out.strid("faction", sid(faction));
		out.u32("amount", static_cast<std::uint32_t>(amount)); // two's complement
	}
	out.listEnd();
	out.f32("sentenceHours", stats.sentenceHours);
	// The AI tail (spectator mode, rows 16-20): additive after row 15, no protocol bump.
	out.u16("goalEnum", stats.goalEnum);
	out.u8("source", stats.source);
	out.u16("stepEnum", stats.stepEnum);
	out.u8("stepIndex", stats.stepIndex);
	out.u8("stepCount", stats.stepCount);
	out.u64("targetId", stats.targetId);
	const float nan = std::numeric_limits<float>::quiet_NaN();
	out.f32("targetX", stats.hasTargetPoint ? stats.targetX : nan);
	out.f32("targetZ", stats.hasTargetPoint ? stats.targetZ : nan);
	// The stats window tail (rows 21-23, kenshi-k16.6.54): additive, no protocol bump.
	out.list("progress", stats.progress.size(), 8);
	for (const auto& [skill, fraction] : stats.progress)
	{
		out.item();
		out.u8("skill", skill);
		out.f32("progress", fraction);
	}
	out.listEnd();
	out.f32("carryKg", stats.carryKg);
	out.f32("carryMaxKg", stats.carryMaxKg);
	out.f32("damageResist", stats.damageResist);
	// The HUD tag row tail (rows 24-32, kenshi-k16.6.215): additive, no protocol bump.
	out.u16("tagFlags", stats.tagFlags);
	out.u8("slaveState", stats.slaveState);
	out.strid("captor", sid(stats.captor));
	out.f32("slaveHoursLeft", stats.slaveHoursLeft);
	out.u8("slaveLook", stats.slaveLook);
	out.u8("crime", stats.crime);
	out.f32("crimeTimer", stats.crimeTimer);
	out.f32("escapeHoursLeft", stats.escapeHoursLeft);
	out.strid("localFaction", sid(stats.localFaction));
	out.list("bountyTags", stats.bountyTags.size(), 8);
	for (const Simulation::CharStats::BountyTag& tag : stats.bountyTags)
	{
		out.item();
		out.u32("crimes", tag.crimes);
		out.u8("claim", tag.claim);
		out.f32("hoursLeft", tag.hoursLeft);
	}
	out.listEnd();
	// The engage trigger (row 34, k16.5.37): additive after row 32, no protocol bump.
	out.u8("engageBasis", stats.engageBasis);
	out.u8("engageSense", stats.engageSense);
	out.u64("engageCause", stats.engageCause);
	out.u64("engageTick", stats.engageTick);
	out.end();
}

void Session::writeDetect(RecordWriter& out, std::uint64_t tick, const DetectRecord& d)
{
	out.begin(kDetect);
	out.u64("tick", tick);
	out.u64("id", d.id);
	out.f32("meter", d.meter);
	out.u8("state", d.state);
	out.u64("by", d.by);
	out.end();
}

void Session::writeSeenBy(RecordWriter& out, std::uint64_t tick, const SeenByRecord& s)
{
	out.begin(kSeenBy);
	out.u64("tick", tick);
	out.u64("id", s.id);
	out.u16("n", static_cast<std::uint16_t>(s.by.size()));
	for (const std::uint64_t observer : s.by)
		out.u64("observer", observer);
	out.end();
}

void Session::writeDetectedBy(RecordWriter& out, std::uint64_t tick, const DetectedByRecord& d)
{
	out.begin(kDetectedBy);
	out.u64("tick", tick);
	out.u64("sneaker", d.id);
	out.u16("n", static_cast<std::uint16_t>(d.by.size()));
	for (const DetectedByRecord::Entry& e : d.by)
	{
		out.u64("observer", e.observer);
		out.u8("state", e.state);
		out.u8("friendly", e.friendly);
	}
	out.end();
}

Session::RouteDiag Session::routeDiag(const CombatArena::Member& m) const
{
	const state::Fighter& fighter = m.fighter;
	RouteDiag d;
	if (const state::DoorStep* step = sim_.doorStep(m.id))
	{
		d.status = 4; // the route is stashed in the step; the Fighter's own is clear
		d.blocker = 1;
		d.blockerId = step->door;
	} else if (fighter.routeNext < fighter.route.size()) {
		if (fighter.walkStuck > 0.0)
		{
			d.status = 3;
			d.blocker = 3;
			d.reason = state::Fighter::kReplanBlocked;
		} else {
			d.status = 1;
		}
	} else if (fighter.clearWhy == state::Fighter::kClearWayShut || fighter.clearWhy == state::Fighter::kClearGiveUp
		|| (fighter.clearWhy >= 4 && fighter.clearWhy <= 11)) {
		d.status = 5; // abandoned by a failure, until the next route or a clear for another reason
		d.blocker = 3;
		d.reason = fighter.clearWhy;
	}
	return d;
}

void Session::writeRoute(RecordWriter& out, std::uint64_t tick, const CombatArena::Member& m, std::uint32_t seq)
{
	const state::Fighter& fighter = m.fighter;
	const std::size_t left = fighter.route.size() - std::min<std::size_t>(fighter.routeNext, fighter.route.size());
	const std::size_t n = std::min(left, kMaxRoutePoints);
	out.begin(kRoute);
	out.u64("tick", tick);
	out.u64("id", m.id);
	out.u32("clientSeq", seq);
	out.u32("routeVersion", fighter.routeVersion);
	out.u8("flags", (n < left ? kRouteTruncated : 0) | (fighter.routeApproach ? kRouteApproach : 0) | kRouteDiag);
	out.list("points", n, 16);
	for (std::size_t i = 0; i < n; ++i)
	{
		const state::RoutePoint& p = fighter.route[fighter.routeNext + i];
		out.item();
		out.f32("x", f(p.x));
		out.f32("y", f(p.y));
		out.f32("z", f(p.z));
	}
	out.listEnd();
	// The diag tail (SIMLINK.md section 6): a walk held in a door step reports the stashed route's end, else the live route's real end
	// (not the 256th point of a truncated one); a clear with nothing stashed reports 0.
	const RouteDiag diag = routeDiag(m);
	const state::DoorStep* step = diag.status == 4 ? sim_.doorStep(m.id) : nullptr;
	const std::vector<state::RoutePoint>& whole = step != nullptr ? step->route : fighter.route;
	const state::RoutePoint goal = whole.empty() ? state::RoutePoint{0.0, 0.0, 0.0} : whole.back();
	out.u8("status", diag.status);
	out.u8("blocker", diag.blocker);
	out.u8("reason", diag.reason);
	out.u64("blockerId", diag.blockerId);
	out.f32("goalX", f(goal.x));
	out.f32("goalY", f(goal.y));
	out.f32("goalZ", f(goal.z));
	out.f32("stuckSecs", f(fighter.walkStuck));
	out.u16("replans", static_cast<std::uint16_t>(std::min<std::uint32_t>(fighter.routeTries, 0xffff)));
	out.u8("clearedWhy", fighter.clearWhy);
	out.end();
}

void Session::openTrade(std::uint64_t actor, std::uint64_t trader)
{
	const std::uint64_t squad = sim_.traderSquad(trader);
	if (squad == 0)
		return;
	if (const auto old = trades_.find(actor); old != trades_.end() && old->second.trader != trader)
		closeTrade(actor, 1); // talking to another merchant closes the first window
	knownInventory_.erase(squad); // the stock feed starts afresh
	OpenTrade fresh{trader, squad, 0, false, 0, 0, 0};
	fresh.window = (sim_.clockTick() << 8) | ((++tradeWindows_ & 0x7f) + 1); // a reconnected session has another tick, so a window id never repeats
	trades_[actor] = std::move(fresh);
}

void Session::closeTrade(std::uint64_t actor, std::uint8_t reason)
{
	const auto found = trades_.find(actor);
	if (found == trades_.end())
		return;
	Observed closed;
	closed.kind = 40;
	closed.who = found->second.trader;
	closed.result = reason;
	observed_.push_back(closed);
	knownInventory_.erase(found->second.squad);
	trades_.erase(found);
}

// TRADE (type 53, SIMLINK.md section 6): the prices of an open trade. Money is i64 / i32 on the wire; the unsigned writers carry the bits.
void Session::writeTrade(RecordWriter& out, std::uint64_t tick, const TradeOut& trade)
{
	const Simulation::TradeView& view = trade.view;
	out.begin(kTrade);
	out.u64("tick", tick);
	out.u64("trader", trade.trader);
	out.u64("actor", trade.actor);
	out.u64("stock", view.squad);
	out.strid("template", sid(view.templateSid));
	out.u64("traderMoney", static_cast<std::uint64_t>(view.traderMoney));
	out.u32("stockVersion", view.stockVersion);
	out.list("stock", view.stock.size(), 16);
	for (const Simulation::TradeView::StockPrice& price : view.stock)
	{
		out.item();
		out.u16("index", price.index);
		out.u32("buy", static_cast<std::uint32_t>(price.buy));
		out.u32("sell", static_cast<std::uint32_t>(price.sell));
	}
	out.listEnd();
	out.u32("actorVersion", view.actorVersion);
	out.list("actor", view.actor.size(), 16);
	for (const auto& price : view.actor)
	{
		out.item();
		out.u16("index", price.first);
		out.u32("sell", static_cast<std::uint32_t>(price.second));
	}
	out.listEnd();
	// The chest prices (MQ12): every owned container within reach of the trader, by id, always present (k = 0 when none).
	out.list("chests", view.chests.size(), 16);
	for (const Simulation::TradeView::ChestPrices& chest : view.chests)
	{
		out.item();
		out.u64("container", chest.container);
		out.u32("version", chest.version);
		out.list("prices", chest.prices.size(), 16);
		for (const auto& price : chest.prices)
		{
			out.item();
			out.u16("index", price.first);
			out.u32("sell", static_cast<std::uint32_t>(price.second));
		}
		out.listEnd();
	}
	out.listEnd();
	// The buyback tail (kenshi-k16.20.16): the stock entries holding units this faction sold in this window; b = 0 for any other session.
	out.list("buyback", view.buyback.size(), 16);
	for (const Simulation::TradeView::BuybackUnits& units : view.buyback)
	{
		out.item();
		out.u16("index", units.index);
		out.u32("count", units.count);
		out.u32("price", static_cast<std::uint32_t>(units.price));
	}
	out.listEnd();
	out.end();
}

void Session::writeEvent(RecordWriter& out, std::uint64_t tick, const Observed& e)
{
	out.begin(kEvent);
	out.u64("tick", e.tick != 0 ? e.tick : tick);
	out.u16("kind", e.kind);
	if (e.kind == 1)
	{
		out.u64("victim", e.who);
		out.u64("attacker", e.other);
		out.strid("part", e.part.empty() ? 0 : strings_.intern(4, e.part));
		out.f32("x", e.pos[0]);
		out.f32("y", e.pos[1]);
		out.f32("z", e.pos[2]);
		out.u8("dir", e.dir);
		out.f32("cut", e.damage[0]);
		out.f32("blunt", e.damage[1]);
		out.f32("pierce", e.damage[2]);
		out.f32("stun", e.damage[3]);
		out.u8("result", e.result);
		out.f32("cutAbsorbed", e.absorbed[0]);
		out.f32("bluntAbsorbed", e.absorbed[1]);
	} else if (e.kind == 51) {
		out.u64("thief", e.who);
		out.u64("witness", e.other);
		out.u8("cause", e.result);
		out.u64("container", e.turret);
	} else if (e.kind == 52) {
		out.u64("raiser", e.who);
		out.u64("criminal", e.other);
		out.u8("crime", e.result);
		out.strid("faction", e.part.empty() ? 0 : sid(e.part));
		out.f32("x", e.pos[0]);
		out.f32("y", e.pos[1]);
		out.f32("z", e.pos[2]);
	} else if (e.kind == 50) {
		out.u64("assassin", e.other);
		out.u64("victim", e.who);
		out.u8("result", e.result);
		out.f32("chance", e.chance);
	} else if (e.kind == 49) {
		out.u64("shooter", e.other);
		out.u64("target", e.who);
		out.f32("fromX", e.pos[0]);
		out.f32("fromY", e.pos[1]);
		out.f32("fromZ", e.pos[2]);
		out.f32("toX", e.shotTo[0]);
		out.f32("toY", e.shotTo[1]);
		out.f32("toZ", e.shotTo[2]);
		out.u8("result", e.result);
		out.strid("weapon", sid(e.part));
		out.u64("turret", e.turret);
	} else if (e.kind == 6 || e.kind == 7) {
		out.u64("id", e.who);
		out.strid("item", sid(e.part));
		out.f32("x", e.pos[0]);
		out.f32("y", e.pos[1]);
		out.f32("z", e.pos[2]);
	} else if (e.kind == 2 || e.kind == 16 || e.kind == 17) {
		if (e.kind == 16)
			out.u64("id", e.who);
		out.f32("x", e.pos[0]);
		out.f32("y", e.pos[1]);
		out.f32("z", e.pos[2]);
		if (e.kind == 17)
			out.u8("dir", e.dir);
		out.f32(e.kind == 16 ? "g" : "amount", e.damage[0]);
	} else if (e.kind == 14) {
		out.u64("id", e.who);
		out.u64("container", e.other);
		out.strid("owner", sid(e.part));
	} else if (e.kind == 13) {
		out.u64("id", e.who);
		out.u64("target", e.other);
		out.u8("action", e.action);
		out.u8("result", e.result);
	} else if (e.kind == 40) {
		out.u64("trader", e.who);
		out.u8("reason", e.result);
	} else if (e.kind == 20) {
		out.u64("char", e.who);
		out.u64("door", e.other);
		out.u8("success", e.result);
	} else if (e.kind == 19) {
		out.strid("town", strings_.intern(1, e.part));
	} else if (e.kind == 18) {
		out.u64("id", e.who);
		out.strid("faction", strings_.intern(2, e.part));
		out.u64("squadId", e.other);
	} else if (e.kind == 47) {
		out.u64("id", e.who);
		out.strid("part", strings_.intern(4, e.part));
	} else if (e.kind == 48) {
		// The ragdoll start (task BG): id, cause 0 KO / 1 death / 2 drop/put-down / 3 launch, the push direction (unit, world
		// frame; 0,0,0 = a collapse), the strength (u/s at the hit part; 0 = a collapse), the hit part (kind 4, 0 none) and
		// the client's flop seed. Right after the KO / death event of the same tick.
		out.u64("id", e.who);
		out.u8("cause", e.result);
		out.f32("dirX", e.ragdollDir[0]);
		out.f32("dirY", e.ragdollDir[1]);
		out.f32("dirZ", e.ragdollDir[2]);
		out.f32("strength", e.strength);
		out.strid("part", e.part.empty() ? 0 : strings_.intern(4, e.part));
		out.u32("seed", e.seed);
	} else if (e.kind == 15) {
		out.u64("observer", e.who);
		out.u64("target", e.other);
	} else if (e.kind == 41) {
		out.u64("speaker", e.who);
		out.u64("target", e.other);
		out.strid("dialogue", sid(e.dialogue));
		out.strid("line", sid(e.line));
		out.strid("text", e.text.empty() ? 0 : strings_.intern(4, e.text));
		out.f32("duration", e.damage[0]);
		out.u8("flags", e.result);
	} else if (e.kind == 42) {
		out.u64("speaker", e.who);
	} else if (e.kind == 43) {
		out.u64("speaker", e.who);
		out.u64("char", e.other);
		out.u32("menuSeq", e.seq);
		out.u8("flags", e.result);
		out.u8("n", static_cast<std::uint8_t>(std::min<std::size_t>(e.options.size(), 255)));
		for (std::size_t k = 0; k < e.options.size() && k < 255; ++k)
		{
			out.strid("line", sid(e.options[k].first));
			out.strid("text", strings_.intern(4, e.options[k].second));
		}
	} else if (e.kind == 44) {
		out.u64("speaker", e.who);
		out.u64("char", e.other);
		out.u8("reason", e.result);
	} else {
		out.u64("id", e.who);
		if (e.kind == 12)
			out.f32("distance", e.damage[0]);
	}
	out.end();
}

std::vector<Session::Box> Session::computeBounds() const
{
	std::vector<Box> boxes;
	if (const state::ViewFocus* view = sim_.world().ecs().get<state::ViewFocus>())
	{
		const kworld::ZoneCell cell = kworld::zoneCellOf(view->x, view->z);
		boxes.push_back({f(kworld::zoneMinX(cell.x - 1)), f(kworld::zoneMinZ(cell.z - 1)),
			f(kworld::zoneMinX(cell.x + 2)), f(kworld::zoneMinZ(cell.z + 2))});
	}
	if (sim_.combat() != nullptr)
	{
		for (const CombatArena::Member& m : sim_.combat()->members())
		{
			if (!m.fighter.playerControlled || m.fighter.dead)
				continue;
			// Centre snapped to a grid so the set changes rarely (SIMLINK.md section 6).
			const double cx = std::floor(m.fighter.x / kBubbleGrid + 0.5) * kBubbleGrid;
			const double cz = std::floor(m.fighter.z / kBubbleGrid + 0.5) * kBubbleGrid;
			const Box box{f(cx - kBubbleHalf), f(cz - kBubbleHalf), f(cx + kBubbleHalf), f(cz + kBubbleHalf)};
			if (std::find(boxes.begin(), boxes.end(), box) == boxes.end())
				boxes.push_back(box);
		}
	}
	return boxes;
}

bool Session::inBounds(const std::vector<Box>& boxes, double x, double z) const
{
	for (const Box& b : boxes)
	{
		if (x >= b.minX && x <= b.maxX && z >= b.minZ && z <= b.maxZ)
			return true;
	}
	return false;
}

bool Session::inInterest(double x, double z) const
{
	if (hasInterest_)
		return std::hypot(x - interestX_, z - interestZ_) <= interestR_;
	bool anyPlayer = false;
	for (const CombatArena::Member& m : sim_.combat()->members())
	{
		if (!m.fighter.playerControlled)
			continue;
		anyPlayer = true;
		if (std::hypot(x - m.fighter.x, z - m.fighter.z) <= kDefaultInterest)
			return true;
	}
	return !anyPlayer;
}

namespace
{

// NPC rows change all the time; their frames are paced to one per sim-second (20 ticks).
constexpr std::uint64_t kTaskNpcIntervalTicks = 20;

// Whether the player's (or, with `player` false, the others') rows differ from what the client extrapolates from `sent`: the count,
// an id or kind, the rate or value, or the progress. A row whose rate is 0 (a build's hp) is on a 1% tolerance, so it does not send
// a frame every tick.
bool taskRowsDiffer(const std::vector<Simulation::TaskProgressRow>& now, const std::vector<Simulation::TaskProgressRow>& sent, float elapsed, bool player)
{
	const auto mine = [&](const Simulation::TaskProgressRow& row) { return ((row.flags & Simulation::kProgressPlayer) != 0) == player; };
	std::size_t a = 0;
	std::size_t b = 0;
	for (;;)
	{
		while (a < now.size() && !mine(now[a]))
			++a;
		while (b < sent.size() && !mine(sent[b]))
			++b;
		if (a == now.size() || b == sent.size())
			return (a == now.size()) != (b == sent.size());
		const Simulation::TaskProgressRow& x = now[a++];
		const Simulation::TaskProgressRow& y = sent[b++];
		if (x.id != y.id || x.kind != y.kind || x.flags != y.flags || x.target != y.target || std::fabs(x.rate - y.rate) > 1e-6f
			|| std::fabs(x.value - y.value) > 1e-6f
			|| std::fabs(x.progress - (y.progress + y.rate * elapsed)) > (x.rate == 0.0f ? 0.01f : 1e-3f))
			return true;
	}
}

}

void Session::stampEvents()
{
	const std::uint64_t tick = sim_.clockTick();
	for (auto it = observed_.rbegin(); it != observed_.rend() && it->tick == 0; ++it)
		it->tick = tick;
}

void Session::sendTick(RecordWriter& out, bool initial)
{
	KSIM_PROFILE_SCOPE(sim_.profiler(), Simlink);
	if (resendDue_)
	{
		// Another session loaded a save (SIMLINK.md SAVES): this client gets the connect snapshot again.
		resendDue_ = false;
		resetKnown();
		debug_.reset(); // a load drops the debug view's pending answers
		initial = true;
	}
	if (savesDue_)
	{
		savesDue_ = false;
		if (!ctx_.saveDir.empty())
			writeSaves(out);
	}
	// NEW_GAME answers: this session's command applied in the ticks since the last call. The members' look (their faction) is
	// registered before the snapshot below describes them.
	for (const NewGameOutcome& outcome : newGameOutcomes_)
	{
		if (outcome.reason.empty())
		{
			for (const std::uint64_t member : outcome.members)
				ctx_.looks[member].factionSid = outcome.factionSid;
			progressLevel_ = 1; // the client sends order 98 again once the new game has started
			ctx_.cameraStart.reset(); // a new game is not the import
		}
		writeNewGameResult(out, outcome);
	}
	newGameOutcomes_.clear();
	const std::uint64_t tick = sim_.clockTick();
	// Whether a multiple of `interval` lies in (the last send's tick, this tick]: with one tick per send that is `tick % interval == 0`.
	const auto crossed = [&](std::uint64_t interval) { return tick / interval != lastSendTick_ / interval; };
	const CombatArena* arena = sim_.combat();

	// ---- plan: decide what goes out and update what the client knows ----
	std::vector<Box> bounds = computeBounds();
	const bool sendBounds = initial || !boundsSent_ || bounds != sentBounds_;
	if (sendBounds)
	{
		sentBounds_ = bounds;
		boundsSent_ = true;
	}
	struct Entry
	{
		const CombatArena::Member* member;
		std::uint32_t version;
		bool appearance;
		bool route;
		std::uint32_t routeSeq; // the route's seq on the wire: the live one, or the last sent one for a clear
		bool medical;
	};
	squadOf_ = &sim_.characterSquads();
	// PLAYER_SQUADS: on connect and whenever the squads, names, members, counts or limits differ from what the client heard.
	PlayerSquadsView squadsNow = sim_.playerSquads();
	const bool sendPlayerSquads = squadsNow.faction != 0 && (initial || !squadsKnown_ || !(squadsNow == squadsSent_));
	if (sendPlayerSquads)
	{
		squadsSent_ = squadsNow;
		squadsKnown_ = true;
	}
	// GAME_OPTIONS: on connect and whenever any value differs from what this session heard (order 121, NEW_GAME, a debug option, a load, new_world).
	GameOptionsView optionsNow;
	if (const state::WorldOptions* world = sim_.worldOptions())
	{
		optionsNow.flags = (world->feedAnimals ? 1 : 0) | (world->shareFood ? 2 : 0);
		optionsNow.chanceOfDeath = static_cast<float>(world->chanceOfDeath);
		optionsNow.damageScale = static_cast<float>(world->damageScale);
		optionsNow.hungerTime = static_cast<float>(world->hungerTime);
	}
	const bool sendOptions = initial || !gameOptionsKnown_ || !(optionsNow == gameOptionsSent_);
	if (sendOptions)
	{
		gameOptionsSent_ = optionsNow;
		gameOptionsKnown_ = true;
	}
	// UNLOCKS: on connect and whenever the completed research changed (the building list derives from it, so it is built only then).
	std::vector<std::string> researchNow = sim_.completedResearch();
	const bool sendUnlocks = initial || (unlocksKnown_ && researchNow != unlocksSent_);
	std::vector<std::string> buildingsNow;
	if (sendUnlocks)
	{
		buildingsNow = sim_.unlockedBuildings(researchNow);
		unlocksSent_ = researchNow;
		unlocksKnown_ = true;
	}
	// DISCOVERED: on connect and whenever the list differs; a place new to the client (not on connect) with type != 0 also gets event 19.
	std::vector<Simulation::DiscoveredPlace> discoveredNow = sim_.discoveredPlaces();
	const bool sendDiscovered = initial || (discoveredKnown_ && discoveredNow != discoveredSent_);
	if (sendDiscovered)
	{
		if (!initial)
		{
			for (const Simulation::DiscoveredPlace& place : discoveredNow)
			{
				if (place.type == 0 || std::any_of(discoveredSent_.begin(), discoveredSent_.end(), [&](const Simulation::DiscoveredPlace& old) { return old.sid == place.sid; }))
					continue;
				Observed o;
				o.kind = 19;
				o.part = place.sid;
				observed_.push_back(std::move(o));
			}
		}
		discoveredSent_ = discoveredNow;
		discoveredKnown_ = true;
	}
	// TASK_PROGRESS: on connect and whenever the list differs from what the client would extrapolate from the last send (a start, an
	// end, a rate or value change, or a lockpick roll resetting progress, which also covers a roll with an identical interval).
	std::vector<Simulation::TaskProgressRow> tasksNow;
	if (progressLevel_ != 0)
	{
		tasksNow = sim_.taskProgress(progressLevel_ >= 3);
		std::erase_if(tasksNow, [&](const Simulation::TaskProgressRow& row)
		{
			const bool player = (row.flags & Simulation::kProgressPlayer) != 0;
			if (progressLevel_ == 1)
				return !player || (row.flags & Simulation::kProgressOriginal) == 0;
			return !player && (progressLevel_ < 3 || known_.count(row.id) == 0);
		});
	}
	bool sendTasks = initial || taskForce_;
	taskForce_ = false;
	if (!sendTasks && taskKnown_)
	{
		const float elapsed = static_cast<float>(static_cast<double>(ticksSince(tick, taskSentTick_)) * kTickSeconds);
		sendTasks = taskRowsDiffer(tasksNow, taskSent_, elapsed, true)
			|| (ticksSince(tick, taskSentTick_) >= kTaskNpcIntervalTicks && taskRowsDiffer(tasksNow, taskSent_, elapsed, false));
	}
	if (sendTasks)
	{
		taskSent_ = tasksNow;
		taskSentTick_ = tick;
		taskKnown_ = true;
	}
	// FACTION_RELATIONS: on connect, then once per sim-second when the set changed or a value moved by 0.5 or more since the last send.
	std::vector<Simulation::FactionRelationRow> relationsNow;
	bool sendRelations = initial;
	if (initial || (relationsKnown_ && crossed(Simulation::kFactionRelationsIntervalTicks)))
	{
		relationsNow = sim_.factionRelations();
		sendRelations = initial || relationsNow.size() != relationsSent_.size();
		for (std::size_t i = 0; !sendRelations && i < relationsNow.size(); ++i)
			sendRelations = relationsNow[i].sid != relationsSent_[i].sid
				|| std::fabs(relationsNow[i].value - relationsSent_[i].value) >= Simulation::kFactionRelationsStep;
	}
	if (sendRelations)
	{
		relationsSent_ = relationsNow;
		relationsKnown_ = true;
	}
	// TOWN_STATE: on connect, then checked once per sim-second, sent only on change and at most one frame per 5 sim-seconds.
	std::vector<Simulation::TownStateRow> townNow;
	bool sendTown = false;
	if (initial || crossed(Simulation::kTownStateIntervalTicks))
	{
		townNow = sim_.townStates();
		sendTown = townNow != townSent_ && (initial || !townKnown_ || ticksSince(tick, townSentTick_) >= Simulation::kTownStateMinGapTicks);
	}
	if (sendTown)
	{
		townSent_ = townNow;
		townSentTick_ = tick;
		townKnown_ = true;
	}
	// JOBS: every player character's list on connect; later only the characters whose list or `enabled` changed (SIMLINK.md section 6).
	std::vector<std::pair<std::uint64_t, JobsView>> jobsOut;
	if (arena != nullptr)
	{
		std::set<std::uint64_t> living;
		for (const CombatArena::Member& m : arena->members())
		{
			if (!m.fighter.playerControlled || m.fighter.dead)
				continue;
			living.insert(m.id);
			JobsView now = sim_.jobsOf(m.id);
			const auto sent = jobsSent_.find(m.id);
			if (initial || (sent == jobsSent_.end() ? !(now == JobsView{}) : !(sent->second == now)))
			{
				jobsSent_[m.id] = now;
				jobsOut.emplace_back(m.id, std::move(now));
			}
		}
		for (auto it = jobsSent_.begin(); it != jobsSent_.end();)
			it = living.count(it->first) != 0 ? std::next(it) : jobsSent_.erase(it);
	}
	// The turrets somebody mans (their keys): kind 12's state and its BUILDING row's workers.
	std::unordered_set<std::uint64_t> mounted;
	if (arena != nullptr)
	{
		for (const CombatArena::Member& m : arena->members())
			if (m.fighter.mount.mounted())
				mounted.insert(m.fighter.mount.building);
	}
	// BUILDING: on connect every building of the faction, later the ones whose version the client has not heard (a rate or status change
	// bumps it; hp alone does not: the client extrapolates progress from the rate).
	std::vector<Simulation::BuildingView> buildingsOut;
	{
		if (initial)
		{
			buildingsSent_.clear();
			producerSent_.reset();
		}
		for (Simulation::BuildingView& v : sim_.playerBuildings())
		{
			const auto sent = buildingsSent_.find(v.id);
			if (sent == buildingsSent_.end() || sent->second != v.version)
			{
				buildingsSent_[v.id] = v.version;
				buildingsOut.push_back(std::move(v));
			}
		}
		// A producer focus (order 87): its panel record, any owner, on focus and on change; nothing while it is outside the detail bounds.
		if (focusProducer_ != 0)
		{
			std::optional<Simulation::BuildingView> node = sim_.producerView(focusProducer_);
			if (node && inBounds(sentBounds_, node->x, node->z) && producerSent_ != node->version)
			{
				producerSent_ = node->version;
				buildingsOut.push_back(std::move(*node));
			}
		}
	}
	// Machines and placed containers inside the detail bounds, whoever owns them (kenshi-k16.5.26): the BUILDING row's id is the INTERACTABLE id of
	// the same thing (kind 4 producer, kind 12 machine, kind 3 container placement), so the join needs no new field. Sent on first sight and
	// whenever the row's version moves; the client drops it with the INTERACTABLE's event 11.
	{
		if (initial)
			machineRowsSent_.clear();
		// The version the row would carry, read from the entity before any row is built (the same sum the row's `version` is).
		std::unordered_map<std::uint64_t, std::uint32_t> producerVersion;
		sim_.world().each([&](const state::Id& id, const state::Producer& box, const state::Inventory& stock) { producerVersion[id.value] = box.version + stock.version; });
		const auto unchanged = [&](std::uint64_t id, std::uint32_t version)
		{
			const auto sent = machineRowsSent_.find(id);
			return sent != machineRowsSent_.end() && sent->second == version;
		};
		for (const MachineSite& site : sim_.world().machineSites())
		{
			if (!inBounds(sentBounds_, f(site.x), f(site.z)) || !inInterest(f(site.x), f(site.z)))
			{
				machineRowsSent_.erase(site.id);
				continue;
			}
			std::uint32_t version = mounted.count(site.key) != 0 ? 2 : 1;
			if (site.producerId != 0)
			{
				const auto live = producerVersion.find(site.producerId);
				if (live == producerVersion.end())
				{
					machineRowsSent_.erase(site.id);
					continue;
				}
				version = live->second;
			}
			if (unchanged(site.id, version))
				continue;
			if (std::optional<Simulation::BuildingView> row = sim_.machineView(site, mounted.count(site.key) != 0))
			{
				machineRowsSent_[site.id] = row->version;
				buildingsOut.push_back(std::move(*row));
			}
		}
		sim_.world().each([&](const state::Id& id, const state::Container& box, const state::Inventory& stock)
		{
			if (id.value >= machines::kMachineIdBase || box.key == 0 || box.piece != state::Container::kNoPiece || box.building != box.key)
				return; // a furnace (a machine), a scenario stand-in or a layout piece
			if (!inBounds(sentBounds_, f(box.x), f(box.z)) || !inInterest(f(box.x), f(box.z)))
			{
				machineRowsSent_.erase(id.value);
				return;
			}
			if (unchanged(id.value, box.version + stock.version))
				return;
			if (std::optional<Simulation::BuildingView> row = sim_.containerView(id.value))
			{
				machineRowsSent_[id.value] = row->version;
				buildingsOut.push_back(std::move(*row));
			}
		});
	}
	const std::shared_ptr<const prospect::Result> prospects = std::move(prospectDue_);
	prospectDue_.reset();
	std::vector<Entry> visible;
	struct LowEntry { const CombatArena::Member* member; std::uint32_t version; bool appearance; bool dynamic; };
	std::vector<LowEntry> lowVisible;
	std::vector<Observed> events = std::move(observed_);
	observed_.clear();
	const std::vector<std::pair<std::uint32_t, std::string>> late = std::move(lateRejects_);
	lateRejects_.clear();
	// A connect, reconnect or load: every open menu of a player-controlled target again, with the resend flag (DIALOGUE.md 13.4); it
	// freezes again.
	if (initial)
	{
		for (const Simulation::ReplyMenuEvent& menu : sim_.openMenus())
		{
			const state::Fighter* target = arena != nullptr ? arena->find(menu.target) : nullptr;
			if (target == nullptr || !target->playerControlled)
				continue;
			Observed o;
			o.kind = 43;
			o.who = menu.speaker;
			o.other = menu.target;
			o.seq = menu.menuSeq;
			o.result = 1 | 2 | (ctx_.dialogueFreeze ? 4 : 0);
			o.options = menu.options;
			holdMenu(menu.speaker);
			events.push_back(std::move(o));
		}
	}
	std::set<std::uint64_t> present;
	if (arena != nullptr)
	{
		for (const CombatArena::Member& m : arena->members())
		{
			present.insert(m.id);
			// The player's own characters are always sent, wherever the camera's interest is (SIMLINK.md section 5): interest
			// and the detail bounds filter only the others. A camera panned 2 km away must not empty the selection.
			// In interest but outside the bounds: sent as a low-detail figure at the squad rate (SIMLINK.md section 5).
			const bool inside = inBounds(sentBounds_, m.fighter.x, m.fighter.z);
			const bool low = !m.fighter.playerControlled && !inside && inInterest(m.fighter.x, m.fighter.z);
			const bool seen = m.fighter.playerControlled || low || (inside && inInterest(m.fighter.x, m.fighter.z));
			auto it = known_.find(m.id);
			if (!seen)
			{
				if (it != known_.end())
				{
					Observed left;
					left.kind = 11;
					left.who = m.id;
					events.push_back(left);
					known_.erase(it);
				}
				blood_.forget(m.id);
				continue;
			}
			const std::uint32_t version = lookVersion(m);
			const bool fresh = it == known_.end();
			if (fresh)
				it = known_.emplace(m.id, Known{0, m.fighter.unconscious}).first;
			else if (it->second.unconscious && !m.fighter.unconscious && !m.fighter.dead)
			{
				Observed wake;
				wake.kind = 4;
				wake.who = m.id;
				events.push_back(wake);
			}
			it->second.unconscious = m.fighter.unconscious;
			const bool appearance = it->second.version != version;
			it->second.version = version;
			const bool wasLow = it->second.low;
			it->second.low = low;
			if (low)
			{
				// Positions only: no route, medical, actions, stats or blood, and not in `visible` (the full-detail set).
				lowVisible.push_back({&m, version, appearance || fresh || !wasLow, fresh || !wasLow || (tick + m.id) % kSlowIntervalTicks == 0});
				continue;
			}
			// A route is sent when it is set or cleared (nothing for a clear the client never heard of). The clear carries the seq
			// of the route it clears: an order's route that arrives zeroes routeSeq (world_tasks.cpp freeMember reads it), so the
			// seq the client last heard is kept here.
			bool route = false;
			std::uint32_t routeSeq = 0;
			// The diag tail's status / blocker / blockerId / reason also send it (stuckSecs and replans do not).
			const RouteDiag diag = routeDiag(m);
			const bool diagChanged = !(diag == it->second.diagSent);
			if ((m.fighter.routeVersion != it->second.routeSent || diagChanged) && (m.fighter.playerControlled || ctx_.sendAiRoutes))
			{
				route = !m.fighter.route.empty() || it->second.routeSent != 0 || diag.status != 0;
				it->second.routeSent = m.fighter.routeVersion;
				it->second.diagSent = diag;
				if (!m.fighter.route.empty())
				{
					routeSeq = m.fighter.routeSeq;
					it->second.routeSeqSent = routeSeq;
				} else if (route)
				{
					routeSeq = it->second.routeSeqSent;
					it->second.routeSeqSent = 0;
				}
			}
			// CHAR_MEDICAL: on first sight and at 2.5 Hz (staggered by id so a crowd does not burst on one tick).
			// A send that steps several ticks (speed) sends when the bucket changed since the last send, not on tick%8 alone.
			const bool medical = fresh || wasLow || (tick + m.id) / kMedicalIntervalTicks != (it->second.medicalTick + m.id) / kMedicalIntervalTicks;
			if (medical)
				it->second.medicalTick = tick;
			visible.push_back({&m, version, appearance, route, routeSeq, medical});
			// Ground blood (events 2 drop, 16 pool, 17 smear): the exe's medical tick, ~1 s per character.
			{
				BloodInput in;
				in.id = m.id;
				in.x = m.fighter.x;
				in.y = sim_.navigation() != nullptr ? m.fighter.y : 0.0;
				in.z = m.fighter.z;
				in.rate = m.fighter.bleeding;
				in.dead = m.fighter.dead;
				in.unconscious = m.fighter.unconscious;
				in.down = m.fighter.down;
				in.carried = m.fighter.carriedBy != 0;
				bloodOut_.clear();
				blood_.observe(in, tick, bloodOut_);
				for (const BloodEvent& b : bloodOut_)
				{
					Observed o;
					o.kind = b.kind;
					o.who = b.id;
					for (int i = 0; i < 3; ++i)
						o.pos[i] = f(b.pos[i]);
					o.dir = b.dir;
					o.damage[0] = f(b.value);
					events.push_back(o);
				}
			}
			if (fresh)
				knownInventory_.erase(m.id); // back in view: the client dropped it with the character
		}
	}
	for (auto it = known_.begin(); it != known_.end();)
	{
		if (present.count(it->first) == 0)
		{
			Observed left;
			left.kind = 11;
			left.who = it->first;
			events.push_back(left);
			blood_.forget(it->first);
			it = known_.erase(it);
		} else {
			++it;
		}
	}
	std::optional<sensor::Context> sensorCtx; // built once, only when an observer has to be judged (CHAR_DETECT, SEEN_BY, DETECTED_BY)
	const auto nonAllied = [&](const state::Fighter& observer, const state::Fighter& sneaker)
	{
		if (!sensorCtx)
			sensorCtx = sim_.sensorContext();
		return !sensor::ally(*sensorCtx, observer, sneaker);
	};
	// CHAR_DETECT: the highest sneak meter of the non-allied observers on each player character in view, on change (and once when
	// it clears). State 1 (noticed) reports meter 1; 2 is the meter filling; the record carries the observer it is from.
	std::vector<DetectRecord> detects;
	for (const Entry& e : visible)
	{
		const state::Fighter& target = e.member->fighter;
		if (!target.playerControlled || target.dead)
			continue;
		DetectRecord best;
		best.id = e.member->id;
		for (const CombatArena::Member& o : arena->members())
		{
			if (o.fighter.dead || !nonAllied(o.fighter, target))
				continue;
			const state::SightRecord* r = perception::find(o.fighter, e.member->id);
			if (r == nullptr || r->state == 0)
				continue;
			const perception::Detection d = perception::detection(*r);
			const bool better = d.state == 1 ? best.state != 1 : (best.state == 0 || (best.state == 2 && d.meter > best.meter));
			if (!better)
				continue;
			best.meter = d.meter;
			best.state = d.state;
			best.by = o.id;
		}
		const int quantised = static_cast<int>(best.meter * 64.0f + 0.5f);
		const auto sent = detectSent_.find(best.id);
		const bool had = sent != detectSent_.end();
		if (best.state == 0)
		{
			if (had)
			{
				detects.push_back(best);
				detectSent_.erase(sent);
			}
			continue;
		}
		if (had && sent->second.first == best.state && sent->second.second == quantised)
			continue;
		detectSent_[best.id] = {best.state, quantised};
		detects.push_back(best);
	}
	for (auto it = detectSent_.begin(); it != detectSent_.end();)
	{
		it = known_.count(it->first) == 0 ? detectSent_.erase(it) : std::next(it); // left the client's view: it dropped the character
	}
	// SEEN_BY: for each sneaking player character in view, the non-allied characters in view that know it (the stealth KO's abort test,
	// stealthko::victimKnows), on change and once empty when the sneak ends or the set empties. Non-allied as CHAR_DETECT (kenshi-rquo).
	std::vector<SeenByRecord> seenBys;
	{
		if (initial)
			seenBySent_.clear();
		for (const Entry& e : visible)
		{
			const state::Fighter& target = e.member->fighter;
			if (!target.playerControlled)
				continue;
			SeenByRecord now;
			now.id = e.member->id;
			if (target.sneak && !target.dead)
			{
				for (const Entry& o : visible)
				{
					const state::Fighter& f = o.member->fighter;
					if (!f.playerControlled && !f.dead && nonAllied(f, target) && stealthko::victimKnows(f, now.id))
						now.by.push_back(o.member->id);
				}
				std::sort(now.by.begin(), now.by.end());
			}
			const auto sent = seenBySent_.find(now.id);
			if (sent == seenBySent_.end() ? now.by.empty() : sent->second == now.by)
				continue;
			if (now.by.empty())
				seenBySent_.erase(now.id);
			else
				seenBySent_[now.id] = now.by;
			seenBys.push_back(std::move(now));
		}
		for (auto it = seenBySent_.begin(); it != seenBySent_.end();)
		{
			it = known_.count(it->first) == 0 ? seenBySent_.erase(it) : std::next(it); // left the client's view: event 11 forgot it
		}
	}
	// DETECTED_BY: the stealth arrows' records. For each sneaking player character (sneak on, out of combat, up) in view, the observers in
	// view with an entry (sensor::detected: red/yellow, friendly through a force-notice), on change and once empty when the sneak ends.
	std::vector<DetectedByRecord> detectedBys;
	{
		if (initial)
			detectedBySent_.clear();
		for (const Entry& e : visible)
		{
			const state::Fighter& target = e.member->fighter;
			if (!target.playerControlled)
				continue;
			DetectedByRecord now;
			now.id = e.member->id;
			if (target.sneak && !target.dead && !target.unconscious && !perception::inCombat(target))
			{
				for (const Entry& o : visible)
				{
					const state::SightRecord* r = o.member->id == now.id ? nullptr : perception::find(o.member->fighter, now.id);
					if (r == nullptr || r->state == 0)
						continue;
					if (!sensorCtx)
						sensorCtx = sim_.sensorContext();
					const sensor::Detected d = sensor::detected(*sensorCtx, o.member->fighter, target, *r);
					if (d.state != 0)
						now.by.push_back({o.member->id, d.state, static_cast<std::uint8_t>(d.friendly ? 1 : 0)});
				}
				std::sort(now.by.begin(), now.by.end(), [](const auto& a, const auto& b) { return a.observer < b.observer; });
			}
			const auto sent = detectedBySent_.find(now.id);
			if (sent == detectedBySent_.end() ? now.by.empty() : sent->second == now.by)
				continue;
			if (now.by.empty())
				detectedBySent_.erase(now.id);
			else
				detectedBySent_[now.id] = now.by;
			detectedBys.push_back(std::move(now));
		}
		for (auto it = detectedBySent_.begin(); it != detectedBySent_.end();)
		{
			it = known_.count(it->first) == 0 ? detectedBySent_.erase(it) : std::next(it); // left the client's view: event 11 forgot it
		}
	}
	// CAGES (kenshi-k16.5.7): the caged characters in view, the full list, on connect and on change (put in / let out, the lock opened or shut, in or out
	// of view). The cheap signature (id, cage, lock opened) is checked every tick; the rows (the door test walks the doors) are rebuilt when it changes
	// and once per squad interval, and sent when a row differs from the last send beyond its hardness (a cut in progress does not resend).
	std::vector<Simulation::CageView> cagesNow;
	std::vector<std::uint64_t> cageIds;
	bool sendCages = false;
	if (arena != nullptr)
	{
		std::vector<std::tuple<std::uint64_t, std::uint64_t, bool>> signature;
		for (const CombatArena::Member& m : arena->members())
			if (m.fighter.cageBuilding != 0 && known_.count(m.id) != 0)
				signature.emplace_back(m.id, m.fighter.cageBuilding, sim_.cageLockOpened(m.fighter.cageBuilding));
		std::sort(signature.begin(), signature.end());
		if (initial || !cagesSent_.known || signature != cagesSent_.signature || crossed(Simulation::kSquadIntervalTicks))
		{
			for (const auto& [id, cage, opened] : signature)
			{
				Simulation::CageView view;
				if (!sim_.cageView(cage, view))
					continue;
				cagesNow.push_back(std::move(view));
				cageIds.push_back(id);
			}
			const auto same = [](const Simulation::CageView& a, const Simulation::CageView& b)
			{ return a.cage == b.cage && a.locked == b.locked && a.level == b.level && a.maxHardness == b.maxHardness && a.record == b.record; };
			sendCages = initial || !cagesSent_.known || cageIds != cagesSent_.ids
				|| !std::equal(cagesNow.begin(), cagesNow.end(), cagesSent_.rows.begin(), cagesSent_.rows.end(), same);
			cagesSent_.signature = std::move(signature);
			if (sendCages)
			{
				cagesSent_.ids = cageIds;
				cagesSent_.rows = cagesNow;
				cagesSent_.known = true;
			}
		}
	}
	// CHAR_STATS: every player character of the session (in view or not) and the focus while it is in view; on first send, then on
	// change at most once per second (staggered by id).
	std::vector<std::pair<std::uint64_t, Simulation::CharStats>> charStatsOut;
	if (arena != nullptr)
	{
		for (const CombatArena::Member& m : arena->members())
		{
			const bool focused = m.id == focus_ && known_.count(m.id) != 0;
			if (!m.fighter.playerControlled && !focused)
			{
				if (m.id == focus_)
					charStatsSent_.erase(m.id); // out of view: the client shows empty frames; a full frame when it is back
				continue;
			}
			const auto sent = charStatsSent_.find(m.id);
			if (sent != charStatsSent_.end() && !charStatsNow_ && (tick + m.id) % kCharStatsIntervalTicks != 0)
				continue;
			Simulation::CharStats stats = sim_.charStats(m.id);
			if (sent != charStatsSent_.end() && sent->second == stats)
				continue;
			charStatsSent_[m.id] = stats;
			charStatsOut.emplace_back(m.id, std::move(stats));
		}
	}
	// Inventories of the player's characters in view: a record on first sight and whenever the version changes.
	std::vector<InvRecord> inventories;
	for (const Entry& e : visible)
	{
		if (!e.member->fighter.playerControlled)
			continue;
		sim_.world().each([&](const state::Id& id, const state::Inventory& inventory)
		{
			if (id.value != e.member->id)
				return;
			const auto known = knownInventory_.find(id.value);
			if (known != knownInventory_.end() && known->second == inventory.version)
				return;
			knownInventory_[id.value] = inventory.version;
			inventories.push_back({id.value, sim_.settledInventory(id.value)});
		});
	}
	// Containers in view: their contents are an INVENTORY record under the container's id, on first sight and on change.
	{
		sim_.world().each([&](const state::Id& id, const state::Container& box, const state::Inventory& inventory)
		{
			if (!inBounds(sentBounds_, box.x, box.z) || !inInterest(box.x, box.z))
			{
				knownInventory_.erase(id.value);
				return;
			}
			const auto known = knownInventory_.find(id.value);
			if (known != knownInventory_.end() && known->second == inventory.version)
				return;
			knownInventory_[id.value] = inventory.version;
			inventories.push_back({id.value, sim_.settledInventory(id.value)});
		});
	}
	// Producers in view: the output slot is an INVENTORY record under the producer's id, always on while it is in bounds (as containers).
	{
		sim_.world().each([&](const state::Id& id, const state::Producer& box, const state::Inventory& inventory)
		{
			if (!inBounds(sentBounds_, box.x, box.z) || !inInterest(box.x, box.z))
			{
				knownInventory_.erase(id.value);
				return;
			}
			const auto known = knownInventory_.find(id.value);
			if (known != knownInventory_.end() && known->second == inventory.version)
				return;
			knownInventory_[id.value] = inventory.version;
			inventories.push_back({id.value, sim_.settledInventory(id.value)});
		});
	}
	// Opened bodies (loot open, order 60 flags bit 0): their INVENTORY + ITEM_GRID follow every change while one of the session's
	// looters that opened them stays within the use range; when none does the feed stops (and a later open sends it afresh).
	for (auto open = openLoots_.begin(); open != openLoots_.end();)
	{
		const Simulation::Storage storage = sim_.findStorage(open->first);
		for (auto looter = open->second.begin(); looter != open->second.end();)
		{
			const state::Fighter* who = arena->find(*looter);
			if (who == nullptr || who->dead || who->unconscious || storage.entity == 0 || !Simulation::inStorageReach(*who, storage))
				looter = open->second.erase(looter);
			else
				++looter;
		}
		if (open->second.empty())
		{
			knownInventory_.erase(open->first);
			stealSent_.erase(open->first);
			open = openLoots_.erase(open);
			continue;
		}
		const flecs::entity opened(sim_.world().ecs().c_ptr(), storage.entity);
		const state::Inventory* held = opened.get<state::Inventory>();
		const auto known = knownInventory_.find(open->first);
		if (held != nullptr)
		{
			// The steal percents are the first looter's (the lowest id; a set keeps them ordered); they are resent (ITEM_GRID alone) when they change.
			const std::uint64_t actor = *open->second.begin();
			const bool fresh = known == knownInventory_.end() || known->second != held->version;
			InvRecord record{open->first, sim_.settledInventory(open->first)};
			record.steal = stealTail(actor, record);
			auto& sent = stealSent_[open->first];
			const bool changed = sent.first != actor || sent.second != record.steal;
			if (fresh || changed)
			{
				sent = {actor, record.steal};
				knownInventory_[open->first] = held->version;
				record.gridOnly = !fresh;
				inventories.push_back(std::move(record));
			}
		}
		++open;
	}
	// Open trades (trade.hpp): the stock's INVENTORY + ITEM_GRID follow its version, a TRADE frame follows either inventory's version or
	// the trader's wallet. A trade closes (event 40, reason 2) when the trader is gone, down or out of the detail bounds; when it
	// does the feed just stops (no event 11 for the squad id).
	std::vector<TradeOut> tradesOut;
	for (auto trade = trades_.begin(); trade != trades_.end();)
	{
		OpenTrade& open = trade->second;
		const state::Fighter* trader = arena != nullptr ? arena->find(open.trader) : nullptr;
		Simulation::TradeView view;
		if (trader == nullptr || !inBounds(sentBounds_, trader->x, trader->z) || !sim_.tradeView(trade->first, open.trader, open.window, view))
		{
			Observed closed;
			closed.kind = 40;
			closed.who = open.trader;
			closed.result = 2;
			events.push_back(closed);
			knownInventory_.erase(open.squad);
			trade = trades_.erase(trade);
			continue;
		}
		const auto known = knownInventory_.find(open.squad);
		if (known == knownInventory_.end() || known->second != view.stockVersion)
		{
			knownInventory_[open.squad] = view.stockVersion;
			inventories.push_back({open.squad, sim_.settledInventory(open.squad)});
		}
		std::vector<std::pair<std::uint64_t, std::uint32_t>> chests;
		for (const Simulation::TradeView::ChestPrices& chest : view.chests)
			chests.emplace_back(chest.container, chest.version);
		if (!open.sent || open.stockVersion != view.stockVersion || open.actorVersion != view.actorVersion || open.money != view.traderMoney
			|| open.chests != chests)
		{
			open.chests = std::move(chests);
			open.sent = true;
			open.stockVersion = view.stockVersion;
			open.actorVersion = view.actorVersion;
			open.money = view.traderMoney;
			tradesOut.push_back({trade->first, open.trader, std::move(view)});
		}
		++trade;
	}
	// Interactables (doors, ground items): a record on first sight and whenever the version changes; leaving the
	// detail bounds, or being destroyed, is event 11 like a character.
	std::vector<InterRecord> interactables;
	{
		const bool navigation = sim_.navigation() != nullptr;
		// Only what the client has not heard is built into a record: each entity is checked against the bounds and the version the
		// client holds first (the world has thousands of doors, items and beds, and a record each per tick was most of this block).
		const std::uint64_t pass = ++interPass_;
		std::vector<std::uint64_t> leaving;
		const auto visit = [&](std::uint64_t id, float x, float z, std::uint32_t version, const auto& make)
		{
			const auto it = knownInter_.find(id);
			if (it != knownInter_.end())
				it->second.pass = pass; // still in the world
			if (!inBounds(sentBounds_, x, z) || !inInterest(x, z))
			{
				if (it != knownInter_.end())
				{
					leaving.push_back(id);
					knownInter_.erase(it);
				}
				return;
			}
			if (it != knownInter_.end() && it->second.version == version)
				return;
			knownInter_[id] = InterKnown{version, pass};
			interactables.push_back(make());
		};
		sim_.world().each([&](const state::Id& id, const state::Door& door)
		{
			visit(id.value, f(door.x), f(door.z), door.version, [&]()
			{
				InterRecord r;
				r.id = id.value;
				r.kind = kInteractableDoor;
				r.record = door.recordSid;
				r.pos[0] = r.use[0] = f(door.x);
				r.pos[1] = r.use[1] = navigation ? f(door.y) : 0.0f;
				r.pos[2] = r.use[2] = f(door.z);
				r.state = door.state;
				r.health = f(door.health);
				r.maxHealth = f(door.maxHealth);
				r.version = door.version;
				r.owner = door.ownerSid;
				r.lockLevel = static_cast<std::uint16_t>(std::clamp(door.lockLevel, 0, 65535));
				return r;
			});
		});
		sim_.world().each([&](const state::Id& id, const state::GroundItem& item)
		{
			visit(id.value, f(item.x), f(item.z), item.version, [&]()
			{
				InterRecord r;
				r.id = id.value;
				r.kind = kInteractableItem;
				r.record = item.itemSid;
				r.pos[0] = f(item.x);
				r.pos[1] = navigation ? f(item.y) : 0.0f;
				r.pos[2] = f(item.z);
				r.use[0] = f(item.useX);
				r.use[1] = navigation ? f(item.useY) : 0.0f;
				r.use[2] = f(item.useZ);
				r.count = static_cast<std::uint16_t>(std::min<std::uint32_t>(item.count, 65535));
				r.version = item.version;
				r.owner = item.ownerSid;
				r.model = item.quality.model;
				if (item.tint.set)
				{
					r.limbTint = true;
					r.skin[0] = f(item.tint.skinR);
					r.skin[1] = f(item.tint.skinG);
					r.skin[2] = f(item.tint.skinB);
					r.blood[0] = f(item.tint.bloodR);
					r.blood[1] = f(item.tint.bloodG);
					r.blood[2] = f(item.tint.bloodB);
					r.scale[0] = f(item.tint.scaleX);
					r.scale[1] = f(item.tint.scaleY);
					r.scale[2] = f(item.tint.scaleZ);
				}
				// The rest yaw: seeded by the item's Id, so it is fixed for the item's life with nothing saved (OURS: no drop physics).
				r.yaw = static_cast<float>(RngStream(streamSeed(id.value, kRestYawStream)).uniform01() * 6.283185307179586);
				return r;
			});
		});
		sim_.world().each([&](const state::Id& id, const state::Container& box)
		{
			if (id.value >= machines::kMachineIdBase)
				return; // a furnace: kind 12 below
			visit(id.value, f(box.x), f(box.z), box.version, [&]()
			{
				InterRecord r;
				r.id = id.value;
				r.kind = kInteractableContainer;
				r.record = box.recordSid;
				r.pos[0] = f(box.x);
				r.pos[1] = navigation ? f(box.y) : 0.0f;
				r.pos[2] = f(box.z);
				r.use[0] = f(box.useX);
				r.use[1] = navigation ? f(box.useY) : 0.0f;
				r.use[2] = f(box.useZ);
				// The door's lock encoding: state 2 when the box is locked (its lockLevel follows), 0 when not (SIMLINK.md section 13).
				r.state = box.locked ? 2 : 0;
				r.lockLevel = box.locked ? static_cast<std::uint16_t>(std::clamp(box.lockLevel, 0, 65535)) : 0;
				r.version = box.version;
				r.owner = box.ownerSid;
				r.building = box.building;
				r.piece = box.piece;
				return r;
			});
		});
		sim_.world().each([&](const state::Id& id, const state::Producer& box, const state::Inventory& stock)
		{
			// The record follows the producer's own version and the stock's (both only grow).
			visit(id.value, f(box.x), f(box.z), box.version + stock.version, [&]()
			{
				InterRecord r;
				r.id = id.value;
				r.kind = kInteractableProducer;
				r.record = box.recordSid;
				r.pos[0] = f(box.x);
				r.pos[1] = navigation ? f(box.y) : 0.0f;
				r.pos[2] = f(box.z);
				r.use[0] = f(box.useX);
				r.use[1] = navigation ? f(box.useY) : 0.0f;
				r.use[2] = f(box.useZ);
				r.state = box.prodState;
				r.count = static_cast<std::uint16_t>(std::min<std::uint32_t>(stock.carried(box.output), 65535));
				r.version = box.version + stock.version;
				r.output = box.output;
				r.cap = static_cast<std::uint16_t>(std::min<std::uint32_t>(box.cap, 65535));
				r.operators = static_cast<std::uint8_t>(std::min<std::size_t>(box.operators.size(), 255));
				r.maxOperators = box.maxOperators;
				r.owner = box.ownerSid;
				// Progress to the next unit and its rate at this tick; the client extrapolates (a unit, a rate change, full: the
				// version moves and the record is sent again). Full or unoperated: rate 0.
				r.progress = static_cast<float>(box.progress);
				r.rate = stock.carried(box.output) >= box.cap ? 0.0f : static_cast<float>(sim_.producerRate(box));
				return r;
			});
		});
		// Machines (kind 12, kenshi-k16.5.26): every placed machine that is no producer entity. The id is machines::kMachineIdBase + the
		// site's index, the same on every load. A turret's version moves when somebody mounts or leaves it; a furnace follows its box.
		{
			for (const MachineSite& site : sim_.world().machineSites())
			{
				if (site.producerId != 0)
					continue;
				const machines::Static& info = sim_.world().machineInfo(site);
				const bool manned = info.kind == machines::Kind::Turret && mounted.count(site.key) != 0;
				visit(site.id, f(site.x), f(site.z), manned ? 2u : 1u, [&]()
				{
					InterRecord r;
					r.id = site.id;
					r.kind = kInteractableMachine;
					r.record = site.recordSid;
					r.pos[0] = r.use[0] = f(site.x);
					r.pos[1] = r.use[1] = navigation ? f(site.y) : 0.0f;
					r.pos[2] = r.use[2] = f(site.z);
					r.machineKind = static_cast<std::uint8_t>(info.kind);
					r.state = manned || (info.maxWorkers == 0 && info.kind != machines::Kind::Turret) ? 1 : 0;
					r.version = manned ? 2 : 1;
					r.owner = site.ownerFaction;
					r.input = info.input;
					r.output = info.output;
					r.building = site.key;
					return r;
				});
			}
		}
		// Beds (kind 5, client-approved 2026-11-06): every real layout bed. The id is the layout placement's site key
		// (routines::siteId), never a runtime index, so it is the same across a save/load and a restart. The version toggles
		// with the occupant, so a take/leave re-sends the record; the client reads the occupant from it.
		for (const Simulation::BedView& bed : sim_.bedViewsRef())
		{
			visit(bed.id, f(bed.x), f(bed.z), bed.occupant != 0 ? 2 : 1, [&]()
			{
				InterRecord r;
				r.id = bed.id;
				r.kind = kInteractableBed;
				r.record = bed.record;
				r.pos[0] = f(bed.x); // the floor point: where the walk ends
				r.pos[1] = navigation ? f(bed.y) : 0.0f;
				r.pos[2] = f(bed.z);
				r.use[0] = f(bed.x); // the use node: where the body lies, a few units above the floor
				r.use[1] = navigation ? f(bed.useY) : 0.0f;
				r.use[2] = f(bed.z);
				r.version = bed.occupant != 0 ? 2 : 1;
				r.building = bed.building;
				r.furniture = bed.record;
				r.occupant = bed.occupant;
				r.yaw = static_cast<float>(bed.yaw);
				return r;
			});
		}
		std::sort(leaving.begin(), leaving.end());
		for (const std::uint64_t id : leaving)
		{
			Observed left;
			left.kind = 11;
			left.who = id;
			events.push_back(left);
		}
		std::sort(interactables.begin(), interactables.end(), [](const InterRecord& a, const InterRecord& b) { return a.id < b.id; });
		for (auto it = knownInter_.begin(); it != knownInter_.end();)
		{
			if (it->second.pass != pass)
			{
				Observed left;
				left.kind = 11;
				left.who = it->first;
				events.push_back(left);
				it = knownInter_.erase(it);
			} else {
				++it;
			}
		}
	}
	// Navmesh tile stream (SIMLINK.md section 12): the tiles that meet the detail bounds, nearest the first box's centre
	// first, at most kNavTileBudget bytes of tile data per tick (one tile at least); tiles that left the bounds go as size 0.
	std::vector<NavSend> navAdd; // whole tiles: new, or a patch chain that is cut
	std::vector<NavPatchSend> navPatch; // door repaints of tiles the client holds, one per version step
	std::vector<std::pair<int, int>> navRemove;
	bool navParams = false;
	if (sim_.navigation() != nullptr && boundsSent_ && !sentBounds_.empty()
		&& (navPending_ || navRevision_ != sim_.navigation()->mesh().revision() || navBounds_ != sentBounds_))
	{
		const knav::NavMesh& mesh = sim_.navigation()->mesh();
		navRevision_ = mesh.revision(); // nothing below changes until the mesh or the bounds do, unless the byte budget cut the pass short
		navBounds_ = sentBounds_;
		const knav::NavConfig& cfg = mesh.config();
		const double size = cfg.tileSize;
		navParams = !navParamsSent_;
		navParamsSent_ = true;
		const double cx = (sentBounds_[0].minX + sentBounds_[0].maxX) * 0.5;
		const double cz = (sentBounds_[0].minZ + sentBounds_[0].maxZ) * 0.5;
		std::set<std::pair<int, int>> wanted;
		std::vector<std::pair<double, const knav::BakedTile*>> fresh;
		std::vector<std::pair<double, const knav::BakedTile*>> stale; // held, but the server's tile moved on and the chain is cut
		for (const knav::BakedTile& tile : mesh.tiles())
		{
			const double x0 = cfg.originX + tile.x * size;
			const double z0 = cfg.originZ + tile.z * size;
			bool inside = false;
			for (const Box& b : sentBounds_)
				inside = inside || (x0 < b.maxX && x0 + size > b.minX && z0 < b.maxZ && z0 + size > b.minZ);
			if (!inside)
				continue;
			wanted.insert({tile.x, tile.z});
			const auto held = navSent_.find({tile.x, tile.z});
			const double distance = std::hypot(x0 + size * 0.5 - cx, z0 + size * 0.5 - cz);
			if (held == navSent_.end())
			{
				fresh.push_back({distance, &tile});
				continue;
			}
			const std::uint32_t current = mesh.tileVersion(tile.x, tile.z);
			if (held->second == current)
				continue;
			// The client holds `held->second`: it needs every patch up to the current version, in order, each applied on
			// exactly version - 1. A cut chain (more than kMaxTilePatches steps behind, or a rebuild) is a whole tile again.
			const std::vector<knav::NavMesh::TilePatch>& patches = mesh.tilePatches(tile.x, tile.z);
			const bool chain = held->second < current && !patches.empty() && patches.front().version <= held->second + 1;
			if (!chain)
			{
				stale.push_back({distance, &tile});
				continue;
			}
			for (const knav::NavMesh::TilePatch& patch : patches)
			{
				if (patch.version > held->second)
					navPatch.push_back({tile.x, tile.z, &patch});
			}
			held->second = current;
		}
		std::stable_sort(fresh.begin(), fresh.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
		std::stable_sort(stale.begin(), stale.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
		fresh.insert(fresh.begin(), stale.begin(), stale.end()); // updates of what the client holds go first
		std::size_t budget = 0;
		navPending_ = false;
		for (const auto& [distance, tile] : fresh)
		{
			NavSend send{tile->x, tile->z, mesh.tileVersion(tile->x, tile->z), mesh.tileBlob(tile->x, tile->z)};
			if (!navAdd.empty() && budget + send.blob.size() > kNavTileBudget)
			{
				navPending_ = true;
				break;
			}
			budget += send.blob.size();
			navSent_[{tile->x, tile->z}] = send.version;
			navAdd.push_back(std::move(send));
		}
		for (auto it = navSent_.begin(); it != navSent_.end();)
		{
			if (wanted.count(it->first) == 0)
			{
				navRemove.push_back(it->first);
				it = navSent_.erase(it);
			} else {
				++it;
			}
		}
	}
	// Events about characters the client does not know (and never will this tick) are dropped.
	events.erase(std::remove_if(events.begin(), events.end(), [&](const Observed& e)
	{
		return e.kind != 11 && e.kind != 19 && e.kind != 20 && e.kind != 40 && e.kind != 43 && e.kind != 44 && known_.count(e.who) == 0 && known_.count(e.other) == 0; // 40 is the trade window's, sent whether or not the trader is still in view
	}), events.end());
	const bool slow = initial || crossed(kSlowIntervalTicks);
	lastSendTick_ = tick; // the last use of crossed()
	// A DEBUG_SET_TIME applied since the last send: its jump must show in this send (SIMLINK "DEBUG VIEW").
	const bool clockDue = slow || ctx_.pauseBits() != clockPausedSent_ || debug_.clockDue();
	// WEATHER: 1 Hz, and at once when any region picked a new weather.
	const std::uint64_t picks = sim_.weather() != nullptr ? sim_.weather()->picks() : 0;
	const bool sendWeather = sim_.weather() != nullptr && (slow || picks != weatherPicksSent_);
	if (sendWeather)
		weatherPicksSent_ = picks;
	const std::vector<BeamsOut> beamsNow = beamsDue(tick, initial, arena);

	// STARTS: on connect and resend, after a new world, and when the navigation mesh changed and a start's readiness with it.
	std::vector<Simulation::StartReady> starts;
	bool sendStarts = false;
	try
	{
		if (sim_.gameData() != nullptr)
		{
			const std::size_t key = sim_.navigation() != nullptr ? sim_.navigation()->mesh().tiles().size() : 0;
			if (initial || startsDue_ || key != startsKey_)
			{
				starts = sim_.startStatuses();
				sendStarts = initial || startsDue_ || starts != startsSent_;
				startsKey_ = key;
				startsDue_ = false;
				if (sendStarts)
					startsSent_ = starts;
			}
		}
	} catch (const std::exception& e) {
		if (ctx_.log)
			ctx_.log(std::string("STARTS: ") + e.what());
	}

	std::set<std::int32_t> playerTeams;
	if (arena != nullptr)
		for (const CombatArena::Member& m : arena->members())
			if (m.fighter.playerControlled)
				playerTeams.insert(m.fighter.team);

	// ---- emit: once into the null writer (interns strings), flush the table, then for real ----
	const auto emit = [&](RecordWriter& w)
	{
		w.begin(kSnapBegin);
		w.u64("tick", tick);
		w.u8("full", 1);
		w.end();
		if (sendBounds)
		{
			w.begin(kBounds);
			w.u64("tick", tick);
			w.list("boxes", sentBounds_.size(), 16);
			for (const Box& b : sentBounds_)
			{
				w.item();
				w.f32("minX", b.minX);
				w.f32("minZ", b.minZ);
				w.f32("maxX", b.maxX);
				w.f32("maxZ", b.maxZ);
			}
			w.listEnd();
			w.end();
		}
		if (navParams)
			writeNavParams(w, tick);
		for (const std::pair<int, int>& gone : navRemove)
			writeNavTile(w, tick, NavSend{gone.first, gone.second, 0, {}});
		for (const NavSend& tile : navAdd)
			writeNavTile(w, tick, tile);
		for (const NavPatchSend& patch : navPatch)
			writeNavPolyFlags(w, patch);
		for (const Entry& e : visible)
		{
			if (e.appearance)
				writeAppearance(w, tick, *e.member, e.version);
		}
		for (const LowEntry& e : lowVisible)
		{
			if (e.appearance)
				writeAppearance(w, tick, *e.member, e.version);
		}
		for (const Entry& e : visible)
			writeDynamic(w, tick, *e.member, e.version, playerTeams);
		for (const LowEntry& e : lowVisible)
		{
			if (e.dynamic)
				writeDynamic(w, tick, *e.member, e.version, playerTeams, true);
		}
		for (const Entry& e : visible)
		{
			if (e.route)
				writeRoute(w, tick, *e.member, e.routeSeq);
		}
		for (const Entry& e : visible)
		{
			if (e.medical)
				writeMedical(w, tick, *e.member);
		}
		for (const DetectRecord& d : detects)
			writeDetect(w, tick, d);
		for (const SeenByRecord& s : seenBys)
			writeSeenBy(w, tick, s);
		for (const DetectedByRecord& d : detectedBys)
			writeDetectedBy(w, tick, d);
		if (sendCages)
			writeCages(w, tick, cagesNow, cageIds);
		for (const auto& [id, stats] : charStatsOut)
			writeCharStats(w, tick, id, stats);
		for (const InterRecord& r : interactables)
			writeInteractable(w, tick, r);
		for (const InvRecord& r : inventories)
		{
			if (!r.gridOnly)
			{
				writeInventory(w, tick, r);
				writeItemQuality(w, tick, r);
			}
			writeItemGrid(w, tick, r);
			if (!r.gridOnly)
				writeItemLock(w, tick, r);
		}
		for (const TradeOut& t : tradesOut)
			writeTrade(w, tick, t);
		if (sendWeather)
			writeWeather(w, tick);
		if (!beamsNow.empty())
			writeBeams(w, tick, beamsNow);
		if (sendPlayerSquads)
			writePlayerSquads(w, tick, squadsNow);
		if (sendOptions)
			writeGameOptions(w, tick, optionsNow);
		if (sendUnlocks)
			writeUnlocks(w, tick, researchNow, buildingsNow);
		if (sendUnlocks)
			writeResearchState(w, tick, squadsNow.faction != 0, researchNow);
		if (sendDiscovered)
			writeDiscovered(w, tick, discoveredNow);
		if (sendRelations)
			writeFactionRelations(w, tick, relationsNow);
		if (sendTown)
			writeTownState(w, tick, townNow);
		if (sendTasks)
			writeTaskProgress(w, tick, tasksNow);
		if (!jobsOut.empty())
			writeJobs(w, tick, jobsOut);
		if (!buildingsOut.empty())
			writeBuildings(w, tick, buildingsOut);
		if (prospects != nullptr)
			writeProspect(w, tick, *prospects);
		if (slow)
			writeSquads(w, tick);
		if (clockDue)
			writeClock(w, tick);
		w.begin(kSnapEnd);
		w.u64("tick", tick);
		w.u8("full", 1);
		w.end();
		for (const Observed& e : events)
			writeEvent(w, tick, e);

		// The debug view (docs/formats/SIMLINK.md "DEBUG VIEW"): after SNAPSHOT_END, outside the bracket like events.
		debug_.write(w, tick, sim_, ctx_.squadLimit, [this](const std::string& text) { return sid(text); }, slow);

		if (sendStarts)
			writeStarts(w, starts);
	};
	NullWriter discard;
	emit(discard);
	flushStrings(out);
	if (ctx_.cameraStart && !cameraStartSent_)
	{
		cameraStartSent_ = true;
		const ServerContext::CameraStart& c = *ctx_.cameraStart;
		out.begin(kCameraStart);
		out.u8("flags", static_cast<std::uint8_t>((c.hasCamera ? 1 : 0) | (c.hasFloor ? 2 : 0)));
		for (const double v : c.pos)
			out.f64("pos", c.hasCamera ? v : 0.0);
		for (const double v : c.rot)
			out.f32("rot", c.hasCamera ? static_cast<float>(v) : 0.0f);
		out.f32("zoom", c.hasCamera ? static_cast<float>(c.zoom) : 0.0f);
		out.u8("floor", static_cast<std::uint8_t>(c.hasFloor ? std::clamp(c.floor, 0, 3) : 0));
		out.end();
	}
	emit(out);
	debug_.consumed();
	for (const auto& [seq, reason] : late)
		ackRejected(out, seq, reason);
}

void Session::sendDebugOnly(RecordWriter& out)
{
	// Paused: no tick runs, so the answers describe the current one. No SQUADS cadence runs, so a subscribe answered here carries its
	// DEBUG_SQUADS (the SQUADS set cannot have changed since the last SQUADS: nothing ticked); a paused F12 shot learns its squads from
	// it (kenshi-k16.2.104.1).
	if (!ctx_.debugView || !debugPeer_ || !debug_.hasWork())
		return;
	const std::uint64_t tick = sim_.clockTick();
	const bool squadsDue = debug_.infoDue();
	const auto emit = [&](RecordWriter& w)
	{
		debug_.write(w, tick, sim_, ctx_.squadLimit, [this](const std::string& text) { return sid(text); }, squadsDue);
	};
	NullWriter discard;
	emit(discard);
	flushStrings(out);
	emit(out);
	debug_.consumed();
}

void Session::holdMenu(std::uint64_t speaker)
{
	if (ctx_.dialogueFreeze && std::find(ctx_.menuHolds.begin(), ctx_.menuHolds.end(), speaker) == ctx_.menuHolds.end())
		ctx_.menuHolds.push_back(speaker);
}

void Session::releaseMenu(std::uint64_t speaker)
{
	ctx_.menuHolds.erase(std::remove(ctx_.menuHolds.begin(), ctx_.menuHolds.end(), speaker), ctx_.menuHolds.end());
}

} // namespace ksim::simlink
