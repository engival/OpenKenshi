// Retired 2026-11-15 (kenshi-k16.3.34, review of kenshi-k16.3.33): ui::Font::page()'s old size read, kept for reference only; not built,
// not included. It handed stb_truetype the font's size as the ascent-to-descent height (stb's plain font_size), with the line
// round(ascent - descent + gap). MyGUI (and so the game) treats the size as FreeType's em, with the line ceil(ascent) + ceil(descent);
// Exo2's ascent-to-descent is 1.2 em, so this read made every Exo2 font 0.83x the game's (FloaterFont_Small at 720 lines a 9 px line, the
// game's 12, read as "no pixels" in k16.2.275.1). It was switchable as cfg ui.font_em off until k16.3.34 removed the key.

#if 0
	const float px = std::max(4.0f, std::round(size_ * scale_));
	float asc, desc, gap;
	stbtt_GetScaledFontVMetrics(data, 0, px, &asc, &desc, &gap);
	p->ascent = std::round(asc);
	p->line = std::round(asc - desc + gap);
	// ... and each pack range baked at that height:
	ranges[r].font_size = px;
#endif
