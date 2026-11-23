// The road overlay's ribbons (kenshi-qjzb, parent kenshi-y8ni / k16.2.515): the road network as terrain-draped strips in the scene pass, so the scene depth decides what hides
// them. Two draws of the same cached mesh: the visible one (depth LESS_EQUAL) and the hidden one (depth GREATER: what a building or the terrain covers), dimmed and dashed.
// Vertex = the point (world, draped on the terrain) + its side (unit xz normal) and the strip width it belongs to, the colour, the distance along the road and its offset from the centre line (direction mode, k16.2.515: strips run from the centre line out to the right-hand side).
// The width is real (the ribbon's own), widened on the GPU to a minimum on screen, so a far road stays a visible line. The near plane clips the strip's triangles itself.
@vs rr_vs
layout(binding=0) uniform rr_vs_params
{
	mat4 mvp;       // projection * rotation-only view (camera-relative world)
	vec4 cam;       // xyz = the camera position (world)
	vec4 p0;        // x = the minimum half width on screen as a world width per clip w (pixels / (0.5 h f)), y = the width scale (1 ribbon, 0 thin line), z = range, w = fade from
	vec4 p1;        // x = dash length (u), y = 0 the visible draw / 1 the hidden draw, z = the colour gain, w = the thinnest dash cell as a world length per clip w
};

in vec3 pos;        // the edge point, world
in vec3 side;       // xy = the signed xz normal toward this edge, z = the half width baked in the position
in vec4 col;        // rgb, a = the distance along the road (u)
in vec2 aux;        // x = this vertex's offset from the centre line toward its side (u, baked in the position), y = 1 for a hatched strip (a same-direction duplicate)

out vec4 v_col;
out float v_dash;
out float v_hatch;

void main()
{
	vec3 rel = pos - cam.xyz;
	float dist = length(rel.xz);
	vec2 centre = rel.xz - side.xy * aux.x;
	// the width w at this depth: the clip w of the centre line, close enough along a vertex pair
	vec4 c0 = mvp * vec4(centre.x, rel.y, centre.y, 1.0);
	float hw = max(side.z * p0.y, p0.x * max(c0.w, 0.0));	// the full strip width (side.z, baked) widened to the on-screen minimum; the vertex keeps its fraction of it
	float off = aux.x * hw / side.z;
	vec3 p = vec3(centre.x + side.x * off, rel.y + 1.5 + dist * 0.0015, centre.y + side.y * off);	// the lift over the terrain grows with distance, against depth resolution
	float fade = 1.0 - smoothstep(p0.w, p0.z, dist);
	float dark = 1.0 - 0.6 * min(dist / p0.z, 1.0);
	vec4 clip = mvp * vec4(p, 1.0);
	float cell = max(p1.x, p1.w * max(clip.w, 0.0));
	v_dash = col.a / cell;
	v_hatch = aux.y;
	float alpha = 0.8 * fade;
	vec3 rgb = col.rgb * dark;
	if (p1.y > 0.5) { rgb *= 0.55; alpha *= 0.4; }
	v_col = vec4(rgb * p1.z, alpha);
	gl_Position = clip;
}
@end

@fs rr_fs
layout(binding=1) uniform rr_fs_params
{
	vec4 q0;        // x = 1 for the hidden draw (dashes)
};

in vec4 v_col;
in float v_dash;
in float v_hatch;
out vec4 frag_color;

void main()
{
	if (v_col.a <= 0.0) { discard; }
	if (q0.x > 0.5 && fract(v_dash * 0.5) >= 0.5) { discard; }
	if (v_hatch > 0.5 && fract(v_dash * 0.25) >= 0.5) { discard; }	// a same-direction duplicate: hatched, so it reads as laid over the strip below
	frag_color = v_col;
}
@end

@program roadribbon rr_vs rr_fs
