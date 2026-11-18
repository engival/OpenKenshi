// Clears the depth of the current viewport/scissor to the far value (1) without clearing the whole attachment: one oversized triangle at z = 1, depth compare ALWAYS.
// Used by Shadow::selectCascade so a cascade tile can be redrawn while the other tiles of the atlas keep their last contents.
@vs shadowclear_vs
void main()
{
	vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
	gl_Position = vec4(p * 2.0 - 1.0, 1.0, 1.0);
}
@end

@fs shadowclear_fs
void main()
{
}
@end

@program shadowclear shadowclear_vs shadowclear_fs
