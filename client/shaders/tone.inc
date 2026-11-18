// The original's display (user 2026-11-10, k16.2.82: the client's one base). The game's hdrfp4 Composite scales the scene colour by EXPOSURE_KEY / adapted
// and hard-clips it to 0..1: no curve and no gamma encode (hdr_to_gamma false live; kenshi-inspect findings/sky_dawn_exposure.md). The textures are sampled
// raw, so this passes them through at unit light; the low sun's saturated disc and yellow rim come from this clip. The AgX / Khronos Neutral curves and the
// sRGB encode are retired (client/retired/tone_curves.glsl).
@block tone
vec3 tone_map(vec3 c)
{
	return clamp(c, 0.0, 1.0);
}
@end
