#pragma once
#include <cmath>

// Retired 2026-11-10 (kenshi-k16.2.80): the sRGB decode of the display-authored particle colours/textures and character tints. The scene target
// holds the game's display-referred values again (the display base of k16.2.82); the game never decodes, so the colours and textures are used raw
// (Fable 5.2 review items 1+2, water/particles/character shaders as the original's hlsl). The last users were particles.cpp and characters.cpp.
// Kept for reference only; not built, not included.

// sRGB EOTF (IEC 61966-2-1): a display-authored colour to linear light — for the retired k16.2.52 linear-radiance scene target, rejected by the display base
// of k16.2.82 (the game never decodes).
inline float srgbDecode(float c)
{
	return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}
