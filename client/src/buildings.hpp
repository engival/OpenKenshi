#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Placed buildings resolved to render parts (CPU only; no graphics). Chain: zone instance -> BUILDING -> refs `parts` -> BUILDING_PART
// `phs or mesh` (.mesh only) + `material` -> MATERIAL_SPEC `texture map`. Docs: docs/formats/COLLISION_FORMAT.md section 3-4,
// docs/reference/BUILDING_MATERIALS.md. Random choices (part groups, material collections) are seeded by the instance id, so they are
// stable across frames and runs.
struct BuildingPartDef
{
	std::string mesh;		// record path of the .mesh
	std::string texture;		// record path of the MATERIAL_SPEC `texture map`, "" if none
	float tileX = 1, tileY = 1;	// MATERIAL_SPEC tile X/Y: UV multiplier
	int materialType = 0;		// BuildingShader: 0 DEFAULT, 1 ALPHA, 2 FOLIAGE, 3 DUAL, 4 EMISSIVE
	std::string normal;		// MATERIAL_SPEC `normal map` (alpha = transparency for ALPHA, glow for EMISSIVE); only set for those types
	// A ground severed limb: an ITEM whose `item function` is 0x11. Its material is a race clothing spec and SeveredLimbItem (vf108) binds its
	// texture, normal and color maps to the original SeveredLimb pass (skin rgb from the wire x this color map's r, the bloody.png blood; mesh.glsl
	// mode 7). The tint itself is per instance (BuildingInstance::limbSkin / limbBlood), never a client guess.
	bool limb = false;
	std::string colorMap;		// MATERIAL_SPEC `color map` (the race body mask, r = skin tone mask); only set for a limb
	std::string texture2;		// MATERIAL_SPEC `texture map 2` (DUAL only)
	std::string normal2;		// MATERIAL_SPEC `normal map 2` (DUAL only): texture2's normal, lerped in by vertex alpha like it
	float offset[3] = {};		// BUILDING_PART offset X/Y/Z (authoring frame, see WorldBuildings::offsetFrame)
	bool marker = false;		// `is for position marker`: the offset marks a standing spot, not a model position
	int rotFunction = 0, rotAxis = 1;	// `rotation function` (0 none, 1-3 spinners, 4/5 wind/turret aiming), `rotation axis` (0 X, 1 Y, 2 Z)
	float rotSpeed = 0;		// deg/s (larger magnitude of `rotation speed` min/max)
	bool aimed = false;		// a wind-aimed part (rotation function 4) or nested in one: it turns with the wind about aimPivot (mesh.glsl inst_vs)
	float aimPivot[3] = {};		// the wind-aimed ancestor's (or its own) offset, the authoring frame like offset
	float aimSpeed = 0;		// deg/s: the wind-aimed ancestor's (or its own) `rotation speed max`, its turn rate toward a new wind
	float windMin = 0, windMax = 0;	// rotation function 3: `wind speed rotation min / max` (u/s), the wind speed's spin ramp
	// Hand-placed features (MAP_FEATURES / FOLIAGE_MESH): the record's MapFeatureMode, -1 for a building part. Then `normal` is the record's
	// normal map (only read for EMISSIVE), `texture2` / `normal2` the second texture and normal map of the dual modes.
	int featureMode = -1;
	int alphaCut = 0;		// 0-255 alpha test threshold on the texture alpha (features; leaves meshes), 0 = none
	bool sway = false;		// a FOLIAGE_MESH `leaves mesh`: the canopy sways in the wind, the trunk stays (foliage.cpp, mesh.glsl inst_vs)
	bool localCoords = false;	// triplanar/terrain mapping in object space instead of world space
	// Interiors (docs/reference/INTERIORS.md). kind: 0 shell (BUILDING `parts`), 1 interior part (`interior`), 2 interior mask (`interior mask`: only its
	// bounds are used, never drawn), 3 furniture from an interiors.level layout. floor: BUILDING_PART `building floor` (a nested part takes the larger of its own
	// and its parent's), 0..3; 0 is never hidden.
	int kind = 0;
	int floor = 0;
	// The exe's INTERIOR material flag (0x100, kenshi-inspect findings/surface_dust_coat.md): an interior part or an interior-layout furniture piece.
	// The dust coat takes the weather's `dust inside` (dustAmount.y) for these, not `dust` (objects.hlsl:213-214, k16.2.202.1).
	bool interior = false;
	// A shell part whose mesh name holds "black" (Outpost01_InteriorBLACK_02, Storm_House_01_Black, ...): the black cut caps. The original draws the shown
	// floor's cap, which blacks out its stairwell (Outpost: BLACK_02, floor 2, is an upward disc just above floor 1's floor); we hide it while a floor cut is active, so the floor below
	// shows through (deliberate deviation, user 2026-11-01).
	bool blackCap = false;
	// A shell part that is a building's outside sign (a mesh under buildings/signs/, or a buildings/misc signboard): hidden while its building is cut to a floor
	// below the top unless render.signs_indoors (the original keeps drawing it; user 2026-11-14, kenshi-k16.2.204).
	bool sign = false;
	// BUILDING_PART `is stairs`: the staircase (else a ramp) from `floor` up to the next one. WorldBuildings reads its mesh for the floor hotspot at the
	// top step (kenshi-k16.2.236.3): the level goes up there when the followed character arrives, and back down at a click on that step.
	bool stairs = false;
	uint32_t firstLight = 0, lightCount = 0;	// the part record's `lights` in BuildingSet::lights
	uint32_t firstEffect = 0, effectCount = 0;	// the part record's `effects` (EFFECT instances) in BuildingSet::effects
};

