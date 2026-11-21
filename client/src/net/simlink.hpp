#pragma once
#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// The live sim link, client half (docs/formats/SIMLINK.md, protocol 1). SimLinkDecoder takes the server's byte stream in any chunking (feed), reassembles
// frames and keeps the latest world state: string table, characters (appearance + the last two dynamic samples for interpolation), squads, clock, weather,
// full-detail bounds, plus the event queue. No socket here: the transport feeds it. OrderWriter builds the client -> server frames.
namespace simlink
{
	constexpr uint32_t kMagic = 0x4D49534B;	// 'KSIM'
	constexpr uint16_t kProtocol = 8;
	enum FrameType : uint16_t { kHello = 1, kStrtab = 2, kError = 3, kPing = 4, kSnapBegin = 10, kSnapEnd = 11, kAppearance = 20, kDynamic = 21, kMedical = 22, kRoute = 23, kInteractable = 24, kInventory = 25, kDetect = 26, kItemQuality = 27, kItemGrid = 28, kItemStats = 29, kSquads = 30, kPlayerSquads = 31, kSaves = 32, kStarts = 33, kJobs = 34, kProspect = 35, kUnlocks = 36, kDiscovered = 37, kFactionRelations = 38, kTaskProgress = 39, kBuilding = 47, kBuildingGrid = 48, kCharStats = 49,
		kClock = 40, kWeather = 41, kBounds = 42, kNavParams = 43, kNavTile = 44, kNavPolyFlags = 45, kEvent = 50, kTrade = 53, kScenarioList = 54, kScenarioStarted = 55, kScenarioMetrics = 56, kItemLock = 57, kBeams = 58, kSeenBy = 59, kDetectedBy = 60, kMenuEntries = 61, kNewGameResult = 62, kResearchState = 63, kCages = 64, kCameraStart = 65, kProfile = 70, kOrder = 100, kScenarioListReq = 102, kScenarioLoad = 103, kScenarioEnd = 104, kNewGame = 111 };
	enum Flag : uint32_t { kCombat = 1, kStealth = 2, kDrawn = 4, kKO = 8, kDead = 16, kProne = 32, kPlayer = 64, kCarryL = 128, kCarryR = 256, kYValid = 512,
		kLowDetail = 1024, kBlockMode = 2048, kRun = 4096, kHold = 8192,
		kHostile = 16384,	// hostile: the sim's fight test says it would fight this session's characters (never on our own)
		kSeated = 32768, kAsleep = 65536,	// on a layout seat / bed (RELAX); derived, unsaved
		kIndoors = 1u << 19,
		kAim = 1u << 20,
		kMounted = 1u << 21, kStrafeLeft = 1u << 22, kStrafeRight = 1u << 23,
		kPassive = 1u << 24, kHoldPosition = 1u << 25 };	// passive / hold position: order 10 which 6 / 7 (k16.6.313); NOT kHold (bit 13, the order-4 stand state).	// mounted: manning a turret, its doorKey in the mounted tail. kAim: the aim tail follows (a swinging technique's clip root aim)	// bits 17-18 are the speed mode: in a building's interior AND under a ceiling (Fighter::indoors), so a roof / top deck is clear (SIMLINK, sim 5ac25fbc)
	enum Section : uint8_t { kBack, kBack2, kHip, kHead, kShirt, kArmour, kLegs, kBoots, kBackpack, kHand };

