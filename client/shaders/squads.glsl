// Sim squads as upright pillars: one instanced draw of a unit box. Instance = world position of the pillar's base + colour.
@vs vs
layout(binding=0) uniform vs_params
{
	mat4 mvp;       // projection * rotation-only view (camera-relative world)
	vec4 cam;       // xyz = camera position
};

in vec3 pos;        // unit box: xz in -1..1, y in 0..1
in vec3 ipos;       // instance base, world
in vec3 icol;

out vec3 v_col;
out float v_shade;

void main()
{
	vec3 rel = ipos - cam.xyz;
	float dist = length(rel);
	float h = max(60.0, dist * 0.03);	// a few metres up close, grows so it stays visible from the air
	vec3 p = rel + vec3(pos.x * h * 0.12, pos.y * h, pos.z * h * 0.12);
	v_col = icol;
	v_shade = 0.6 + 0.4 * pos.y;
	gl_Position = mvp * vec4(p, 1.0);
}
@end

@fs fs
in vec3 v_col;
in float v_shade;
out vec4 frag_color;

void main()
{
	frag_color = vec4(v_col * v_shade, 1.0);
}
@end

@program squads vs fs