// An EFFECT instance on a BUILDING_PART, BUILDING or MAP_FEATURES record: which ParticleUniverse script the world spawns there, and the
// instance's local transform. Data only in slice 1; WorldBuildings turns visible ones into PlacedEffectRt.
struct EffectDef
{
	std::string script;		// EFFECT `particle system` (the .pu `system` name)
	int type = 0;			// EFFECT `type` (0 = a world-instanced fire/smoke/spark; weather refs are 1-10)
	bool windAffected = false;	// EFFECT `wind affected`
	float pos[3] = {};		// instance position in the owner's frame
	float quat[4] = { 1, 0, 0, 0 };	// instance rotation (w, x, y, z)
};

// A LIGHT instance on a BUILDING_PART (`lights`; kenshi-inspect light_record.md). The exe ignores the record's `buildings` / `characters` / `landscape`
// (shadow bools) and casts no shadows; `variance` is a one-time offset on the power, not a flicker.
struct LightDef
{
	float pos[3];			// the instance position in the part's frame, added to BuildingPartDef::offset
	float dir[3];			// spot direction: the instance rotation applied to (0, -1, 0) (the bar lamps' identity points down, live)
	float colour[3];		// `diffuse` 0x00RRGGBB / 255
	float brightness, variance, radius;
	float cosInner, cosOuter, falloff;	// spots: cos of the half cone (`inner` / `outer` are full angles in degrees)
	bool spot;			// `type` 1; 0 = point
	int effect;			// 0 none, 1 PULSE, 2 FLICKER (steady in the game), 3 SHIMMER
};

// A feature mode (kworld::FeatureMode) that samples the record's UV textures, so its normal map lights it: UV_MAPPED, DUAL_TEXTURE, FOLIAGE, EMISSIVE
// (TRIPLANAR, TERRAIN and DUAL_TRIPLANAR map by position)
inline bool uvMapped(int featureMode) { return featureMode == 0 || featureMode == 3 || featureMode == 4 || featureMode == 6; }

struct BuildingInstance
{
	double x = 0, y = 0, z = 0;	// y absolute (terrain + instance y)
	float qw = 1, qx = 0, qy = 0, qz = 0;
	float scale = 1;
	float stretch[3] = { 1, 1, 1 };	// per-axis factor on top of `scale` (non-uniform feature scale; may be negative)
	bool feature = false;		// hand-placed feature, not a building
	bool prop = false;		// a sim item / container drawn as a synthetic instance (WorldBuildings::setProps): no building and no floor of its own
	bool propCut = false;		// a prop on a floor the cut hides (WorldBuildings::Prop::cut)
	// A ground severed limb with the wire's tint (WorldBuildings::Prop::limb): the skin tone, the blood colour 0..1 and the bone scale; all zero /
	// one when the server sent no tail, and the item draws as before
	float limbSkin[3] = { 0, 0, 0 }, limbBlood[3] = { 0, 0, 0 };
	bool limb = false;
	float ground = 0;		// terrain height under the origin
	uint32_t firstPart = 0, partCount = 0;	// range in BuildingSet::partRefs
	// Interiors. A furniture instance (parent >= 0) is a BUILDING from the layout, placed in the building instance `parent`; its position and
	// rotation are already composed with the parent's.
	int32_t parent = -1;
	int8_t floor = -1;		// furniture: `floor` of its GAMESTATE_BUILDING, -1 = derive from its height in the building
	// interior furniture: the building placement's doorKey and the piece's index in the collection's instance list (every entry counted), the two
	// the wire's container tail names (SIMLINK "Tail (kind 3 only)"): a container picks and draws by this piece (WorldBuildings::setProps)
	uint64_t key = 0;
	uint32_t piece = 0;
	bool ceiling = false;		// furniture: BUILDING `ceiling placement` (lamps): it hangs from the floor above, so it belongs to the one below
	uint64_t door = 0;		// a door leaf (parent = its building): its kworld::DoorKey; the leaf draws moved by WorldBuildings::setDoors' offset
	uint8_t maxFloor = 0;		// building: highest `building floor` over its shell and interior parts
	uint32_t nameId = 0;		// building: index into BuildingSet::names
	float height = 100;		// building: BUILDING `building height` (mesh units): the top of the construction cut (WorldBuildings::setConstruction)
	// EFFECT / LIGHT instances on the building record itself (Campfire/Cooking Pit effects, the campfire and Holy torch lights). The range
	// lives on the instance because furniture instances share the record but their transform is already composed with the parent's.
	uint32_t firstEffect = 0, effectCount = 0;
	uint32_t firstOwnLight = 0, ownLightCount = 0;
};