	struct Item { uint32_t item = 0, spec = 0, colour = 0; uint8_t section = 0; };
	struct Appearance
	{
		uint64_t squad = 0;
		uint32_t faction = 0, race = 0, hair = 0, beard = 0, hairColour = 0;
		uint8_t sex = 0, hiddenLimbs = 0;
		uint16_t head = 0, crossbowAmmo = 0;
		float skin = 0;
		std::vector<std::pair<uint32_t, float>> body;
		std::vector<Item> items;
		uint32_t version = 0;
		uint32_t name = 0;	// strid of the display name (protocol 4; 0: none)
		uint32_t portraitSerial = 0;	// protocol 5: the portrait camera's seed (Portraits::serialYaw / serialPitch); 0: not sent
	};
	struct Dynamic
	{
		uint64_t tick = 0, carriedBy = 0, actionStart = 0, building = 0;
		float pos[3] = {}, yaw = 0, fwd = 0, lat = 0, legs[2] = { 100, 100 }, actionSpeed = 1, heading = 0;
		uint32_t flags = 0, action = 0, appearanceVersion = 0, driveSeq = 0;	// driveSeq: clientSeq of the last applied drive order (SIMLINK 12)
		uint8_t drive = 0;
		uint8_t actionFlags = 0;	// bit 0: loop `action` until it changes (an operate clip; SIMLINK CHAR_DYNAMIC 2026-11-02)
		int16_t floor = 0;
		// pose tail (sim e7340dc), only while seated / asleep: the snap point (bed use node, seat origin) and yaw, the pose clip (strid, never 0);
		// poseClip 0: an older server, the sim's own position and yaw
		float pose[4] = {};
		uint32_t poseClip = 0;
		// rest tail (sim BG), only while prone and not seated / asleep: the lying body's head-to-feet yaw (record yaw convention); x, y, z are the rest point
		float restYaw = 0;
		bool hasRest = false;
		// aim tail (sim bbedfbd0), only while flag kAim: the aim the technique's clip root is turned by (record yaw convention), fixed at its start
		float aimYaw = 0;
		bool hasAim = false;
		// mounted tail (sim 00f1d450), only while flag kMounted: the manned turret's placement, kworld::doorKey (the `building` key space)
		uint64_t turret = 0;
	};
	// CHAR_ROUTE (SIMLINK 6): the waypoints still ahead, the first being the next; empty = no route
	struct Route
	{
		uint32_t clientSeq = 0, version = 0;	// clientSeq 0: the AI's own route
		uint8_t flags = 0;	// bit 0 truncated (more than the cap), bit 1 approach point (not the clicked spot)
		std::vector<std::array<float, 3>> points;
		// The leading tick of the CHAR_ROUTE frame that set this (kenshi-k16.2.231.1, the shot route sidecar): the sim tick the route
		// arrived at; 0 before any route frame. A clear (n = 0) keeps its own tick and seq here.
		uint64_t arrivedTick = 0;
	};
	struct Character
	{
		bool hasAppearance = false;
		Route route;
		Appearance look;
		Dynamic prev, cur;	// the two newest samples (prev.tick == 0: only one so far)
		// The samples before prev, oldest first. kPastSamples 32 (kenshi-k16.2.74): the client draws a few ticks behind cur, and the startup clock
		// backlog eases in instead of snapping, so the history must reach back further than any lag the ease corrects (kClockEaseMax in simview.cpp).
		static constexpr size_t kPastSamples = 32;
		std::deque<Dynamic> past;
		int samples = 0;
	};
	// The navmesh tile stream (SIMLINK 12): the server's Detour bake for the full-detail area, for prediction and previews only
	struct NavParams { uint32_t meshId = 0, detourVersion = 0, maxTiles = 0, maxPolys = 0; float origin[3] = {}, tileWidth = 0, tileHeight = 0; bool valid = false; };
	struct NavTile { uint32_t version = 0, baked = 0; std::vector<uint8_t> data; };	// data: the Detour tile blob at version `baked`; version: after the patches since
	struct NavPoly { uint16_t poly, flags; uint8_t area; };
	struct NavPatch { std::array<int32_t, 3> key; uint32_t version; std::vector<NavPoly> polys; };	// NAV_POLY_FLAGS on a tile the decoder holds at the previous version
	// INTERACTABLE (SIMLINK 6): a door or a ground item; leaves with event 11
	struct Interactable
	{
		enum Kind : uint8_t { kDoor = 1, kItem = 2, kContainer = 3, kProducer = 4, kBed = 5 };	// container / producer: contents are an INVENTORY under its id
		enum State : uint8_t { kOpen = 0, kClosed = 1, kLocked = 2, kBroken = 3 };	// broken: bashed in, open for good
		enum ProdState : uint8_t { kFull = 1, kWorking = 2, kIdle = 3 };	// a producer's state (MINE_SELL_SLICE 3.1)
		uint64_t id = 0;
		uint8_t kind = 0, state = 0;
		uint32_t record = 0, version = 0;	// record: door, container: its BUILDING sid; item: its item sid
		float pos[3] = {}, use[3] = {}, health = 0, maxHealth = 0;
		uint16_t count = 0;	// a producer: units in its output
		uint32_t output = 0;	// producer tail: the produced item's strid, its cap (`produces` v0), operators working / the most allowed
		uint16_t cap = 0;
		uint8_t operators = 0, maxOperators = 0;
		float progress = 0, rate = 0;	// toward the next unit, 0..1; per sim second over all operators (0: full or idle)
		uint64_t progressTick = 0;	// the tick progress was taken: extrapolate progress + rate x elapsed, clamped at 1
		// ownership tail (sim a7a8704), every kind: owner = the FACTION strid (0 nobody's; compare SimView::ownFaction), a door's lock level,
		// a ground weapon's MATERIAL_SPECS_WEAPON strid, a ground item's rest yaw (radians); hasOwner false from an older server
		uint32_t owner = 0, model = 0;
		uint16_t lockLevel = 0;
		float yaw = 0;
		bool hasOwner = false;
		// bed tail (kind 5, sim ec678bc7): the owning building (CHAR_DYNAMIC's doorKey), the bed's BUILDING record, the character whose routine
		// targets it (asleep in it or walking to it; 0 free). version 1 free, 2 occupied; yaw = the lie-down facing
		uint64_t building = 0, occupant = 0;
		uint32_t furniture = 0;
		// container tail (kind 3, sim kenshi-k16.7.28, SIMLINK "Tail (kind 3 only)"): the layout piece this container is, so the client draws no
		// mesh of its own for it. building = the owning building's doorKey (0 for a scenario stand-in box); piece = the piece's index in the
		// interior layout's instance list exactly as read from the data, 0xFFFF for a placed container, a player's storage or a scenario box.
		// hasPiece false from an older server (no tail): the client falls back to matching a piece by position.
		uint16_t piece = 0xFFFF;
		bool hasPiece = false;
		// limb tail (kind 2 only, ITEM `item function` 0x11, sim kenshi-h2v.13.9): the victim's skin tone (the SeveredLimbItem's custom 2 = 1 - Skin
		// Tone), blood colour (custom 3 = the race's `blood colour`, 0..1) and the limb's bone scale, all 9 as the wire sends them; hasLimb false from
		// an older server (no tail) or for every other item. The tint is the wire's, never a client guess (SIMLINK "Tail, kind 2 severed limb only")
		float limbSkin[3] = { 0, 0, 0 }, limbBlood[3] = { 0, 0, 0 }, limbScale[3] = { 1, 1, 1 };
		bool hasLimb = false;
	};
	// INVENTORY (SIMLINK 6): what a player character or a container owns (a container's entries are all slot 0); slot 0 carried, 1 wielded, 2 + n worn in ARMOUR slot n
	struct Inventory
	{
		// item: its item sid. ITEM_QUALITY (27, after the INVENTORY of the same version): maker (WEAPON_MANUFACTURER) and model (MATERIAL_SPECS_WEAPON)
		// strids, 0 = none; level 0..1; grade 0..5 prototype..masterwork (UNSURE server mapping), 255 = none
		// ITEM_GRID (28, after them): section 0 main / a container's own grid, 1 the worn backpack's content, 255 equipped (no x, y; also before any
		// ITEM_GRID); material (MATERIAL_SPECS_WEAPON / _CLOTHING) and colour (COLOR_DATA) strids name the icon, 0 = none
		struct Entry
		{
			uint32_t item = 0, count = 0; uint8_t slot = 0; bool hasQuality = false; uint32_t maker = 0, model = 0; float level = 0; uint8_t grade = 255;
			uint8_t section = 255, x = 0, y = 0; uint32_t material = 0, colour = 0;
			uint8_t steal = 255;	// ITEM_GRID steal tail (kenshi-k16.6.392): the take session's steal chance in percent for this entry, 255 = no roll (own item, or no tail)
		};
		// ITEM_LOCK (57, after ITEM_GRID; SIMLINK "ITEM_LOCK"): the worn shackles' lock. index = their entry; locked: bit 0 (a picked or broken pair stays worn,
		// open); canCut: the wearer carries a tool; canBreak: strength beats 3 x hardness; chance: the wearer's own pick chance per roll, 0..0.9; hardness
		// left of maxHardness (a cut in progress does not resend it: its bar is TASK_PROGRESS kind 7)
		struct Lock { uint16_t index = 0; bool locked = false, canCut = false, canBreak = false; uint16_t level = 0; float chance = 0, hardness = 0, maxHardness = 0; };
		const Lock* lockOf(size_t index) const { for (const Lock& l : locks) { if (l.index == index) { return &l; } } return nullptr; }
		struct Grid { uint8_t section = 0, w = 0, h = 0; };
		uint32_t version = 0;
		std::vector<Entry> items;
		std::vector<Grid> grids;	// ITEM_GRID: the grids it has now (empty before the first one)
		std::vector<Lock> locks;	// ITEM_LOCK: empty with no shackles worn (and before the first one)
	};
	// CHAR_MEDICAL (SIMLINK 6, 2.5 Hz): parts in the race's anatomy order; health may go below 0 (to -3 x max; max comes from the race)
	struct Medical
	{
		// part: the LOCATIONAL_DAMAGE name; max (max health minus wear) < 0, bandage, splint, limb 0: an older server (no panel tail)
		struct Part { uint32_t part = 0; float health = 0, stun = 0, max = -1, bandage = 0, splint = 0; uint8_t limb = 0; };
		// flags: portrait bits 2-3 (k16.6.158, SIMLINK CHAR_MEDICAL; 0 on an older server)
		enum : uint8_t { kDigesting = 1, kInBed = 2, kHurt = 4, kPutIn = 8 };
		enum : uint8_t { kLimbState = 3, kLimbStump = 1, kLimbCrushed = 3, kLimbRobot = 4 };	// Part::limb: bits 0-1 state (0 original, 1 stump, 2 replaced, 3 crushed), bit 2 robot
		uint64_t tick = 0;
		float blood = 0;
		std::vector<Part> parts;
		uint16_t severed = 0;	// bit i = parts[i] severed (b0 46e5a05; 0 from an older server)
		float bloodMax = -1, bleed = 0, hunger = -1;	// the panel tail (sim 3c4b53a): the Blood row's scale, blood lost a second, hunger 0..3; < 0: an older server
		uint8_t flags = 0;
	};
	// CHAR_STATS (SIMLINK "CHAR_STATS and the inspect focus"): the status panel's rows, for each player character and the session's focus.
	// skill ids: the wire table (0 strength .. 32 precision shooting); goal "" = Aimless; bounty amounts signed
	struct CharStats
	{
		enum State : uint8_t { kNormal = 0, kDefensive = 1, kSleeping = 2, kUnconscious = 3, kDead = 4, kLocked = 5, kUnlocked = 6 };
		enum Skill : uint8_t { kStrength = 0, kDexterity = 1, kToughness = 2, kAthletics = 3, kDodge = 4, kAttack = 5, kDefence = 6, kMartialArts = 12, kStealth = 16, kPerception = 24 };
		struct SkillRow { uint8_t skill = 0; float value = 0, bonus = 0; };
		struct Bounty { uint32_t faction = 0; int32_t amount = 0; };
		struct SkillProgress { uint8_t skill = 0; float progress = 0; };
		uint64_t tick = 0;
		uint8_t state = 0, lockLevel = 0, extraKind = 0, extraSkill = 0xFF, weaponClass = 0;	// extraKind 1 Stealth, 2 Athletics, 3-6 job/ranged/melee/swimming (not drawn yet); extraSkill 0xFF = none; weaponClass 0 martial arts, 1 attack + defence, 2 crossbows
		uint32_t goal = 0;
		float encumbrance = 0, extra = 0;
		std::vector<SkillRow> skills;
		float athleticsXp = 0, strengthXp = 0, toughnessXp = 0, koPoint = 0, runSpeed = 0, runPenalty = 0, hungerRate = 0, light = 0, sentenceHours = 0;
		std::vector<Bounty> bounties;
		// The AI tail (rows 16-20, additive after row 15; absent from an older server: hasAi false). goalEnum / stepEnum are AI_TASK values
		// (0xFFFF none; names in docs/formats/AI_TASK_ENUMS.md, ai_task_names.cpp's table); source: 0 none, 1 a player order, 2 a package / job row,
		// 3 the routine slot, 4 the group AI, 5 the fight, 6 a carry chain, 7 fleeing; targetId: a character, else a building / site, else 0; the
		// target point is NaN when there is none.
		static constexpr uint16_t kNoTaskValue = 0xFFFF;
		bool hasAi = false;
		uint16_t goalEnum = kNoTaskValue, stepEnum = kNoTaskValue;
		uint8_t source = 0, stepIndex = 0, stepCount = 0;
		uint64_t targetId = 0;
		float targetX = 0, targetZ = 0;
		// The stats window tail (rows 21-23, k16.6.54; additive after the AI tail; absent from an older server: hasProgress / hasCarry / hasResist
		// false). Row 21: the Next Level bar of every row-8 skill, the fractional part of its value (same ids, same count). UNSURE: whether it is
		// the exe's own bar formula (ksim's note, deferred to playtesting). Row 22: the pack's kg without the fixed 30 kg carry-person weight.
		// Row 23: the damage resistance as a fraction (1 - toughnessFactor, about -0.65..0.65).
		bool hasProgress = false, hasCarry = false, hasResist = false;
		std::vector<SkillProgress> progress;
		float carryKg = 0, carryMaxKg = 0, damageResist = 0;
		// The HUD tag row tail (rows 24-32, k16.6.215; additive after row 23, absent from an older server: hasTag false). The booleans are
		// tagFlags; b1 army commander, b3 diplomat and b5 escaped prisoner arrive 0 until the sim models them (k16.6.211 / .212 / .214), and
		// slaveLook is 255 while slave look is unmodelled (k16.6.213): b4 is then meaningless and the slave tooltip leaves its two lines out.
		enum TagFlag : uint16_t { kSquadLeader = 1, kArmyCommander = 2, kCaptive = 4, kDiplomat = 8, kSlaveSeen = 16, kEscapedPrisoner = 32, kObedient = 64 };
		// The flags the status tag row reads: no Leader / War Leader tag on a character of any player faction (user 2026-11-15, kenshi-k16.2.276); the sim sets b0 on
		// a player squad's first member, so the client drops b0 / b1 there. NPC leaders keep them.
		static uint16_t shownTagFlags(uint16_t flags, bool playerFaction) { return playerFaction ? uint16_t(flags & ~(kSquadLeader | kArmyCommander)) : flags; }
		struct BountyTag
		{
			uint32_t crimes = 0;	// bit n = CrimeEnum n was committed
			uint8_t claim = 0;	// 1: the reward was already collected
			float hoursLeft = -1;	// -1: never expires (notorious)
		};
		bool hasTag = false;
		uint16_t tagFlags = 0;	// the TagFlag bits above
		uint8_t slaveState = 0;	// 0 free, 1 slave, 2 escaped, 3 ex-slave
		uint32_t captor = 0;	// the faction that chained the character; 0 = none
		float slaveHoursLeft = -1;	// states 2 / 3: hours until the hunt cools; -1 = none / unstamped
		uint8_t slaveLook = 255;	// the percent the slave has been looked at; 255 = not modelled
		uint8_t crime = 0;	// the pending crime's CrimeEnum, 0 = none
		float crimeTimer = 0;	// its timer in seconds; (int) = the tag's N, > 3.0 = caught
		float escapeHoursLeft = -1;	// the escaped prisoner's expiry; -1 = none
		uint32_t localFaction = 0;	// the owner of the town the character stands in; 0 = none / outdoors
		std::vector<BountyTag> bountyTags;	// one per bounties entry, same order
	};
	// CHAR_DETECT (SIMLINK 6): the detection meter of a player-controlled character; state 0 none, 2 being noticed, 1 noticed (meter 1); by = the observer (any non-allied observer, sensor::ally, not only a hostile team: kenshi-rquo)
	struct Detect
	{
		enum State : uint8_t { kNone = 0, kNoticed = 1, kNoticing = 2 };
		uint64_t tick = 0, by = 0;
		float meter = 0;
		uint8_t state = 0;
	};
	// SEEN_BY (SIMLINK 6): for a sneaking player-controlled character in view, the non-allied observers (sensor::ally, hostile or neutral) in view that know it, sorted by id. It is
	// exactly the stealth KO's abort test (a non-allied observer absent is one the KO does not abort on); n = 0 (an empty set) erases the entry.
	struct SeenBy { uint64_t tick = 0; std::vector<uint64_t> by; };
	// DETECTED_BY (SIMLINK 60): a sneaking player character's detection records, one entry per observer (the original's per-pair notice meter):
	// state 1 noticed (red), 2 being noticed (yellow), friendly = a friend force-noticed it (green). n = 0 (an empty set) erases the entry.
	struct DetectedBy
	{
		struct Entry { uint64_t observer = 0; uint8_t state = 0, friendly = 0; bool operator==(const Entry&) const = default; };
		uint64_t tick = 0;
		std::vector<Entry> by;
	};
	struct Squad { uint64_t id; uint32_t faction; float x, z; uint16_t members; };
	// PLAYER_SQUADS (SIMLINK 31): this session's faction's squads in list order (= HUD tab order), members leader first; empty squads included
	struct PlayerSquads
	{
		struct Squad { uint64_t id = 0; std::string name; std::vector<uint64_t> members; };
		uint64_t tick = 0, faction = 0;
		uint32_t factionName = 0;	// strid
		uint16_t factionCount = 0, maxFaction = 0, maxSquad = 0, maxSquads = 0;	// living members; GLOBAL CONSTANTS limits
		std::vector<Squad> squads;
		int64_t money = 0;	// the faction's cats (trailing, MINE_SELL_SLICE 3.1); hasMoney false from an older server
		bool hasMoney = false, valid = false;
	};
	// SAVES (SIMLINK 32): the server's save dir, newest first; status 0 ok, 1 older (migrates on load), 2 unreadable / newer
	struct SaveInfo { std::string name; int64_t mtime = 0; uint32_t version = 0; uint8_t status = 0; uint32_t day = 0; float hour = 0; std::string factionName, leaderName; uint16_t members = 0; int64_t money = 0; };
	struct Weather { uint32_t group, weather, previous; float fogBlend, skyBlend, strength; };	// WEATHER_CYCLE_LIVE: fog / sky fades 0..1 (server-timed), strength per instance
	// BEAMS (SIMLINK "BEAMS", 58): the burning wandering weather effects (EFFECT type 9) the sim has planned, by biome group. A beam exists for
	// sim ticks spawnTick <= t < endTick; from (x, z) it moves speed / 20 per tick along the yaw of the segment that covers that tick (yaw 0 = +Z,
	// positive toward +X). The client draws this path, y = the terrain under it (its own snap); the damage is the sim's. `flags` bit 0: forget
	// the group's beams before applying this list. The strids are the EFFECT's sid, its `particle system` and its `sound` (0 = none).
	struct BeamSegment { uint16_t ticks = 0; float yaw = 0; };
	struct Beam
	{
		uint32_t id = 0;	// unique inside one weather instance of one group
		uint32_t effect = 0, particles = 0, sound = 0;	// strids
		uint64_t spawnTick = 0, endTick = 0;
		uint16_t visibleTicks = 0;	// ticks from spawnTick the beam is seen; 0 = all its life (h2v.14.11)
		float x = 0, z = 0;	// the ground point at spawnTick
		float speed = 0, radius = 0, strength = 0;
		std::vector<BeamSegment> segments;	// they cover the whole life: the ticks sum to endTick - spawnTick
	};
	struct BeamGroup { uint32_t group = 0; uint8_t flags = 0; std::vector<Beam> beams; };
	// Sim ticks a beam's particle system can still live after its endTick (the scripts' longest emitter duration + particle life, rwh.48 5). The
	// decoder drops a beam this long after its end (the sim forgets it at its end; nothing else comes for it), BeamField keeps drawing it meanwhile.
	inline constexpr uint64_t kBeamParticleTailTicks = 250;	// 12.5 s
	struct Box { float minX, minZ, maxX, maxZ; };
	struct Event
	{
		uint64_t tick = 0;
		uint16_t kind = 0;
		uint64_t c = 0;	// shot (49): the turret's doorKey (0: a hand crossbow)
		uint64_t a = 0, b = 0;	// shot (49): shooter, target (pos = muzzle, to = target point, result 0 hit / 2 missed, str = the CROSSBOW); interact done: actor, target (dir = action, result = result); lock_roll (20): picker, door (result = success); theft: thief, container (str = owner FACTION); limb severed (47): id (str = the part); ragdoll (48): id (result = cause, pos = dir, amount = strength, str = part, ref[0] = seed); dismissed (18): id, new squad (str = faction name); discovered (19): str = the TOWN; KO / wake / death / drop / pick / blocked / left / corrected: id; applied: clientSeq, appliedTick; rejected: clientSeq
		uint32_t str = 0;	// hit: part; drop / pick: item; rejected: reason; speech: the text; ragdoll: the part
		uint32_t ref[2] = {};	// speech: the DIALOGUE and DIALOGUE_LINE sids
		float pos[3] = {}, damage[4] = {}, amount = 0;	// amount: bleed drip rate, bleed pool g, smear alpha; position_corrected: distance (-1: no mesh within 256, not moved)
		float absorbed[2] = {};	// hit: the armour-stopped cut (pierce merged) and blunt, before toughness
		float to[3] = {};	// shot: the target point (the bolt's end)
		uint8_t dir = 0, result = 0;	// stealth_ko (50): assassin, victim (amount = the chance), result 0 landed / 1 failed / 2 aborted (aware)
	};

