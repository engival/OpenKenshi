#pragma once
#include "audio/mixer.hpp"
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

// HIRC objects (bank version 112): Event, Action, the interactive-music nodes (Music Segment / Track / Random-Sequence / Switch) and the sound
// structure (Sound, Random/Sequence, Switch and Layer containers, Actor-Mixer). Music decoders are a C++ port of tools/wwise_bnk.py; the sound
// structure follows wwiser's wparser.py v112 branches (format knowledge from bnnm/wwiser, read as documentation). docs/reference/audio/MUSIC.md
// describes Kenshi's music trees. Every decoded object must consume exactly its declared size. Other object types are skipped.
namespace audio
{
	class Bank;

	namespace hirc
	{
		enum Sync : uint32_t { kImmediate, kNextGrid, kNextBar, kNextBeat, kNextMarker, kNextUserMarker, kEntryMarker, kExitMarker, kExitNever, kLastExitPosition };
		enum PlayType : int32_t { kLeaf = -1, kContinuousSequence, kStepSequence, kContinuousRandom, kStepRandom };
		constexpr uint32_t kAny = 0xFFFFFFFF;	// transition rule wildcard
		// AkPropID (v90..113) values the players use.
		enum PropId : uint8_t
		{
			kPropVolume = 0x00, kPropPitch = 0x02, kPropLpf = 0x03, kPropHpf = 0x04, kPropBusVolume = 0x05, kPropDelayTime = 0x0E,
			kPropTransitionTime = 0x0F, kPropProbability = 0x10, kPropMakeUpGain = 0x21, kPropLoop = 0x3A, kPropInitialDelay = 0x3B,
		};
		// AkRTPC_ParameterID (v<=113) values the players use.
		enum RtpcParam : uint8_t { kParamVolume = 0x00, kParamPitch = 0x02, kParamLpf = 0x03, kParamHpf = 0x04, kParamBusVolume = 0x05, kParamMakeUpGain = 0x24 };

		struct Fade { int32_t ms = 0; Curve curve = kLinear; int32_t offsetMs = 0; };
		struct Rule
		{
			std::vector<uint32_t> src, dst;
			Fade srcFade, dstFade;
			uint32_t sync = kExitMarker, jumpTo = 0, transitionSegment = 0;
			uint16_t entry = 0;
			bool postExit = true, preEntry = true;
		};
		struct Meter { double gridMs = 1000, gridOffsetMs = 0; float bpm = 120; uint8_t beatsPerBar = 4, beatValue = 4; };

		struct Point { float x = 0, y = 0; Curve curve = kLinear; };
		float evalPoints(const std::vector<Point>& points, float x);	// unscaled graph lookup
		// A curve over a game parameter: eval() gives y in the property's units (volume in dB, pitch in cents) after the curve's scaling.
		struct Rtpc
		{
			uint32_t id = 0, curveId = 0;
			uint8_t type = 0, accum = 0, param = 0, scaling = 0;
			std::vector<Point> points;
			float eval(float x) const;
		};
		struct Prop { uint8_t id; uint32_t raw; };
		struct RangedProp { uint8_t id; float min, max; };	// randomized on each play
		// CAkParameterNodeBase: the parameters every music and sound-structure node carries.
		struct Params
		{
			uint32_t parent = 0, bus = 0;
			std::vector<Prop> props;
			std::vector<RangedProp> ranged;
			bool posOverride = false, is3d = false;	// positioning is inherited unless posOverride
			uint32_t attenuation = 0;
			std::vector<Rtpc> rtpcs;
			std::vector<uint32_t> stateGroups;
			bool has(uint8_t id, uint32_t& raw) const;
			float f(uint8_t id, float def = 0) const;
			int32_t i(uint8_t id, int32_t def = 0) const;
		};

		// CAkAttenuation (HIRC 0x0E, v112): the distance curves of a 3D sound. x = distance (world units, 1 u = 1 game unit: the footstep curve
		// ends at 1000, its gate's distance), y as the property's scaling gives it (volume: scaling 2, the stored [-1, 0] = 20 log10(y + 1) dB, -1 =
		// silence; LPF: scaling 0, the Wwise LPF amount 0..100). curveToUse[k] indexes `curves` or is 0xFF (none): 0 volume dry, 1 aux game-defined,
		// 2 aux user-defined, 3 low-pass, 4..6 spread / focus (not applied).
		struct AttCurve { uint8_t scaling = 0; std::vector<Point> points; };
		struct Attenuation
		{
			uint32_t id = 0;
			bool cone = false;
			float coneInside = 0, coneOutside = 0, coneOutsideDb = 0, coneLpf = 0;	// degrees, degrees, volume (stored), LPF (a high-pass float follows in the bank, not kept; the cone is not applied)
			uint8_t curveToUse[7] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
			std::vector<AttCurve> curves;
			const AttCurve* volume() const { return curveToUse[0] < curves.size() ? &curves[curveToUse[0]] : nullptr; }
			const AttCurve* lowPass() const { return curveToUse[3] < curves.size() ? &curves[curveToUse[3]] : nullptr; }
			float maxDistance() const;	// the largest x on any curve
			float volumeDb(float distance) const;	// 0 with no volume curve; -96.3 at silence
			float lpf(float distance) const;	// 0 with no low-pass curve
		};