struct BuildingSet
{
	std::vector<BuildingPartDef> parts;
	std::vector<BuildingInstance> instances;
	std::vector<uint32_t> partRefs;		// indices into parts
	std::vector<LightDef> lights;		// BuildingPartDef::firstLight
	std::vector<EffectDef> effects;		// BuildingPartDef::firstEffect; BuildingInstance::firstEffect is the building/feature-level list
	std::vector<LightDef> ownLights;	// LIGHT instances on BUILDING records (BuildingInstance::firstOwnLight); read, not drawn yet (slice 2)
	std::unordered_map<int, std::vector<uint32_t>> cells;	// zone index (x + 64 z) -> instance indices
	size_t placedRaw = 0, noGeometry = 0, missingTarget = 0;
	size_t typeCount[5] = {};		// part uses per BuildingShader value, over all instances
	// Hand-placed features (kworld::Features): features.dat placements, those with a drawable mesh, hidden markers, missing records, road foliage points
	// (no record, not drawable), instances per MapFeatureMode.
	size_t featurePlacements = 0, featureInstances = 0, featureHidden = 0, featureMissing = 0, roadFoliage = 0;
	size_t featureModes[7] = {};
	// Interiors: furniture instances live in `interiorCells` (zone index -> instance indices, same instance vector) so the client can stream them by
	// camera distance instead of by zone radius. `floorable` lists the building instances with maxFloor >= 1; `names` the distinct BUILDING names.
	std::unordered_map<int, std::vector<uint32_t>> interiorCells;
	std::vector<uint32_t> floorable;
	std::unordered_map<uint64_t, uint32_t> byKey;	// building instances by kworld::doorKey(placementId): the wire's CHAR_DYNAMIC `building`
	std::vector<std::string> names;
	int editorPlatformRef = -1;	// index in partRefs of the character editor's platform part (BUILDING_PART 5370-platform_otto.mod), -1 when absent
	// Storage (BUILDING `has inventory`: crates, barrels, shelves): shell parts by record stringID, for the sim's containers that no placed instance
	// draws (WorldBuildings::setProps). Resolved with the stringID as seed: one look per record.
	struct PartRange { uint32_t first = 0, count = 0; };
	std::unordered_map<std::string, PartRange> storage;
	// Every other BUILDING record's shell parts, by stringID: the player's buildings (SIMLINK BUILDING 47) that no placed instance draws, drawn as
	// a world-placed building of that record is (WorldBuildings::setProps, Prop::building). height / nameId as BuildingInstance's.
	struct Shell { uint32_t first = 0, count = 0, nameId = 0; float height = 100; };
	std::unordered_map<std::string, Shell> shells;
	std::unordered_set<std::string> shopCounters;	// the has-inventory records whose `functionality` has `function` 9 (a shop counter, docs/formats/TOWN_ROUTINE_DATA.md): CURSORS_LIVE B4b's building type 9 (UNSURE: the exe's type and this function id are taken as one)
	// Ground items (the sim's kind 2 things): one part per item record, its `ground mesh`, else `mesh` for ITEM / NEST_ITEM, textured by its
	// `material` spec or the mesh's own script materials. By record stringID. Weapons and crossbows wait for the wire (their texture and scale are
	// the instance's MATERIAL_SPECS_WEAPON model, their rest pose the sim's); armour and backpacks have only a worn, skinned mesh.
	std::unordered_map<std::string, PartRange> items;
	size_t layoutPlacements = 0, layoutJoined = 0, layoutMissing = 0, exteriorPlacements = 0, exteriorJoined = 0, ruinedPlacements = 0, furnitureInstances = 0, doorInstances = 0, furnitureNoGeometry = 0, interiorPartUses = 0;
	double loadMs = 0, interiorLoadMs = 0;
	std::string report;

	// Applies only the four shipped data files (no .mod support in kworld yet). Empty set + report on failure.
	// features: also place the hand-placed features (features.dat; M2d).
	static BuildingSet load(const std::string& dataDir, bool features = true);
};
