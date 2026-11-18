#pragma once
#include <cstdint>
#include <string>

struct SkyWeather;

// kclient's audio (docs/AUDIO_ENGINE.md): the game's music and zone ambience from the original Wwise banks, on their own thread. init() returns at once;
// the control thread loads the area table, the sky constants and the boot banks, opens the device, then follows the latest update() at ~20 Hz, so bank
// loads never stall a frame. KCLIENT_NOAUDIO=1 turns it off. shutdown() is safe at any time, also when init() never ran.
void audiohook_init(const std::string& dataDir);
// The camera position and its forward and right unit vectors (the listener: world sounds are attenuated by the distance to it and panned by the
// side of its right vector they are on), the ground height under it, the weather around it (SkyWeather: rain, windIntensity, windSpeed) and the
// clock hours (0..24).
void audiohook_update(const double camPos[3], const double camFwd[3], const double camRight[3], double groundY, const SkyWeather& weather, double hours);
// A UI sound event by its Wwise name (Pause_Game, Game_Speed_x2, Create_or_Load_Game, ...; kenshi-inspect findings/audio_load_pause.md). Queued to
// the audio thread, which runs it on the boot banks (SFX_General_Bank for the sounds, MT_Music_Bank for the menu music) and logs every action and
// voice. Calls before audiohook_init run when the audio thread starts; ignored when audio is off.
void audiohook_event(const std::string& name);
// A UI post (ui::setSoundSink; docs/UI.md "Sounds", kenshi-inspect findings/audio_sfx_triggers.md section 2): the event id (FNV-1 32 of the
// lowercase name, audio/events.hpp) and, when group is not 0, the switch the host sets on the GUI object (110) before the post, both ids with
// their names for the log. Queued like audiohook_event; it runs on the SFX player only (a switch belongs to it).
struct UiPost
{
	uint32_t id = 0, group = 0, value = 0;
	std::string name, groupName, valueName;
};
void audiohook_ui(const UiPost& post);

// World sound objects (docs/AUDIO_ENGINE.md "World sounds"; kenshi-inspect findings/audio_sfx_triggers.md "How the posts are made"): the original posts
// every game sound on an object that has a position (a character, a door), not on the GUI object. The id is the host's (a character's sim id fits;
// 0 is not an object) and must not repeat for two things. Same thread rules as audiohook_ui: every call is queued to the audio thread, in order;
// calls before audiohook_init are dropped, and nothing runs until the first audiohook_update (the listener).
//  - audiohook_object: registers the object or moves it; the host calls it every frame it exists. A sound that is playing follows it.
//  - audiohook_object_remove: forgets it (sounds still playing stay where it was). Do not update it again in the same frame.
//  - audiohook_object_switch: sets a switch on the object by group and value name (Armor_Worn = Cloth); the sounds posted on it read it.
//  - audiohook_object_post: posts an event by name on the object. gate > 0: dropped, not played quietly, when the object is farther than that from
//    the listener (the original's per-post gate: footsteps 200, swings and hits 1000; 0 = none). The event's 3D sounds are attenuated by the
//    attenuation object of their positioning (volume and low-pass curves by distance) and panned by the side the object is on.
void audiohook_object(uint32_t object, const double pos[3]);
void audiohook_object_remove(uint32_t object);
// debug.audio_trace: the per-event world sound lines (and main.cpp's animation-event `notify:` lines) are logged; off by default.
bool audiohook_trace();
void audiohook_object_switch(uint32_t object, const std::string& group, const std::string& value);
void audiohook_object_post(uint32_t object, const std::string& event, float gate = 0);
// The listener's character is inside a building (Enter_Building / Enter_Building_Big / Leave_Building, posted on the world / ambience object): material =
// the Indoors switch (Metal / Stone / Wicker, "" none), integrity = the Building_Integrity RTPC (>= 0), big = the record's `cavernous`. Call each frame.
void audiohook_indoors(bool inside, bool big, const std::string& material, float integrity);
void audiohook_shutdown();
