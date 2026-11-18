// Screen-door fade: interleaved gradient noise (Jimenez), a stable per-pixel threshold in [0,1).
@block fade_ign
float ign(vec2 p)
{
	return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));
}
@end
