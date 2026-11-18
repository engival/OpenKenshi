#pragma once
#include <cstdint>
#include <string_view>

// The Wwise event and switch names the host posts, as ids: FNV-1 32 of the lowercased name (kenshi-inspect
// findings/audio_sfx_triggers.md, "How the posts are made"). The same hash as audio::wwiseHash (music.cpp), small enough for a constexpr,
// so a game event's id sits beside its name; a posted name that is not in the loaded banks is dropped by Wwise, so the asserts below are the
// only check there is.
namespace audio
{
	constexpr uint32_t wwiseHashName(std::string_view name)
	{
		uint32_t h = 2166136261u;
		for (char ch : name)
		{
			h *= 16777619u;
			h ^= uint8_t(ch >= 'A' && ch <= 'Z' ? ch + 32 : ch);
		}
		return h;
	}

	// The game's own events the host posts (SFX_General_Bank; audio_sfx_triggers.md sections 1 and 2).
	constexpr uint32_t kRightClick = wwiseHashName("Right_Click");	// 1412919228: the click of an order the selection got from a world right click, once per order (1407f9e20)
	constexpr uint32_t kStealth = wwiseHashName("Stealth");		// 2909291642: the sneak toggle turned on (140721410)
	constexpr uint32_t kGeneralClick = wwiseHashName("General_Click");	// 3551211898: the sneak toggle turned off, and every generic widget click
	constexpr uint32_t kChangeLevel = wwiseHashName("Change_Level");	// 2600852838: a floor change by key (the original's PgUp / PgDn)
	static_assert(kRightClick == 1412919228u);
	static_assert(kStealth == 2909291642u);
	static_assert(kGeneralClick == 3551211898u);
	static_assert(kChangeLevel == 2600852838u);
}