		struct Node	// shared music-node fields
		{
			uint32_t id = 0, parent = 0;
			float volumeDb = 0;
			std::vector<uint32_t> children;
			Meter meter;
			std::vector<Rule> rules;
		};
		struct Segment : Node { double durationMs = 0, entryMs = 0, exitMs = 0; };
		struct Clip { uint32_t sourceId = 0; double playAtMs = 0, beginTrimMs = 0, endTrimMs = 0, srcDurationMs = 0; };
		struct Track { uint32_t id = 0; float volumeDb = 0; std::vector<uint32_t> sources; std::vector<Clip> clips; uint8_t type = 0; };
		struct Item
		{
			uint32_t segment = 0, id = 0, weight = 50000;
			int32_t type = kLeaf;
			int16_t loop = 1;	// 0 = infinite
			uint16_t avoidRepeat = 0;
			bool useWeight = false, shuffle = false;
			std::vector<Item> children;
		};
		struct RanSeq : Node { Item root; };
		struct TreeNode { uint32_t key = 0, audio = 0; std::vector<TreeNode> children; };
		struct Switch : Node
		{
			bool continuePlayback = true;
			std::vector<uint32_t> groups;	// one per tree level
			std::vector<uint8_t> groupIsState;
			TreeNode tree;
		};
		struct Action
		{
			uint16_t type = 0;	// high byte = class (1 Stop, 4 Play, 0x12 SetState, 0x19 SetSwitch, ...)
			uint32_t target = 0, group = 0, value = 0;
			int32_t fadeMs = 0, delayMs = 0;
			Curve curve = kLinear;
			// SetPitch/Volume/BusVolume/LPF (classes 0x08-0x0F; odd classes reset): meaning 0 default, 1 absolute, 2 offset; base randomized by
			// [min, max].
			uint8_t propMeaning = 0;
			float propBase = 0, propMin = 0, propMax = 0;
			uint8_t cls() const { return uint8_t(type >> 8); }
		};
		struct Event { std::vector<uint32_t> actions; };

		// Sound structure. Object is also the whole Actor-Mixer.
		struct Object { uint32_t id = 0; Params p; std::vector<uint32_t> children; };
		constexpr uint32_t kPluginSilence = 0x00650002;	// Wwise Silence source: no media, only a duration
		struct Sound : Object
		{
			uint32_t source = 0, plugin = 0;
			uint8_t stream = 0;	// 0 = embedded in a bank's DATA
			float silenceS = 0, silenceMinusS = 0, silencePlusS = 0;	// kPluginSilence: duration and its random range
		};
		struct Container : Object	// CAkRanSeqCntr
		{
			enum Mode : uint8_t { kRandom, kSequence };
			enum Transition : uint8_t { kNone, kCrossfadeAmp, kCrossfadePower, kDelay, kSampleAccurate, kTriggerRate };
			uint16_t loopCount = 1, loopModMin = 0, loopModMax = 0, avoidRepeat = 0;	// loopCount 0 = infinite
			float transitionMs = 0, transitionModMinMs = 0, transitionModMaxMs = 0;
			uint8_t transition = kNone, randomMode = 0, mode = kRandom;	// randomMode 1 = shuffle
			bool resetEachPlay = false, restartBackward = false, continuous = false, global = false;
			struct Entry { uint32_t id; int32_t weight; };
			std::vector<Entry> playlist;
		};
		struct SwitchCntr : Object
		{
			uint8_t groupType = 0;	// 0 switch, 1 state
			uint32_t group = 0, defaultValue = 0;
			bool continuousValidation = false;
			struct Package { uint32_t value; std::vector<uint32_t> nodes; };
			std::vector<Package> packages;
			struct NodeParams { uint32_t node; bool firstOnly, continuePlayback; uint8_t onSwitch; int32_t fadeOutMs, fadeInMs; };	// onSwitch 0 play to end, 1 stop
			std::vector<NodeParams> nodeParams;
		};
		struct Layer
		{
			uint32_t id = 0, crossfadeRtpc = 0;
			uint8_t crossfadeRtpcType = 0;
			std::vector<Rtpc> rtpcs;
			struct Assoc { uint32_t child; std::vector<Point> curve; };
			std::vector<Assoc> assocs;
		};
		struct LayerCntr : Object { std::vector<Layer> layers; };

		// Objects from every added bank. Music switches repeat across banks with only their children differing: those are merged. Other repeats keep
		// the first copy, so pointers into the maps (the player's playlist items) stay valid as banks are added.
		class Hierarchy
		{
		public:
			bool add(const Bank& bank);
			const std::string& error() const { return error_; }

			std::unordered_map<uint32_t, Event> events;
			std::unordered_map<uint32_t, Action> actions;
			std::unordered_map<uint32_t, Segment> segments;
			std::unordered_map<uint32_t, Track> tracks;
			std::unordered_map<uint32_t, RanSeq> ranseqs;
			std::unordered_map<uint32_t, Switch> switches;
			std::unordered_map<uint32_t, Sound> sounds;
			std::unordered_map<uint32_t, Container> containers;
			std::unordered_map<uint32_t, SwitchCntr> switchCntrs;
			std::unordered_map<uint32_t, LayerCntr> layerCntrs;
			std::unordered_map<uint32_t, Object> mixers;
			std::unordered_map<uint32_t, Attenuation> attenuations;
			std::unordered_map<uint32_t, uint32_t> buses;	// Wwise bus (Init.bnk) -> its parent bus, 0 at the master buses
			std::unordered_map<uint32_t, const Bank*> media;	// media id -> the bank whose DATA holds it

			const Node* node(uint32_t id) const;	// music
			const Object* object(uint32_t id) const;	// sound structure
			// The 3D positioning a node plays with: the nearest node up its chain with a positioning override decides (is3d, attenuation id).
			struct Positioning { bool is3d = false; uint32_t attenuation = 0; };
			Positioning positioning(uint32_t id) const;
			size_t skipped = 0;	// undecoded object types

		private:
			std::string error_;
		};
	}
}