	class SimLinkDecoder
	{
	public:
		// Appends bytes and decodes every complete frame. False after a protocol error (error() says why); the caller sends ERROR and closes.
		bool feed(const uint8_t* data, size_t n);
		const std::string& error() const { return error_; }

		const std::string& str(uint32_t id) const;	// "" for 0 or unknown
		// DEBUG VIEW frames 200-219 (SIMLINK "DEBUG VIEW"): handed whole to this sink (client/src/debug/, KCLIENT_DEBUG_VIEW); unset: dropped quietly
		std::function<void(uint16_t type, const uint8_t* p, size_t n)> debugFrames;
		uint32_t find(const std::string& s) const;	// the strid of a table string, 0 when absent (a linear scan: for rare lookups)
		bool helloSeen() const { return hello_; }
		uint64_t serverDataHash() const { return dataHash_; }
		const std::string& serverName() const { return serverName_; }

		std::unordered_map<uint64_t, Character> chars;
		std::unordered_map<uint64_t, Interactable> things;
		// Counts the things map's changes (kInteractable frames, the reset, event 11): the server sends an interactable on first sight and when its
		// version changes, so a client list built from it (the props, main.cpp) only has to be rebuilt when this moves (kenshi-k16.2.205.11).
		uint32_t thingsSerial = 0;
		std::unordered_map<uint64_t, Medical> medical;	// dropped with the character (event 11)
		std::unordered_map<uint64_t, CharStats> charStats;	// dropped with the character (event 11)
		std::unordered_map<uint64_t, Inventory> inventories;	// player characters only; dropped with the character (event 11)
		std::unordered_map<uint64_t, Detect> detect;	// player characters in view; state 0 erases; dropped with the character (event 11)
		// CAGES (64, kenshi-k16.2.105.1): the caged characters in view, by character id: the cage's key (the target of order 99 and of order 60
		// actions 18 pick / 19 cut / 20 break), its BUILDING record strid, whether it is locked, its lock level and what is left of the lock.
		// The whole list on connect and on change: a frame replaces the map. Dropped with the character (event 11) and on a reset.
		struct Cage { uint64_t cage = 0; uint32_t record = 0; bool locked = false; uint16_t level = 0; float hardness = 0, maxHardness = 0; };
		std::unordered_map<uint64_t, Cage> cages;
		uint32_t cagesSerial = 0;	// counts CAGES frames
		// CAMERA_START (65, SIMLINK.md "CAMERA_START", kenshi-cym.40.2.3): the imported save's camera and floor, once per session, no tick. flags bit 0:
		// pos / rot / zoom valid; bit 1: floor valid. pos is the pivot (focus); rot (w, x, y, z) the pivot's world orientation, the original camera looking
		// +Z; zoom as saved (negative: the eye's distance behind the pivot). Not world state: a reset does not clear it; the host consumes it by serial.
		struct CameraStart { uint8_t flags = 0; double pos[3] = {}; float rot[4] = {}; float zoom = 0; uint8_t floor = 0; };
		CameraStart cameraStart;
		uint32_t cameraStartSerial = 0;	// counts CAMERA_START frames
		// PROFILE (70, SIMLINK.md "PROFILE", kenshi-k16.2.148): a KSIM_PROFILE server's answer to order 120, the window summary and the per-system
		// tick timings of docs/PROFILING.md, in that order for the whole server run. Each frame replaces the last; rows are keyed by name (the client
		// keeps no row across answers), trailing bytes are ignored. Not world state: a load does not clear it.
		struct Profile
		{
			struct Row { std::string name; float meanUs = 0, maxUs = 0; uint64_t totalNs = 0; };
			uint64_t tick = 0;
			uint32_t window = 0;
			float tickMeanUs = 0, tickP50Us = 0, tickP99Us = 0, tickMaxUs = 0, tickBudgetUs = 0;
			float characters = 0, squads = 0, pathQueries = 0, pathUs = 0;
			std::vector<Row> rows;
			bool valid = false;
		};
		Profile profile;
		uint32_t profileSerial = 0;	// counts PROFILE frames applied
		std::unordered_map<uint64_t, SeenBy> seenBy;	// sneaking player characters in view; an empty set erases; dropped with the character (event 11)
		uint32_t seenBySerial = 0;	// counts SEEN_BY frames applied (a scripted log prints on change)
		std::unordered_map<uint64_t, DetectedBy> detectedBy;	// sneaking player characters in view; an empty set erases; dropped with the character (event 11)
		uint32_t detectedBySerial = 0;	// counts DETECTED_BY frames applied (a scripted log prints on change)
		std::vector<Squad> squads;
		PlayerSquads playerSquads;
		uint32_t playerSquadsSerial = 0;	// counts PLAYER_SQUADS frames
		std::vector<Weather> weather;
		uint32_t weatherSerial = 0;
		// The WEATHER frame's optional wind tail (after the list): the wind where the camera is, absent with no view focus or region. The yaw is
		// the character convention: toward = (sin, cos), 0 = +Z (SIMLINK "WEATHER", sim 61416de0).
		bool windValid = false;
		float windSpeed = 0, windYaw = 0;
		// BEAMS (58): the beams last applied, by group (a group's entry is its full set after a bit-0 reset, else a list merged by id). The client
		// drops a beam itself at endTick; a beam planned while out of the session's range turns up already alive (spawnTick in the past).
		std::vector<BeamGroup> beamGroups;
		uint64_t beamsTick = 0;	// the sim tick of the last BEAMS frame
		uint32_t beamsSerial = 0;	// counts BEAMS frames
		std::vector<SaveInfo> saves;
		uint32_t savesSerial = 0;	// counts SAVES frames
		// A load (order 81) replaces the world without event 11s: when event 9 answers resetSeq, the world state is dropped right there, before the
		// full resend that follows it in the same write (SIMLINK SAVES "Answers").
		uint32_t resetSeq = 0;
		uint32_t resetSerial = 0;	// counts those resets
		// JOBS (34): the player characters' jobs lists (priority order, top first); an entry replaces the character's whole list. A character
		// with no entry has the default: enabled, no rows.
		struct JobRow { uint32_t jobId = 0; uint16_t type = 0; uint64_t target = 0; bool active = false; };
		struct Jobs { bool enabled = true; std::vector<JobRow> rows; };
		std::unordered_map<uint64_t, Jobs> jobs;	// dropped with the character (event 11)
		uint32_t jobsSerial = 0;
		// UNLOCKS (36): the player faction's completed research and the buildings it enables (both strids of stringIDs; empty with no player faction)
		std::vector<uint32_t> research, unlocked;
		uint32_t unlocksSerial = 0;
		// RESEARCH_STATE (63): the player faction's research page (SIMLINK.md; sent with UNLOCKS). state 0 completed, 1 listable, 2 locked; reason 0 none,
		// 1 requirement, 2 tech level, 3 blueprint only, 4 nothing left to unlock (sent as state 0). kinds: 1 item .. 8 building, each the refs unlocked.
		struct ResearchRecord { uint32_t research = 0; uint8_t state = 2, reason = 0; };
		struct ResearchState
		{
			bool valid = false;	// a frame has arrived
			uint64_t tick = 0;
			uint8_t techLevel = 1;
			std::vector<ResearchRecord> records;
			std::map<uint8_t, std::vector<uint32_t>> unlocks;	// by kind id, unknown ids kept
		} researchState;
		uint32_t researchStateSerial = 0;
		// DISCOVERED (37): the player faction's discovered places (TOWN strids) in TOWN record order, the whole list each time
		struct Place { uint32_t town = 0; bool visited = false; };
		std::vector<Place> discovered;
		uint32_t discoveredSerial = 0;
		// FACTION_RELATIONS (38): the factions the player knows, in the sim's order; value = their relation toward the player (-100..100)
		struct Relation { uint32_t faction = 0; std::string name; float value = 0; };
		std::vector<Relation> relations;
		uint32_t relationsSerial = 0;
		// BUILDING (47, BUILDINGS_SLICE.md section 12): the player faction's buildings by id, a record replaced on change, dropped by event 11.
		// status: section 7's enum; progress at `tick`, advanced by rate per sim-second (as TASK_PROGRESS).
		struct Building
		{
			enum Flag : uint32_t { kBuilt = 1, kPaused = 2, kBroken = 4, kSwitchedOn = 8, kRunOffBattery = 16, kRepairWanted = 32, kDismantling = 64, kHasItems = 128,
				kUpgrade = 256, kDowngrade = 512, kOnGrid = 1024, kAutomatic = 2048 };
			struct Material { uint32_t item = 0; uint16_t have = 0, need = 0; };
			uint64_t tick = 0, id = 0, owner = 0, town = 0;
			uint32_t record = 0, flags = 0, version = 0;
			float pos[3] = {}, rot[4] = { 1, 0, 0, 0 };	// rot w, x, y, z
			uint8_t floor = 0, status = 0, workers = 0, maxWorkers = 0;
			float progress = 0, rate = 0, hp = 0, maxHp = 0;
			std::vector<Material> materials;
			float powerValue = 0, powerSupplied = 0, powerDemand = 0, stored = 0, capacity = 0, efficiency = 0, maxEfficiency = 0, quality = 1;
		};
		// TASK_PROGRESS (39): the player characters' timed tasks with a bar, the whole list at `tasksTick`; fill = clamp(progress + rate x sim-seconds since, 0, 1)
		// flags / target: the tail (2026-11-11, h2v.16.1) — bit 0 the original shows this bar, bit 1 the character is the player faction's; target is the
		// door, container, building, producer, body, patient or cage worked on (0: none). 0 when an older server sent no tail (SIMLINK TASK_PROGRESS "Tail").
		struct TaskProgress { uint64_t ch = 0; uint8_t kind = 0; float progress = 0, rate = 0, value = 0; uint8_t flags = 0; uint64_t target = 0; };
		std::vector<TaskProgress> tasks;
		uint64_t tasksTick = 0;
		// TRADE (53; MINE_SELL_SLICE 3.2 said 43, which is NAV_PARAMS: 53 proposed to b0): the open trade of this session, replaced whole on each frame; event 40 clears it (valid = false) and
		// drops inventories[stock]. Prices per unit: stock entries {buy, sell}, actor entries {sell, -1: the trader won't buy}.
		struct Trade
		{
			struct StockPrice { uint16_t index = 0; int32_t buy = 0, sell = 0; };
			struct ActorPrice { uint16_t index = 0; int32_t sell = -1; };
			bool valid = false;
			uint64_t tick = 0, trader = 0, actor = 0, stock = 0;
			uint32_t tmpl = 0, stockVersion = 0, actorVersion = 0;
			int64_t traderMoney = 0;
			std::vector<StockPrice> stockPrices;
			std::vector<ActorPrice> actorPrices;
			struct ChestPrices { uint64_t container = 0; uint32_t version = 0; std::vector<ActorPrice> prices; };	// a price holds only while version is the container's INVENTORY version
			std::vector<ChestPrices> chests;	// tail (b0 4af93a3): owned containers within 1000 u of the trader, order 68's `source` set, by id
			struct Buyback { uint16_t index = 0; uint32_t count = 0; int32_t price = 0; };
			std::vector<Buyback> buyback;	// tail after the chests (k16.20.16): stock entries holding units this faction sold in this window; a buy pays `price` for the first `count`
		};
		Trade trade;
		// REPLY_MENU (event 43): the open menus, one per (speaker, char); a resend replaces it, event 44 or the char's answer (event 41 bit 2) removes it, a reset clears them (the server resends).
		// An option's line 0 is the "[...]" continue entry.
		struct ReplyMenu
		{
			enum : uint8_t { kResend = 1, kDismissible = 2, kFreezes = 4 };
			struct Option { uint32_t line = 0, text = 0; };
			uint64_t speaker = 0, ch = 0;
			uint32_t seq = 0;
			uint8_t flags = 0;
			std::vector<Option> options;
		};
		std::vector<ReplyMenu> menus;
		// MENU_ENTRIES (61): the answer to order 99 menu_query, to the asking session only; entries is a bitset of the right-click entries that
		// apply (bit 0 Talk To, the rest reserved). Consumed by the caller (deque, like events), so a stale reply is still seen and can be dropped.
		// A cage key as target (kenshi-k16.2.105.1): bit 16 pick lock, 17 use tools, 18 use strength; pickChance (-1: none) trails the bit-16 frame.
		struct MenuEntries { uint32_t clientSeq = 0; uint64_t id = 0, target = 0; uint32_t entries = 0; float pickChance = -1; };
		std::deque<MenuEntries> menuEntries;
		// ITEM_STATS (29): order 69's answers by (id, version, index), never pushed; the sim's numbers for the weapon / armour tooltip rows.
		// kind 0 plain, 1 armour, 2 weapon; mults are 1 and bonuses 0 when neutral; grade 255 / level -1: no quality; sellValue -1: not modelled
		struct ItemStats
		{
			struct Cover { uint32_t part = 0; float fraction = 0; };
			uint8_t kind = 0;
			float blunt = 0, cut = 0, cutEfficiency = 0, harpoon = 0;	// armour
			std::vector<Cover> coverage;
			float combatSpeedMult = 1, dodgeMult = 1, damageOutputMult = 1, attackBonus = 0, defenceBonus = 0, unarmedBonus = 0;
			uint8_t armourClass = 0, material = 0;
			float stealthMult = 1, athleticsMult = 1, loadedCombatSpeedMult = 1, dexterityMult = 1;	// (g): the loader's runtime values
			int32_t meleeAttack = 0, meleeDefence = 0;
			float cutDamage = 0, bluntDamage = 0, cutMult = 0, bluntMult = 0, bleed = 0, armourPenetration = 0, attackMod = 0, defenceMod = 0, length = 0;	// weapon
			uint8_t weaponType = 0;
			float weight = 0, value = 0, sellValue = -1, level = -1;
			uint8_t grade = 255;
		};
		std::map<std::array<uint64_t, 3>, ItemStats> itemStats;	// key {id, version, index}
		uint32_t itemStatsSerial = 0;
		uint32_t tradeSerial = 0;	// counts TRADE frames and closes
		std::map<uint64_t, Building> buildings;
		uint32_t buildingsSerial = 0;
		// BUILDING_GRID (48): the player towns with their power totals at the last solve
		struct PlayerTown { uint64_t id = 0; std::string name; float supply = 0, demand = 0, stored = 0, capacity = 0; uint16_t generators = 0, consumers = 0, batteries = 0; };
		std::vector<PlayerTown> playerTowns;
		uint32_t playerTownsSerial = 0;
		// PROSPECT (35): the latest finished prospect (a singleton: the latest wins). Planes N x N, row 0 = the lowest z; deposits within 1.2 r.
		struct Prospect
		{
			uint64_t id = 0;	// the prospector (0: none yet)
			float x = 0, z = 0, altitude = 0, science = 0, r = 0, wind = 0;
			uint32_t zone = 0;	// strid of the biome group's name (0: none)
			std::vector<std::pair<uint8_t, float>> env;	// class (1 Green, 2 Arid, 3 Swamp), fraction; empty = "Dead"
			std::vector<std::pair<uint8_t, float>> res;	// resId (5 Water, 6 Fertility, 2 Stone, 1 Iron, 3 Copper), raw value
			uint16_t n = 0;
			std::vector<uint8_t> grey, planes[3];	// planes: Water, Fertility, Stone, round(clamp(v) x 255)
			struct Deposit { uint8_t res = 0; float x = 0, z = 0, richness = 0; };
			std::vector<Deposit> deposits;
		};
		Prospect prospect;
		uint32_t prospectSerial = 0;
		// STARTS (33): every NEW_GAME_STARTOFF in record order, ready = its start zone has a navmesh (a not-ready one answers not_ready).
		// startoff: strid of the record's stringID, sent back in NEW_GAME; forceRace: the creator's allowed races (empty: any); members: the squad NEW_GAME
		// builds, leader first (its size is the Spec count; flags bit 0: editable, a creator race; else the Spec echoes race and sex with no look)
		struct Member { uint32_t race = 0; bool female = false; std::string name; uint8_t flags = 0; uint32_t character = 0; std::vector<uint32_t> armour; uint32_t weapon = 0; };
		struct Start { uint32_t startoff = 0; bool ready = false; std::vector<uint32_t> forceRace; std::vector<Member> members; };
		std::vector<Start> starts;
		uint32_t startsSerial = 0;
		// NEW_GAME_RESULT (62): result 0 = the new faction and its members (leader first) and, when the server sends it, the leader's position; 1 = rejected (reason strid).
		struct { uint8_t result = 0; uint64_t faction = 0; std::vector<uint64_t> members; bool hasPos = false; float pos[3] = {}; uint32_t reason = 0; } newGame;
		uint32_t newGameSerial = 0;
		// SCENARIO (SIMLINK "SCENARIO": the playtest scenarios, task CE): the server's list (54), the answer to a load (55) and a run's metrics (56).
		// Each answer replaces its state and bumps its serial; strings are inline (str), the answers carry no tick head.
		// bead: the file header's bead=<feature id> (the 2026-11-10 frame-tail; "" on an older server that sends no tail)
		struct ScenarioInfo { std::string name, title, setup, lookFor, bead; uint64_t fileHash = 0; std::vector<std::string> metrics; };
		std::vector<ScenarioInfo> scenarioList;
		uint32_t scenarioListSerial = 0;
		struct { uint32_t clientSeq = 0; uint8_t status = 1; std::string error, name; uint64_t fileHash = 0; uint32_t seed = 0; std::string simHash; double startGameTime = 0; float centreX = 0, centreZ = 0; std::string bead; } scenarioStarted;
		uint32_t scenarioStartedSerial = 0;
		struct ScenarioMetric { std::string name; double value = 0; bool valid = false; };
		struct { uint32_t clientSeq = 0; std::string name; double elapsedGameS = 0; std::vector<ScenarioMetric> metrics; } scenarioMetrics;
		uint32_t scenarioMetricsSerial = 0;
		std::vector<Box> bounds;
		// paused: any bit; pauseBits: bit 0 the idle / admin pause (order 40), bit 1 a reply menu is open (the dialogue freeze)
		struct { uint64_t tick = 0; uint32_t day = 0; float hour = 0, speed = 1; bool paused = false; uint8_t pauseBits = 0; bool valid = false; } clock;
		uint64_t lastCompleteTick = 0;	// tick of the newest SNAPSHOT_END
		uint32_t snapSerial = 0;	// counts SNAPSHOT_END frames (a load's first snapshot is the first one after its reset, k16.2.134.1)
		NavParams nav;
		std::map<std::array<int32_t, 3>, NavTile> navTiles;	// (tx, tz, layer); cleared by a new meshId
		std::deque<NavPatch> navPatches;	// consumed by the caller (applied to its dtNavMesh), like events
		uint32_t navGeneration = 0;	// bumped on every params / tile change: a consumer rebuilds or re-adds tiles
		std::deque<Event> events;	// consumed by the caller
		size_t frames = 0;

