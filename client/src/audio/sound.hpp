#pragma once
#include "audio/mixer.hpp"
#include "audio/wwhirc.hpp"
#include <cstdint>
#include <memory>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

// Wwise sound-structure playback (ambience, effects, voice) over the Mixer. Events run their actions: Play / Stop on Sound, Random/Sequence,
// Switch and Layer containers; Set/Reset Volume, Pitch and LPF on any node (Enter_Building muffles the outdoor mixer this way); SetState and
// SetSwitch. A played node becomes a tree of runs: containers pick their children and chain them on the mixer clock, layers start all of theirs.
// update() re-evaluates every voice's volume, pitch and LPF from its node chain (props, per-play random ranges, game-parameter curves, action
// offsets), so game parameters such as Camera_Zoom and Intensity act on sounds that are already playing. docs/AUDIO_ENGINE.md lists what is
// interpreted rather than known.
namespace audio
{
	class BankSet;

	class SoundPlayer
	{
	public:
		SoundPlayer(Mixer& mixer, const hirc::Hierarchy& h, uint32_t seed = 0);
		~SoundPlayer();

		// `object` 0 = the player's own global object (UI, ambience: no position). A registered object (setObject) posts a positioned event: a 3D
		// sound is attenuated by the distance object -> listener (the sound's attenuation object: volume and low-pass curves) and panned by the
		// side the object is on, and follows the object while it plays. `gate` > 0: the original's per-post distance gate (the post is dropped, not
		// played quietly, when the object is farther than that; FUN_1405c9a70 maxDistSq). Returns false if the event is unknown or the post was gated.
		bool postEvent(uint32_t eventId, Bus bus = kSfx, uint32_t object = 0, float gate = 0);
		void setRtpc(uint32_t id, float value);
		void setState(uint32_t group, uint32_t value);
		void setSwitch(uint32_t group, uint32_t value, uint32_t object = 0);	// a switch on an object is read by the sounds posted on it, before the global one
		// World sound objects. setObject registers or moves one (call each frame); removeObject forgets it, sounds already playing on it stay where
		// it was. setListener: position and the camera's forward and right unit vectors (+y up).
		void setObject(uint32_t object, const double pos[3]);
		void removeObject(uint32_t object);
		void setListener(const double pos[3], const double fwd[3], const double right[3]);
		bool hasObject(uint32_t object) const { return emitters_.count(object) != 0; }
		float objectDistance(uint32_t object) const;	// to the listener; -1 if the object is not registered
		void update();	// call at least every ~100 ms

		size_t instances() const { return instances_.size(); }
		std::string describe() const;	// one line per playing instance
		struct Played { uint64_t start; uint32_t sound; VoiceId voice; float gainDb, pan, rate; uint32_t object; float distance; };
		std::vector<Played> history;	// every voice started, for tests

	private:
		struct Ramp;
		struct Mods;
		struct Walker;
		struct Run;
		struct Instance;
		struct Eval { float db = 0, cents = 0, lpf = 0, lin = 1; };
		struct Emitter { double pos[3] = {}; std::unordered_map<uint32_t, uint32_t> switches; };
		struct Spatial { bool is3d = false; float distance = 0, pan = 0, db = 0, lpf = 0; };
		Spatial spatial(const Run& r, const Instance& in) const;

		void play(uint32_t node, Bus bus, float fadeS, Curve c, uint64_t at, uint32_t object);
		void stop(uint32_t target, float fadeS, Curve c);
		void stopRun(Run& r, float fadeS, Curve c);
		void setMod(const hirc::Action& a, uint64_t now);
		std::unique_ptr<Run> start(uint32_t node, Run* parent, Instance& in, uint64_t at);
		void startNext(Run& r, Instance& in, uint64_t at);
		uint32_t pick(const hirc::Container& c, Walker& w);
		void tick(Run& r, Instance& in, uint64_t now);
		Eval eval(const Run& r, const Instance& in, uint64_t t) const;
		void addParams(const hirc::Params& p, uint32_t id, Eval& e, uint64_t t) const;
		uint32_t syncValue(uint8_t groupType, uint32_t group, uint32_t def, uint32_t object) const;
		float rtpc(uint32_t id) const;
		uint64_t frames(double ms) const;

		Mixer& mix_;
		const hirc::Hierarchy& h_;
		std::mt19937 rng_;
		std::unordered_map<uint32_t, uint32_t> states_, switches_;
		std::unordered_map<uint32_t, Emitter> emitters_;
		double listenerPos_[3] = {}, listenerFwd_[3] = { 0, 0, 1 }, listenerRight_[3] = { 1, 0, 0 };
		std::unordered_map<uint32_t, float> rtpcs_;
		std::unordered_map<uint32_t, std::unique_ptr<Mods>> mods_;	// node -> action offsets (SetVolume etc.)
		std::unordered_map<uint32_t, std::unique_ptr<Walker>> walkers_;	// container -> playlist state kept across plays
		std::vector<std::unique_ptr<Instance>> instances_;
	};

	// The game's ambience logic (AUDIO_EXE.md section 3): the area's ambience zone loads SFX_Amb_<zone> and posts Enter_<zone>_Day / _Night
	// (Leave_<zone> on a zone change); the camera height above ground drives Camera_Zoom; weather drives Weather_Rain_* / Weather_Wind_* and the
	// Intensity / Rain_Intensity parameters. SFX_Amb_Weather_Bank and SFX_Amb_Indoors_Bank load up front, as the game's boot pass does.
	class GameAmbience
	{
	public:
		enum Zone { kAshlands, kBoneyard, kCanyons, kCoast, kDesert, kForest, kJungle, kPlains, kSwamplands, kTarsands, kNoZone };
		static const char* zoneName(Zone z);
		static Zone zoneByName(const std::string& name);	// kNoZone if unknown

		GameAmbience(BankSet& banks, Mixer& mixer, uint32_t seed = 0);
		~GameAmbience();

		void setZone(Zone z);
		void setDay(bool day);
		// Enter_Building / Enter_Building_Big / Leave_Building (1406b0140): on entry the Indoors switch = material (Metal / Stone / Wicker, from the building's
		// `interior ambience`; "" leaves it) and the Building_Integrity RTPC = integrity (>= 0) are set first. No-op when `inside` did not change.
		void setIndoors(bool inside, bool big = false, const std::string& material = std::string(), float integrity = -1);
		// Height of the camera above the ground under it, world units (the game: (camera y - ground y) / 2000 * 100 -> Camera_Zoom).
		void setCameraHeight(float h);
		void setRain(float intensity);	// the game's rain value: > 0 raining
		void setWind(float intensity);	// the game's Intensity value, 0 .. 100
		void setVolume(float v);
		void update();
		SoundPlayer* player() { return player_.get(); }

	private:
		void enter();
		bool post(const std::string& event);

		BankSet& banks_;
		Mixer& mix_;
		std::unique_ptr<SoundPlayer> player_;
		Zone zone_ = kNoZone, entered_ = kNoZone;
		bool day_ = true, enteredDay_ = true, inside_ = false;
		float rain_ = 0, wind_ = 0;
	};
}
