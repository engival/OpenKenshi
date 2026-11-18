#pragma once
#include "audio/mixer.hpp"
#include "audio/wwhirc.hpp"
#include <cstdint>
#include <deque>
#include <memory>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

// Wwise interactive-music playback for Kenshi's music banks (docs/reference/audio/MUSIC.md, AUDIO_EXE.md section 4), over the Mixer.
// MusicPlayer runs events (Play / Stop / SetState / SetSwitch) against the hierarchy: a played node becomes an Instance that walks its
// playlist one segment at a time and schedules each segment's tracks on the mixer clock, so pieces chain on their Exit/Entry cues. Music
// switches pick their child from the current state/switch values and cross over with their transition rules when a value changes.
// GameMusic is the game side: biome -> Music_<Biome>_Play, and the re-trigger timer of the "Music frequency" setting.
namespace audio
{
	class Bank;

	// Wwise short-id hash: FNV-1 32-bit over the lower-cased name.
	uint32_t wwiseHash(const std::string& name);

	class MusicPlayer
	{
	public:
		MusicPlayer(Mixer& mixer, const hirc::Hierarchy& h, uint32_t seed = 0);
		~MusicPlayer();

		bool postEvent(uint32_t eventId);	// false if the event is unknown
		void setState(uint32_t group, uint32_t value);
		void setSwitch(uint32_t group, uint32_t value);
		// Schedules what is due within the look-ahead window and retires finished instances. Call at least every ~100 ms.
		void update();

		size_t instances() const { return instances_.size(); }
		// One line per playing instance: node, current segment, time left.
		std::string describe() const;
		struct Played { uint64_t start; uint32_t node, segment; };
		std::vector<Played> history;	// every segment scheduled, for tests

	private:
		struct Walker;
		struct Instance;
		void play(uint32_t node, float fadeS, Curve c, uint64_t at);
		void stop(uint32_t node, float fadeS, Curve c);
		uint32_t resolve(uint32_t node) const;	// through switches to a playlist or segment
		uint32_t nextSegment(Instance& in);
		void schedule(Instance& in, uint32_t seg, uint64_t start, float fadeS, Curve c);
		void transition(Instance& in, uint32_t target);
		float gainDb(uint32_t node) const;
		uint64_t frames(double ms) const;

		Mixer& mix_;
		const hirc::Hierarchy& h_;
		std::mt19937 rng_;
		std::unordered_map<uint32_t, uint32_t> states_, switches_;
		std::unordered_map<const hirc::Item*, std::unique_ptr<Walker>> walkers_;	// playlist position and history, kept across plays
		std::vector<std::unique_ptr<Instance>> instances_;
	};

	// Banks by name from the game's data/audio directory, loaded once and fed into one hierarchy.
	class BankSet
	{
	public:
		explicit BankSet(std::string dir);
		~BankSet();
		bool load(const std::string& name);	// "Savannah_Music_Bank" (".bnk" optional)
		bool loaded(const std::string& name) const;
		const hirc::Hierarchy& hierarchy() const { return h_; }
		const std::string& error() const { return error_; }

	private:
		std::string dir_, error_;
		std::unordered_map<std::string, std::unique_ptr<Bank>> banks_;
		hirc::Hierarchy h_;
	};

	// The game's music logic (AUDIO_EXE.md section 4): the area's music biome selects Music_<Biome>_Play, posted on a biome change and then
	// again every rand[0.8, 1.4] x frequency minutes while the music volume is above 0. Time_in_Game (Day/Night) feeds the day/night playlists.
	class GameMusic
	{
	public:
		enum Biome { kDunes, kSavannah, kSwamplands, kTarsands, kNone };
		GameMusic(BankSet& banks, Mixer& mixer, uint32_t seed = 0);
		~GameMusic();

		void setBiome(Biome b);
		void setDay(bool day);
		void setFrequencyMinutes(float minutes);
		void setVolume(float v);
		void update();	// call every frame or so
		MusicPlayer* player() { return player_.get(); }

	private:
		void trigger();
		double now() const;

		BankSet& banks_;
		Mixer& mix_;
		std::unique_ptr<MusicPlayer> player_;
		std::mt19937 rng_;
		Biome biome_ = kNone;
		bool day_ = true;
		float freqMin_ = 5, volume_ = 1;
		double next_ = 0;	// seconds on the mixer clock
	};
}