	private:
		bool frame(uint16_t type, const uint8_t* p, size_t n);
		void skipped(const char* what, uint16_t v);	// logs an unknown frame type / event kind once
		std::vector<uint8_t> buf_;
		std::vector<std::string> strings_ = { std::string() };
		std::string error_, serverName_;
		std::unordered_set<std::string> skipped_;
		uint64_t dataHash_ = 0, openTick_ = 0;
		bool hello_ = false, inSnap_ = false;
	};

	// Client -> server frames, appended to out (the transport sends them in order).
	class OrderWriter
	{
	public:
		static void hello(std::vector<uint8_t>& out, uint64_t dataHash, const std::string& name);
	static void ping(std::vector<uint8_t>& out) { const uint8_t f[6] = { 2, 0, 0, 0, kPing, 0 }; out.insert(out.end(), f, f + 6); }	// empty payload; every 5 s after HELLO (SIMLINK 11)
		uint32_t moveTo(std::vector<uint8_t>& out, const std::vector<uint64_t>& ids, float x, float z, bool queue, float y = 0, bool yValid = false);	// y: the clicked surface height (yValid: 3D snap)
		uint32_t attack(std::vector<uint8_t>& out, const std::vector<uint64_t>& ids, uint64_t target);
		uint32_t follow(std::vector<uint8_t>& out, const std::vector<uint64_t>& ids, uint64_t target);
		uint32_t interact(std::vector<uint8_t>& out, const std::vector<uint64_t>& ids, uint64_t target, uint8_t action, bool queue, uint8_t flags = 0);	// SIMLINK 13 actions: 1 open, 2 close, 5 loot, 7 pick_up, 10 first_aid, ...; flags: the optional trailing byte (5 loot: bit 0 open), sent when not 0
		uint32_t drop(std::vector<uint8_t>& out, uint64_t id, const std::string& item, uint16_t count);	// order 61: carried units of item (its stringID) to the ground
		uint32_t equip(std::vector<uint8_t>& out, uint64_t id, const std::string& item, bool on);	// order 62: wield / wear a carried WEAPON or ARMOUR, or put it back
		uint32_t transfer(std::vector<uint8_t>& out, uint64_t id, uint64_t container, const std::string& item, uint16_t count, bool take);	// order 63: within 16 u
		// Grid inventory (SIMLINK 9 ITEM_GRID): version / index address an entry of the (source) INVENTORY, answered `stale` when it changed; section 255 = autoplace
		uint32_t moveItem(std::vector<uint8_t>& out, uint64_t id, uint32_t version, uint16_t index, uint8_t section, uint8_t x, uint8_t y, uint16_t count = 0);	// order 64 (id: a character or a container; count 0: the whole stack)
		uint32_t arrange(std::vector<uint8_t>& out, uint64_t id, uint8_t section);	// order 65
		uint32_t dropEntry(std::vector<uint8_t>& out, uint64_t id, uint32_t version, uint16_t index, uint16_t count);	// order 66
		uint32_t equipEntry(std::vector<uint8_t>& out, uint64_t id, uint32_t version, uint16_t index, bool on);	// order 67
		uint32_t transferEntry(std::vector<uint8_t>& out, uint64_t id, uint64_t container, uint32_t version, uint16_t index, uint16_t count, bool take, uint8_t section, uint8_t x, uint8_t y);	// order 68
		uint32_t toggle(std::vector<uint8_t>& out, const std::vector<uint64_t>& ids, uint8_t which, uint8_t value);	// which: 0 block, 1 stealth, 2 run, 3 ai_off, 4 jobs, 5 speed mode (value 0..3)
		uint32_t stop(std::vector<uint8_t>& out, const std::vector<uint64_t>& ids, bool hold);
		uint32_t pause(std::vector<uint8_t>& out, bool paused);	// order 40
		uint32_t itemStats(std::vector<uint8_t>& out, uint64_t id, uint32_t version, uint16_t index);	// order 69 (ITEM_STATS 29 answers)
		uint32_t reply(std::vector<uint8_t>& out, uint64_t id, uint64_t speaker, uint32_t menuSeq, uint8_t option);	// order 88: answer a REPLY_MENU (option = its index)
		uint32_t endTalk(std::vector<uint8_t>& out, uint64_t id, uint64_t speaker);	// order 89: close it; event 44 reason 4
		uint32_t tradeClose(std::vector<uint8_t>& out);	// order 86 trade_close (no body; event 40 answers)
		uint32_t setSpeed(std::vector<uint8_t>& out, float speed);	// order 41, (0, 64]
		uint32_t setInterest(std::vector<uint8_t>& out, float x, float z, float radius);
		uint32_t focus(std::vector<uint8_t>& out, uint64_t id);	// order 87: the session's inspect focus (0 none), session control
		uint32_t setProgressLevel(std::vector<uint8_t>& out, uint8_t level);	// order 98: which TASK_PROGRESS rows this session gets (0 none, 1 the original's set, 2 every player row, 3 plus NPCs), session control
		uint32_t menuQuery(std::vector<uint8_t>& out, uint64_t id, uint64_t target);	// order 99: which right-click entries apply for id on target (MENU_ENTRIES 61; a read, no tick)
		uint32_t setView(std::vector<uint8_t>& out, float x, float z);
		uint32_t drive(std::vector<uint8_t>& out, uint64_t id, uint8_t drive, float heading, int8_t strafe = 0);
		// Player squads (SIMLINK 70-75); each applied one is followed by a fresh PLAYER_SQUADS
		uint32_t squadAdd(std::vector<uint8_t>& out);	// 70
		uint32_t squadDelete(std::vector<uint8_t>& out, uint64_t squad);	// 71
		uint32_t squadRename(std::vector<uint8_t>& out, uint64_t squad, const std::string& name);	// 72
		uint32_t factionRename(std::vector<uint8_t>& out, const std::string& name);	// 85
		uint32_t squadMove(std::vector<uint8_t>& out, uint64_t id, uint64_t squad, uint16_t index, bool swap);	// 73: index 0xFFFF appends, 0 = leader
		uint32_t squadReorder(std::vector<uint8_t>& out, uint64_t squad, uint16_t index);	// 74
		uint32_t dismiss(std::vector<uint8_t>& out, uint64_t id);	// 75
		// Jobs (SIMLINK 76-78, section 6 JOBS); each applied one is followed by a fresh JOBS entry
		uint32_t jobAdd(std::vector<uint8_t>& out, const std::vector<uint64_t>& ids, uint16_t type, uint64_t target);	// 76: the exe's TaskData type; target 0 = none
		uint32_t jobRemove(std::vector<uint8_t>& out, uint64_t id, uint32_t jobId);	// 77
		uint32_t prospect(std::vector<uint8_t>& out, uint64_t id);	// 79
		uint32_t jobMove(std::vector<uint8_t>& out, uint64_t id, uint32_t jobId, uint16_t index);	// 78: index in the list without the row; 0xFFFF = last
		// Saves (SIMLINK 80-83, session controls, admin = a loopback client); each applied one is followed by SAVES
		uint32_t saveGame(std::vector<uint8_t>& out, const std::string& name);	// 80
		uint32_t loadGame(std::vector<uint8_t>& out, const std::string& name, bool resetPositions);	// 81
		uint32_t deleteSave(std::vector<uint8_t>& out, const std::string& name);	// 82
		uint32_t listSaves(std::vector<uint8_t>& out);	// 83
		uint32_t profileQuery(std::vector<uint8_t>& out);	// 120: the KSIM_PROFILE window summary (PROFILE 70); admin session, session control, one-shot
		uint32_t newWorld(std::vector<uint8_t>& out);	// 84: a fresh world without a player faction (admin)
		// NEW_GAME Spec (SIMLINK NEW_GAME, character creation): strids of the server's table, 0 = none; stance 1..8, 0 for a non-editable member
		struct NewGameSpec { uint32_t race = 0; bool female = false; uint32_t head = 0, hair = 0, beard = 0, hairColour = 0; float skin = 0; std::vector<std::pair<std::string, float>> sliders; std::string name; int32_t stance = 0; };
		uint32_t newGame(std::vector<uint8_t>& out, const std::string& name, uint32_t startoff, const std::vector<NewGameSpec>& specs = {});	// frame 111, no specs: the start-off's own squad	// order 20: 0 stop, 1 forward, 2 backward (SIMLINK 12)
		// Scenarios (SIMLINK "SCENARIO", session controls like load_game, any session): 102 the list, 103 a load (the world swap), 104 the run's end
		uint32_t scenarioList(std::vector<uint8_t>& out);	// 102
		uint32_t scenarioLoad(std::vector<uint8_t>& out, const std::string& name, uint32_t seed);	// 103; seed 0 = the server picks
		uint32_t scenarioEnd(std::vector<uint8_t>& out);	// 104: SCENARIO_METRICS answers
	private:
		uint32_t seq_ = 0;
	};
}
