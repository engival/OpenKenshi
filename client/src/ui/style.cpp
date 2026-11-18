#include "ui/style.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <memory>

#include "config.hpp"

namespace ui
{
	namespace
	{
		const cfg::Text kStyle(cfg::Group{ "Look" }, "ui.style", "flat", { "flat", "original" },
			"How the interface is drawn: flat (our own look, colours below) or original (the game's art). Takes effect at the next start.");

		// The flat style's palette: one option per colour, "#rrggbb" or "#rrggbbaa". Names shared with the game's kenshi_colours.xml mean the same thing.
		struct PaletteEntry { const char* name; const char* def; const char* help; };
		const PaletteEntry kPalette[] = {
			{ "Window", "#1d1d1d", "Window background" },
			{ "Header", "#292929", "Window header bars" },
			{ "Panel", "#1d1d1df0", "Panel background (HUD, dialogue)" },
			{ "Inset", "#151414", "Sunken areas: text boxes, menus, slots" },
			{ "Edge", "#050505", "The dark outer edge of windows, panels and buttons" },
			{ "Border", "#525252", "The hairline inside the edge" },
			{ "Button", "#2c2c2c", "Button face" },
			{ "HudMoney", "#9c9c9c", "HUD money box text (Dark UI's Kenshi_TextboxStandardText; shadowed while light)" },
			{ "DebugKey", "#e0c070", "Debug panel: key chords" },
			{ "DebugHeading", "#d9a441", "Debug panel: group headings (the Accent)" },
			{ "MapNew", "#00ff00", "Map: the frame around a newly discovered place (the original's NewDiscovered, tinted MyGUI's Green)" },
			{ "ButtonHover", "#363636", "Glyph button face under the mouse (pause, speed, close)" },
			{ "ButtonDown", "#161616", "Button face while held down" },
			{ "ButtonDownBorder", "#35302a", "Button hairline while held down" },
			{ "ButtonText", "#666159", "Button text (Dark UI's Kenshi_Button1Skin)" },
			{ "ButtonTextHover", "#816635", "Button text under the mouse" },
			{ "ButtonTextDown", "#3f362d", "Button text while held down" },
			{ "ButtonTextOn", "#b0ad98", "Button text when checked (the open tab)" },
			{ "Accent", "#d9a441", "Selection, focus, ticks and lit toggles" },
			{ "TextSelect", "#6b5426", "Selected text in a text box (behind the text)" },
			{ "Main", "#e8e4da", "Text" },
			{ "Secondary", "#cfcabf", "Text in sunken areas" },
			{ "Title", "#e9b85c", "Titles and highlighted rows" },
			{ "Special", "#f2d382", "Hovered items" },
			{ "Good", "#8fd18a", "Good news, friendly names" },
			{ "Bad", "#c0503c", "Bad values" },
			{ "BadBright", "#ee755f", "Warnings, the down and dead" },
			{ "ToggleOn", "#b6b4a6", "Combat toggles (BLOCK, JOBS): the light and text when on (Dark UI)" },
			{ "ToggleOff", "#666562", "Combat toggles: the light and text when off" },
			{ "TagHover", "#0c0d07", "World tags under the mouse (ALT held): fill" },
			{ "TagHoverBorder", "#8c8d6e", "World tags under the mouse: hairline" },
			{ "TagHoverText", "#c3c9a4", "World tags under the mouse: text" },
			{ "DropLabelSteal", "#ff6666", "World tags under the mouse when taking the item is theft: frame and text (the game's)" },
			{ "DialogueText", "#a8a8a8", "Dialogue: the NPC's line (original style: the parchment's ink)" },
			{ "DialogueLabel", "#a8a8a8", "Dialogue: names and money, on the panel" },
			{ "DialogueChoice", "#8c8c8c", "Dialogue: the player's replies" },
			{ "Banner", "#dfd8cf", "The zone banner's name (Kenshi_MainPanel BiomeTextBox)" },
			{ "TipText", "#8c8c8c", "Item tooltips: rows and description" },
			{ "TipTitle", "#cbbc9c", "Item tooltips: the item's name" },
			{ "TipGood", "#c3c9a4", "Item tooltips: bonuses and protections" },
			{ "TipBad", "#d0676d", "Item tooltips: penalties, Stolen" },
			{ "StatGood", "#a0ad6e", "Race stats: skill bonuses" },
			{ "StatBad", "#b8473a", "Race stats: skill penalties" },
			{ "XpBar", "#7f7553", "Stats window: progress to the next level" },
			{ "XpTrack", "#141414", "Stats window: the progress bar's track" },
			{ "StatGroup", "#a8735c", "Stats window: -Group- titles, the described stat" },
			{ "StatLabel", "#7a756c", "Stats window: Stats Affected / Ways To Train / Description labels" },
			{ "StatUp", "#a0ad6e", "Stats window: a level raised by modifiers" },
			{ "StatDown", "#b8473a", "Stats window: a level lowered by modifiers" },
			{ "SquadFull", "#b8473a", "Squads window: a squad's members/max at the max" },
			{ "SquadEmpty", "#5a5a5a59", "Squads window: an empty squad's portrait strip" },
			{ "ProspectLow", "#c46a4f", "Prospecting: a resource value under 50 (the original's #59231a, lifted for dark fields)" },
			{ "ProspectMid", "#d8cfbd", "Prospecting: a resource value of 50 to 75 (the original's near black #140806)" },
			{ "ProspectHigh", "#a8b774", "Prospecting: a resource value over 75" },
			{ "ProspectZone", "#c46a4f", "Prospecting: the overview's Zone line (the host's; the original's #492620, lifted)" },
			{ "ProspectEnv", "#afa68b", "Prospecting: the overview's Environment values (the host's)" },
			{ "MarkerPlayer", "#8dcdff", "Map: the player's places and characters (icon and label)" },
			{ "MarkerAlly", "#5cb473", "Map: an allied place" },
			{ "MarkerNeutral", "#dfdfdf", "Map: a neutral place" },
			{ "MarkerEnemy", "#e65139", "Map: an enemy place" },
			{ "MarkerMovement", "#79b44b", "Map: the move order's marker" },
			{ "ResearchName", "#d8c27a", "Research: the described entry's name and the picked type" },
			{ "ResearchBlocked", "#e2553f", "Research: a blueprint that cannot start (its button text)" },
			{ "ResearchMissing", "#e2553f", "Research: a missing cost or unmet requirement in the description" },
			{ "ResearchDim", "#8c8c8c", "Research: a met requirement" },
			{ "ResearchUnlock", "#c3cf8a", "Research: what it unlocks (New Building ...)" },
			{ "ResearchGood", "#c3cf8a", "Research: a cost in stock, a queued requirement in the description" },
			{ "QueueRemove", "#cc4444", "A queue row's X (Kenshi_ItemListItem's RemoveButton)" },
			{ "ProgressText", "#ded8ce", "A progress bar's text in the world (Kenshi_ProgressBarPanel's)" },
			{ "RelationName", "#ded8ce", "Faction: a relation's name (Kenshi_FactionRelationsLine's)" },
			{ "RelationBad", "#bf4834", "Faction: a relation's value below -30 (the game's)" },
			{ "RelationNeutral", "#afa68b", "Faction: a relation's value from -30 to 60 (the game's)" },
			{ "RelationGood", "#a8b774", "Faction: a relation's value above 60 (the game's)" },
			{ "BuildMaterial", "#afa68b", "Build window: a material line of the stats (the host's; the original's, never red)" },
			{ "BuildPowerOut", "#a8b774", "Build window: Power Output +N (the host's)" },
			{ "BuildPowerUse", "#bf4834", "Build window: Power Consumption (the host's)" },
			{ "BuildLabel", "#cfcabf", "Build window: a COMMANDS label (the host's markup; the original's #140806 on parchment)" },
			{ "BuildKey", "#d9a441", "Build window: a COMMANDS key (the host's markup; the original's #afa68b)" },
			{ "Figure", "#00000073", "Robotics window: the body figure's tint (a dark silhouette, as Dark UI's)" },
			{ "TipDim", "#606060", "Item tooltips: what the item lacks ([No Armour Coverage])" },
			{ "TipMarkup", "#c46a4f", "Trade tooltips: a price at or above the item's value, and its (+N%) (the original's #59231a, lifted for dark tooltips)" },
			{ "TipMarkdown", "#a8b774", "Trade tooltips: a price below the item's value" },
			{ "TipTrading", "#afa68b", "Trade tooltips: the price line while trading" },
			{ "TipMaker", "#afa68b", "Item tooltips: [Manufactured by ...] (the game's, kinspect tooltips.md)" },
			{ "NameTagUnselected", "#b3b3b3", "Name labels: an unselected character's (the game's theme NameTagUnselected)" },
			{ "NameTagSelected", "#ffffff", "Name labels: a selected character's [Name] (the game's)" },
			{ "NameTagShadow", "#484848", "Name labels: the text shadow (the layout's 0.2807 grey)" },
			{ "NameTagHidden", "#4085be", "Name labels: the stealth icon, hidden (the game's)" },
			{ "NameTagAlmostSeen", "#beb840", "Name labels: the stealth icon, almost seen (the game's)" },
			{ "NameTagSeen", "#be404a", "Name labels: the stealth icon, seen (the game's)" },
			{ "NameTagLight", "#f0cd57", "Name labels: the light-level icon (the layout's 0.94 0.81 0.34)" },
			{ "HudGood", "#a8b774", "Status panel: a raised stat (24 (+4)), good states" },
			{ "HudBad", "#c46a4f", "Status panel: a lowered stat, bad states (the original's #59231a, lifted for dark insets)" },
			{ "HudNeutral", "#afa68b", "Status panel: neutral states (Staying low), a local bounty" },
			{ "BuildingCrafting", "#afa68b", "Building window: a crafting bench's item in progress (the game's)" },
			{ "HudTag", "#b0745f", "Status panel: tags (Leader, Bounty:) (the original's #492620, lifted)" },
			{ "HudFaint", "#6a6a6a", "Status panel: another faction's bounty (the original's #444444, lifted)" },
			{ "Cell", "#3c3938", "Item grid cells" },
			{ "CellGap", "#1d1b1b", "Between item grid cells" },
			{ "Blocked", "#4b2619", "Item grid cells nothing may go in (a limb still attached)" },
			{ "BlockedGap", "#2b1207", "Between blocked cells" },
			{ "ItemBack", "#0000008c", "Behind an item in a grid (the cells it covers)" },
			{ "DropOk", "#5fae4580", "Item grids while dragging: the cells an item would take" },
			{ "DropSwap", "#d9a44180", "Item grids while dragging: dropping there swaps or stacks with the item under it" },
			{ "DropNo", "#c0503c80", "Item grids while dragging: it does not fit there" },
			{ "BarFill", "#004f00", "Health bars: health (the original's band colour; the body below it is 0.71 of it)" },
			{ "BarText", "#9c9c9c", "Health bars: the names and values" },
			{ "ProgressFill", "#004f00", "Progress bars: the filled part" },	// Dark UI's green, as BarFill (kenshi-h2v.16.8)
			{ "BarStun", "#8d929c", "Health bars: stunned (health lost until it wears off)" },
			{ "BarHurt", "#5a1c16", "Health bars: damage" },
			{ "BarHealed", "#987408", "Health bars: bandaged; Hunger's middle band" },
			{ "BarSplint", "#d8d2c4", "Health bars: splinted (jury-rigged)" },
			{ "BarRobot", "#3a7bb0", "Health bars: a robotic limb or a robot" },
			{ "BarCrushed", "#1a1512", "Health bars: a stump or a crushed limb (over the slat)" },
			{ "BarLayerGreen", "#7c7543", "HUD slats' layers, the game's skins (approximate: kinspect hud30_static (f), average pixels): Green (Condition, Power)" },
			{ "BarLayerYellow", "#ad9567", "HUD slats: Yellow (a generator's output and fuel, a battery's; approximate)" },
			{ "BarLayerRed", "#591c11", "HUD slats: Red (approximate)" },
			{ "BarLayerWhite", "#d7cbb4", "HUD slats: White (approximate)" },
			{ "BarLayerGrey", "#524440", "HUD slats: Grey (approximate)" },
			{ "BarLayerRobot", "#665f5a", "HUD slats: Robot (approximate)" },
			{ "BarLayerCrushed", "#33363d", "HUD slats: Crushed (approximate)" },
			{ "HudBarTrack", "#544743", "HUD building bars: the empty part (approximate: read off the genbuilding capture)" },
			{ "HudBarText", "#140806", "HUD building bars: the label and right text (the game's Secondary)" },
			{ "HudBarSlat", "#3b322c", "HUD building panel: the spare rows' empty slats (?)" },
			{ "HudBarEdge", "#15110e", "HUD building panel: the slats' edge (?)" },
			{ "ItemStandard", "#e0cccc", "Item names: ungraded items (the game's)" },
			{ "ItemGrade0", "#8c8c8c", "Item names: the lowest grade (the game's #666666, lifted for dark backgrounds)" },
			{ "ItemGrade1", "#d9ccbf", "Item names: grade 1" },
			{ "ItemGrade2", "#4099b3", "Item names: grade 2" },
			{ "ItemGrade3", "#66fc80", "Item names: grade 3" },
			{ "ItemGrade4", "#fceb0d", "Item names: grade 4" },
			{ "ItemArtifact", "#0dfceb", "Item names: the top grade (artifact / masterwork)" },
		};

		std::vector<std::unique_ptr<cfg::Text>>& paletteOptions()
		{
			static std::vector<std::unique_ptr<cfg::Text>> v = []
			{
				std::vector<std::unique_ptr<cfg::Text>> out;
				for (const PaletteEntry& e : kPalette) { out.push_back(std::make_unique<cfg::Text>(cfg::Group{ "Colours" }, (std::string("ui.colour.") + e.name).c_str(), e.def, e.help)); }
				return out;
			}();
			return v;
		}
		const bool kPaletteDeclared = !paletteOptions().empty();	// declared at start-up like any other option

		Color lighter(const Color& c, float k) { return { std::min(1.0f, c.r + k), std::min(1.0f, c.g + k), std::min(1.0f, c.b + k), c.a }; }
		Color times(const Color& a, const Color& b) { return { a.r * b.r, a.g * b.g, a.b * b.b, a.a * b.a }; }
		Color mix(const Color& a, const Color& b, float t) { return { a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t }; }

		// The Dark UI frame's widths, pixels: the near-black edge about 2.6 units, the grey line inside it 1.9 (the user's 1440p Dark UI shots:
		// 3-4 px of edge, 2-3 of line).
		float rimEdge(float s) { return std::max(1.0f, std::round(2.6f * s)); }
		float rimLine(float s) { return std::max(1.0f, std::round(1.9f * s)); }

		// r filled, its top-left corner cut at 45 degrees (cut: the legs, pixels; 0: square).
		void cutFill(const Rect& r, float cut, const Color& c)
		{
			if (cut <= 0) { gfx::fill(r, c); return; }
			cut = std::round(cut);
			gfx::fill({ r.x + cut, r.y, r.w - cut, cut }, c);
			gfx::fill({ r.x, r.y + cut, r.w, r.h - cut }, c);
			gfx::triangle(r.x, r.y + cut, r.x + cut, r.y, r.x + cut, r.y + cut, c);
		}

		// The Dark UI construction: the near-black edge, the grey line inside it, the fill. cut: the top-left corner cut at 45 degrees, pixels
		// (the speed panel and its pause button); each layer's cut is the outer one moved in by its inset (a leg shrinks 0.586 per pixel).
		void framed(const Rect& r, float s, const Color& edge, const Color& hair, const Color& fill, float cut = 0)
		{
			const float e = rimEdge(s), in = e + rimLine(s);
			cutFill(r, cut, edge);
			if (cut > 0)
			{
				cutFill(r.inset(e), cut - 0.586f * e, hair);
				cutFill(r.inset(in), cut - 0.586f * in, edge);	// under the fill, as the square frame has it
			}
			else { gfx::frame(r.inset(e), in - e, hair); }
			cutFill(r.inset(in), cut - 0.586f * in, fill);
		}

		// The close button's X: two strokes from triangles.
		void cross(const Rect& r, const Color& c, float b)
		{
			const float a = std::round(std::min(r.w, r.h) * 0.22f), cx = r.x + r.w / 2, cy = r.y + r.h / 2, t = 1.2f * b;
			for (int sgn : { 1, -1 })
			{
				const float x0 = cx - a, y0 = cy - sgn * a, x1 = cx + a, y1 = cy + sgn * a;
				const float nx = sgn * t * 0.7071f, ny = t * 0.7071f;	// perpendicular half-thickness
				gfx::triangle(x0 - nx, y0 + ny, x0 + nx, y0 - ny, x1 + nx, y1 - ny, c);
				gfx::triangle(x0 - nx, y0 + ny, x1 + nx, y1 - ny, x1 - nx, y1 + ny, c);
			}
		}

		// The reset glyph: an open ring (a stroke of quads from 50 to 320 degrees) with an arrowhead at its start, pointing anticlockwise.
		void resetArrow(const Rect& r, const Color& c, float b)
		{
			const float cx = r.x + r.w / 2, cy = r.y + r.h / 2, rad = std::min(r.w, r.h) * 0.28f, t = 1.1f * b;
			constexpr float kPi = 3.14159265f, a0 = 50 * kPi / 180, a1 = 320 * kPi / 180;
			const int n = 18;
			for (int i = 0; i < n; i++)
			{
				const float p = a0 + (a1 - a0) * i / n, q = a0 + (a1 - a0) * (i + 1) / n;
				const float ix0 = cx + (rad - t) * std::cos(p), iy0 = cy - (rad - t) * std::sin(p), ox0 = cx + (rad + t) * std::cos(p), oy0 = cy - (rad + t) * std::sin(p);
				const float ix1 = cx + (rad - t) * std::cos(q), iy1 = cy - (rad - t) * std::sin(q), ox1 = cx + (rad + t) * std::cos(q), oy1 = cy - (rad + t) * std::sin(q);
				gfx::triangle(ix0, iy0, ox0, oy0, ox1, oy1, c);
				gfx::triangle(ix0, iy0, ox1, oy1, ix1, iy1, c);
			}
			// The head at a0, along the tangent (anticlockwise: the direction of increasing angle, screen y up).
			const float hx = cx + rad * std::cos(a0), hy = cy - rad * std::sin(a0), h = rad * 0.75f;
			const float tx = -std::sin(a0), ty = -std::cos(a0);	// the tangent backwards (toward smaller angles), screen coordinates
			const float nx = std::cos(a0), ny = -std::sin(a0);	// outward
			gfx::triangle(hx + tx * h, hy + ty * h, hx + nx * h * 0.8f, hy + ny * h * 0.8f, hx - nx * h * 0.8f, hy - ny * h * 0.8f, c);
		}

		// The pin button's pin: a round head over a needle.
		void pin(const Rect& r, const Color& c, float b)
		{
			const float cx = std::round(r.x + r.w / 2), rad = std::round(std::min(r.w, r.h) * 0.14f), top = std::round(r.y + r.h * 0.26f), w = std::max(1.0f, std::round(1.2f * b));
			gfx::fill({ cx - rad, top + rad - w / 2, 2 * rad, rad }, c);
			gfx::fill({ cx - rad - w, top + 2 * rad - w / 2, 2 * (rad + w), w }, c);
			gfx::fill({ cx - std::round(w / 2), top + 2 * rad, w, std::round(r.h * 0.26f) }, c);
			gfx::fill({ cx - rad + w, top, 2 * (rad - w), rad }, c);
		}

		// The target toggle's mark: a ring round a dot (ours, kenshi-k16.20.13.1.1).
		void target(const Rect& r, const Color& c, float b)
		{
			const float cx = r.x + r.w / 2, cy = r.y + r.h / 2, rad = std::round(std::min(r.w, r.h) * 0.3f), w = std::max(1.0f, std::round(1.4f * b));
			const float dot = std::max(1.5f, std::round(rad * 0.35f));
			constexpr int kSides = 24;
			for (int i = 0; i < kSides; i++)
			{
				const float p = i * 6.2831853f / kSides, q = (i + 1) * 6.2831853f / kSides, cp = std::cos(p), sp = std::sin(p), cq = std::cos(q), sq = std::sin(q);
				gfx::triangle(cx + (rad - w) * cp, cy + (rad - w) * sp, cx + rad * cp, cy + rad * sp, cx + rad * cq, cy + rad * sq, c);
				gfx::triangle(cx + (rad - w) * cp, cy + (rad - w) * sp, cx + rad * cq, cy + rad * sq, cx + (rad - w) * cq, cy + (rad - w) * sq, c);
				gfx::triangle(cx, cy, cx + dot * cp, cy + dot * sp, cx + dot * cq, cy + dot * sq, c);
			}
		}

		Rect snap(float x, float y, float w, float h) { return { std::round(x), std::round(y), std::round(w), std::round(h) }; }

		class FlatStyle : public Style
		{
		public:
			void draw(Look l, const Rect& r, float s, State st, bool checked, const Color& tint) const override
			{
				const float b = std::max(1.0f, std::round(s));	// a border, pixels
				const bool hot = st == State::kHighlighted, down = st == State::kPushed, off = st == State::kDisabled;
				const Color k = off ? tint.alpha(0.45f) : tint;
				const Color hair = times(c("Border"), k), edge = times(c("Edge"), k), accent = times(c("Accent"), k);
				switch (l)
				{
					case Look::kNone:
						return;
					case Look::kWindow:
					case Look::kPanel:
					case Look::kHudPanel:
					case Look::kSpeedPanel:
					case Look::kBuildPanel:
					case Look::kMessage:
					case Look::kSpeech:
					case Look::kPanelThin:
						framed(r, s, edge, hair, times(c(l == Look::kWindow ? "Window" : "Panel"), k), l == Look::kSpeedPanel ? 15 * s : 0);
						return;
					case Look::kHeader:	// the window's header bar: a box of its own, lighter in its upper half
					{
						framed(r, s, edge, hair, times(c("Header"), k));
						const Rect in = r.inset(rimEdge(s) + rimLine(s));
						gfx::fill({ in.x, in.y, in.w, std::round(in.h / 2) }, Color{ 1, 1, 1, 0.025f });
						return;
					}
					case Look::kInset:
					case Look::kMenu:
					case Look::kTooltip:
						gfx::fill(r, times(c("Inset"), k).alpha(l == Look::kTooltip ? 0.96f : 1));
						gfx::frame(r, b, hair.alpha(0.8f));
						return;
					case Look::kSlot:	// selectable (inventory rows): hover lightens, checked = selected
						gfx::fill(r, hot || down ? lighter(times(c("Inset"), k), 0.05f) : times(c("Inset"), k));
						gfx::frame(r, b, checked ? accent : hot ? hair : hair.alpha(0.6f));
						return;
					case Look::kCell:	// a soft tile in a dark gap; red-brown where blocked
					{
						const char* face = off ? "Blocked" : "Cell";
						const Color f = times(c(face), tint), g = times(c(off ? "BlockedGap" : "CellGap"), tint);
						gfx::fill(r, g);
						gfx::fill(r.inset(b), mix(g, f, 0.45f));
						gfx::fill(r.inset(std::round(r.w * 0.12f)), mix(g, f, 0.8f));
						gfx::fill(r.inset(std::round(r.w * 0.22f)), f);
						return;
					}
					case Look::kField:
						gfx::fill(r, times(c("Button"), k));
						return;
					case Look::kSlotBox:	// no box: the machine window's columns sit on its body (kenshi-k16.19.8.1)
						return;
					case Look::kNamePlate:	// the plate from the skin's row 18 down to 12 units above the foot (Dark UI's gap over the panel)
					{
						const Rect p = snap(r.x, r.y + r.h * 0.28f, r.w, r.h * 0.72f - 12 * s);
						framed(p, s, edge, hair, times(c("Panel"), k));
						return;
					}
					case Look::kStatsPanel:
					case Look::kExtStatsPanel:
					{
						// Where the skins have them: the state box over the top 37 %, the value column from x 62.5 % to 96.7 % below it.
						framed(r, s, edge, hair, times(c("Panel"), k));
						const bool ext = l == Look::kExtStatsPanel;
						const float below = ext ? r.h * 0.38f : 0;
						if (ext)
						{
							const Rect box = snap(r.x + 4 * b, r.y + 4 * b, r.w - 8 * b, r.h * 0.37f - 4 * b);
							gfx::fill(box, times(c("Inset"), k));
							gfx::frame(box, b, hair.alpha(0.8f));
						}
						const Rect col = snap(r.x + r.w * 0.625f, r.y + below + 4 * b, r.w * 0.342f, r.h - below - 8 * b);
						gfx::fill(col, times(c("Inset"), k));
						gfx::frame(col, b, hair.alpha(0.8f));
						return;
					}
					case Look::kExtStatsButton:
					{
						framed(r, s, edge, hot || down ? lighter(hair, 0.12f) : hair, times(c(down ? "ButtonDown" : "Button"), k));
						const Color g = hot ? accent : times(c("Main"), k).alpha(0.8f);
						const float cx = r.x + r.w / 2, cy = r.y + r.h / 2, a = std::min(r.w, r.h) * 0.25f;
						if (!checked) { gfx::triangle(cx - a, cy + a * 0.6f, cx, cy - a * 0.6f, cx + a, cy + a * 0.6f, g); }
						else { gfx::triangle(cx - a, cy - a * 0.6f, cx + a, cy - a * 0.6f, cx, cy + a * 0.6f, g); }
						return;
					}
					case Look::kInput:
						gfx::fill(r, times(c("Inset"), k));
						gfx::frame(r, b, hot ? accent : hair);
						return;
					case Look::kButton:
					case Look::kToggle:
					case Look::kClose:
					case Look::kPin:
					case Look::kTarget:
					case Look::kPause:
					case Look::kSpeed1:
					case Look::kSpeed2:
					case Look::kSpeed3:
					case Look::kBuild:
					{
						// Dark UI: flat; held down darkens the face and warms the hairline. Text buttons answer the mouse in their text colour
						// only, and checked (the open tab) is brighter text, not a frame.
						const bool text = l == Look::kButton;
						const Color face = times(down ? c("ButtonDown") : hot && !text ? c("ButtonHover") : c("Button"), k);
						const Color line = down ? times(c("ButtonDownBorder"), k) : checked && !text && l != Look::kToggle ? accent
							: hot && !text ? lighter(hair, 0.12f) : hair;
						framed(r, s, edge, line, face, l == Look::kPause ? 10 * s : 0);
						if (l == Look::kToggle)	// Dark UI's light: a small dot near the left edge, pale when on
						{
							const float d = std::round(6 * s);
							disc(std::round(r.x + 8 * s) + d / 2, std::round(r.y + (r.h - d) / 2) + d / 2, d / 2, times(c(checked ? "ToggleOn" : "ToggleOff"), k));
						}
						else if (l == Look::kClose) { cross(r, times(c("Main"), k).alpha(hot ? 1 : 0.75f), b); }
						else if (l == Look::kPin) { pin(r, checked ? accent : times(c("Main"), k).alpha(hot ? 1 : 0.75f), b); }
						else if (l == Look::kTarget) { target(r, checked ? accent : times(c("Main"), k).alpha(hot ? 1 : 0.75f), b); }
						else if (l == Look::kBuild) { tools(r, times(c("Main"), k).alpha(hot ? 1 : 0.75f)); }
						else if (l != Look::kButton) { glyph(l, r, checked ? accent : times(c("Main"), k)); }
						return;
					}
					case Look::kReset:
					{
						if (hot || down) { gfx::fill(r, times(c("Main"), k).alpha(down ? 0.12f : 0.06f)); }
						resetArrow(r, hot || down ? accent : times(c("Main"), k).alpha(0.45f), b);
						return;
					}
					case Look::kTabTop:
					case Look::kTabSide:
					{
						const bool top = l == Look::kTabTop;
						if (checked || hot || down) { gfx::fill(r, times(c("Main"), k).alpha(checked ? 0.07f : 0.04f)); }
						const float t = std::round((checked ? 2 : 1) * s);
						const Color mark = checked ? accent : hair;
						if (checked || hot) { gfx::fill(top ? Rect{ r.x, r.bottom() - t, r.w, t } : Rect{ r.x, r.y, std::round(3 * s), r.h }, mark); }
						return;
					}
					case Look::kTick:
					{
						framed(r, s, edge, hot ? accent : hair, times(c("Inset"), k));
						if (checked) { gfx::fill(r.inset(std::round(r.w * 0.28f)), accent); }
						return;
					}
					case Look::kPortrait:
						framed(r, s, edge, checked ? accent : hot ? lighter(hair, 0.15f) : hair, times(c("Inset"), k));
						if (checked) { gfx::frame(r.inset(rimEdge(s) + rimLine(s)), b, accent); }
						return;
					case Look::kProgressTrack:
						gfx::fill(r, times(c("Inset"), k));
						return;
					case Look::kProgressFill:	// two tones as the medical bars (the original's skins: kenshi-h2v.16.8)
					{
						const Color f = times(c("ProgressFill"), k);
						const float band = std::round(r.h * 0.4f);
						gfx::fill({ r.x, r.y, r.w, band }, f);
						gfx::fill({ r.x, r.y + band, r.w, r.h - band }, Color{ f.r * 0.71f, f.g * 0.71f, f.b * 0.71f, f.a });
						return;
					}
					case Look::kProgressFrame:
						gfx::frame(r, b, hair);
						return;
					case Look::kBarFill:
					case Look::kBarStun:
					case Look::kBarHurt:
					case Look::kBarHealed:
					case Look::kBarSplint:
					case Look::kBarRobot:
					{
						const char* name = l == Look::kBarFill ? "BarFill" : l == Look::kBarStun ? "BarStun" : l == Look::kBarHurt ? "BarHurt"
							: l == Look::kBarHealed ? "BarHealed" : l == Look::kBarSplint ? "BarSplint" : "BarRobot";
						// The original's two tones: the colour as a band over the top 40 %, the body below at 0.71 of it.
						const Color f = times(c(name), k);
						const float band = std::round(r.h * 0.4f);
						gfx::fill({ r.x, r.y, r.w, band }, f);
						gfx::fill({ r.x, r.y + band, r.w, r.h - band }, Color{ f.r * 0.71f, f.g * 0.71f, f.b * 0.71f, f.a });
						return;
					}
					case Look::kBarCrushed:
						gfx::fill(r, times(c("BarCrushed"), k).alpha(0.7f));
						return;
					case Look::kTrack:
					{
						const float h = std::max(2 * b, std::round(r.h * 0.22f));
						gfx::fill(snap(r.x, r.y + (r.h - h) / 2, r.w, h), times(c("Inset"), k));
						gfx::frame(snap(r.x, r.y + (r.h - h) / 2, r.w, h), b, hair);
						return;
					}
					case Look::kThumb:
					{
						const Rect t = snap(r.x, r.y + r.h * 0.15f, r.w, r.h * 0.7f);
						gfx::fill(t, hot || down ? accent : times(c("ButtonHover"), k));
						gfx::frame(t, b, edge);
						return;
					}
					case Look::kScrollTrack:	// a hairline groove down the middle
					{
						if (r.w > r.h)	// horizontal
						{
							const float h = std::max(2 * b, std::round(r.h * 0.3f));
							gfx::fill(snap(r.x, r.y + (r.h - h) / 2, r.w, h), times(c("Inset"), k));
							return;
						}
						const float w = std::max(2 * b, std::round(r.w * 0.3f));
						gfx::fill(snap(r.x + (r.w - w) / 2, r.y, w, r.h), times(c("Inset"), k));
						return;
					}
					case Look::kScrollThumb:
					{
						const Rect t = r.w > r.h ? snap(r.x, r.y + r.h * 0.15f, r.w, r.h * 0.7f) : snap(r.x + r.w * 0.15f, r.y, r.w * 0.7f, r.h);
						gfx::fill(t, hot || down ? accent : times(c("ButtonHover"), k));
						gfx::frame(t, b, edge);
						return;
					}
					case Look::kScrollUp:
					case Look::kScrollDown:
					{
						const Color g = hot || down ? accent : times(c("Main"), k).alpha(0.8f);
						const float cx = r.x + r.w / 2, cy = r.y + r.h / 2, a = std::min(r.w, r.h) * 0.3f;
						if (l == Look::kScrollUp) { gfx::triangle(cx - a, cy + a * 0.6f, cx, cy - a * 0.6f, cx + a, cy + a * 0.6f, g); }
						else { gfx::triangle(cx - a, cy - a * 0.6f, cx + a, cy - a * 0.6f, cx, cy + a * 0.6f, g); }
						return;
					}
					case Look::kScrollLeft:
					case Look::kScrollRight:
					{
						const Color g = hot || down ? accent : times(c("Main"), k).alpha(0.8f);
						const float cx = r.x + r.w / 2, cy = r.y + r.h / 2, a = std::min(r.w, r.h) * 0.3f;
						if (l == Look::kScrollLeft) { gfx::triangle(cx + a * 0.6f, cy - a, cx + a * 0.6f, cy + a, cx - a * 0.6f, cy, g); }
						else { gfx::triangle(cx - a * 0.6f, cy - a, cx + a * 0.6f, cy, cx - a * 0.6f, cy + a, g); }
						return;
					}
					case Look::kArrowLeft:
					case Look::kArrowRight:
					{
						const Color g = hot || down ? accent : times(c("Main"), k).alpha(0.8f);
						const float cx = r.x + r.w / 2, cy = r.y + r.h / 2, a = std::min(r.w, r.h) * 0.22f;
						if (l == Look::kArrowLeft) { gfx::triangle(cx + a * 0.7f, cy - a, cx + a * 0.7f, cy + a, cx - a * 0.7f, cy, g); }
						else { gfx::triangle(cx - a * 0.7f, cy - a, cx + a * 0.7f, cy, cx - a * 0.7f, cy + a, g); }
						return;
					}
					case Look::kTag:	// hovered (what a click would act on, ALT held): Dark UI's olive box, as in darkui_ground_item_alt_hover
						if (hot) { framed(r, s, edge, times(c("TagHoverBorder"), k), times(c("TagHover"), k)); return; }
						gfx::fill(r, times(c("Inset"), k).alpha(0.78f));
						if (checked) { gfx::frame(r, b, times(c("Good"), k).alpha(0.8f)); }
						return;
					case Look::kRuleTop:	// a hairline centred in r, fading out over its outer fifths
					case Look::kRuleBottom:
					{
						const Color ink = times(c("Main"), k);
						const float h = std::max(1.0f, std::round(2 * s)), y = std::round(r.y + (r.h - h) / 2), fade = std::round(r.w / 5);
						gfx::gradientH({ r.x, y, fade, h }, ink.alpha(0), ink);
						gfx::fill({ r.x + fade, y, r.w - 2 * fade, h }, ink);
						gfx::gradientH({ r.right() - fade, y, fade, h }, ink, ink.alpha(0));
						return;
					}
				}
			}

			Size natural(Look l) const override
			{
				switch (l)
				{
					case Look::kButton: return { 0, 36 };
					case Look::kSlotBar: return { 0, 6 };
					case Look::kToggle: return { 0, 32 };
					case Look::kTick: return { 22, 22 };
					case Look::kPause: case Look::kSpeed1: case Look::kSpeed2: case Look::kSpeed3: return { 40, 32 };
					default: return {};
				}
			}

			Rect textArea(Look l, const Rect& px, float s, State, bool) const override
			{
				if (l == Look::kToggle) { const float lw = std::round(16 * s); return { px.x + lw, px.y, px.w - 2 * lw, px.h }; }	// centred on the button, clear of the light
				if (l == Look::kTabSide) { const float in = std::round(14 * s); return { px.x + in, px.y, px.w - 2 * in, px.h }; }	// clear of the bar
				return px;
			}

			Color textColour(Look l, State st, bool checked) const override
			{
				if (l == Look::kButton || l == Look::kTabTop || l == Look::kTabSide)
				{
					const Color t = c(st == State::kPushed ? "ButtonTextDown" : st == State::kHighlighted ? "ButtonTextHover" : checked ? "ButtonTextOn" : "ButtonText");
					return st == State::kDisabled ? t.alpha(0.45f) : t;
				}
				if (l == Look::kToggle)
				{
					const Color t = c(st == State::kPushed ? "ButtonTextDown" : checked ? "ToggleOn" : st == State::kHighlighted ? "ButtonTextHover" : "ToggleOff");
					return st == State::kDisabled ? t.alpha(0.45f) : t;
				}
				Color t = c(l == Look::kInset || l == Look::kMenu || l == Look::kInput || l == Look::kSlot || l == Look::kNamePlate ? "Secondary" : "Main");
				if (st == State::kHighlighted || st == State::kPushed) { t = lighter(t, 0.1f); }
				return st == State::kDisabled ? t.alpha(0.45f) : t;
			}

			Color colour(const std::string& name, const Color& def) const override
			{
				refresh();
				auto it = palette_.find(name);
				if (it != palette_.end()) { return it->second; }
				return res::fileColour(name, def);	// names the palette does not cover, from the game's file
			}

		private:
			mutable std::map<std::string, Color> palette_;
			mutable uint64_t gen_ = 0;

			Color c(const char* name) const { return colour(name, {}); }

			void refresh() const
			{
				if (gen_ == cfg::generation()) { return; }
				gen_ = cfg::generation();
				palette_.clear();
				for (size_t i = 0; i < std::size(kPalette); i++)
				{
					Color col;
					const std::string v = paletteOptions()[i]->get();
					if (Color::parse(v, col) || Color::parse(kPalette[i].def, col)) { palette_[kPalette[i].name] = col; }
				}
			}

			static void disc(float cx, float cy, float rad, const Color& col)
			{
				constexpr int kSides = 16;
				for (int i = 0; i < kSides; i++)
				{
					const float a0 = i * 6.2831853f / kSides, a1 = (i + 1) * 6.2831853f / kSides;
					gfx::triangle(cx, cy, cx + rad * std::cos(a0), cy + rad * std::sin(a0), cx + rad * std::cos(a1), cy + rad * std::sin(a1), col);
				}
			}

			// Pause: two bars; speed n: n triangles.
			// The build button's picture (our drawing): a hammer and a pick, their handles crossed, the heads up.
			static void tools(const Rect& r, const Color& g)
			{
				const float u = std::min(r.w, r.h) * 0.62f, ox = r.x + (r.w - u) / 2, oy = r.y + (r.h - u) / 2;
				auto line = [&](float x0, float y0, float x1, float y1, float t)
				{
					const float ax = ox + x0 * u, ay = oy + y0 * u, bx = ox + x1 * u, by = oy + y1 * u, dx = bx - ax, dy = by - ay, len = std::sqrt(dx * dx + dy * dy);
					const float nx = -dy / len * t * u / 2, ny = dx / len * t * u / 2;
					gfx::triangle(ax + nx, ay + ny, bx + nx, by + ny, bx - nx, by - ny, g);
					gfx::triangle(ax + nx, ay + ny, bx - nx, by - ny, ax - nx, ay - ny, g);
				};
				line(0.08f, 0.95f, 0.78f, 0.18f, 0.11f);	// the hammer's handle, up to the right
				line(0.6f, 0.02f, 0.98f, 0.36f, 0.2f);	// its head across the handle's end
				line(0.92f, 0.95f, 0.24f, 0.2f, 0.11f);	// the pick's handle, up to the left
				gfx::triangle(ox + 0.02f * u, oy + 0.42f * u, ox + 0.18f * u, oy + 0.06f * u, ox + 0.3f * u, oy + 0.16f * u, g);	// its blades
				gfx::triangle(ox + 0.18f * u, oy + 0.06f * u, ox + 0.56f * u, oy + 0.0f * u, ox + 0.3f * u, oy + 0.16f * u, g);
			}

			static void glyph(Look l, const Rect& r, const Color& g)
			{
				const float h = std::round(r.h * 0.42f), cy = r.y + r.h / 2;
				if (l == Look::kPause)
				{
					const float w = std::max(2.0f, std::round(h * 0.28f)), gap = std::round(h * 0.22f), x = std::round(r.x + (r.w - 2 * w - gap) / 2);
					gfx::fill({ x, std::round(cy - h / 2), w, h }, g);
					gfx::fill({ x + w + gap, std::round(cy - h / 2), w, h }, g);
					return;
				}
				const int n = l == Look::kSpeed1 ? 1 : l == Look::kSpeed2 ? 2 : 3;
				const float tw = h * 0.6f, total = n * tw;
				float x = r.x + (r.w - total) / 2;
				for (int i = 0; i < n; i++, x += tw) { gfx::triangle(x, cy - h / 2, x + tw, cy, x, cy + h / 2, g); }
			}
		};

		// The game's skins for each role (gui/skins). Roles without one fall back to the flat drawing.
		class SkinStyle : public Style
		{
		public:
			void draw(Look l, const Rect& r, float s, State st, bool checked, const Color& tint) const override
			{
				if (l == Look::kCell)	// one 25 x 25 tile of the slot skin per cell, whatever the cell size
				{
					if (const Skin* k = skin(l)) { k->draw(r, r.w / std::max(1.0f, k->w), st, checked, tint); return; }
				}
				if (l == Look::kScrollTrack) { return; }	// Kenshi_ScrollBarV's track is bare (Kenshi_TextBoxEmptySkin)
				if (const Skin* k = skin(l)) { k->draw(r, s, st, checked, l == Look::kTrack || l == Look::kThumb || l == Look::kScrollThumb || l == Look::kScrollUp || l == Look::kScrollDown || l == Look::kScrollLeft || l == Look::kScrollRight ? (st == State::kDisabled ? tint.alpha(0.45f) : tint) : tint); }	// the slider skins have no disabled look
				else { flat_.draw(l, r, s, st, checked, tint); }
			}

			Size natural(Look l) const override
			{
				const Skin* k = skin(l);
				if (l == Look::kButton || l == Look::kTick || l == Look::kToggle || l == Look::kPause || l == Look::kSpeed1 || l == Look::kSpeed2 || l == Look::kSpeed3)
				{
					if (k) { return { l == Look::kButton || l == Look::kToggle ? 0 : k->w, k->h }; }
				}
				if (l == Look::kSlotBar) { return { 0, 24 }; }	// the layout's 112 x 16 at 720 lines
				return {};
			}

			Rect textArea(Look l, const Rect& px, float s, State st, bool checked) const override
			{
				Rect a = px;
				Color c;
				if (const Skin* k = skin(l)) { k->text(px, s, st == State::kDisabled ? State::kNormal : st, checked, a, c); }
				if (l == Look::kTabSide) { const float in = std::round(10 * s); a = { a.x + in, a.y, a.w - 2 * in, a.h }; }	// the text clear of the skin's edge
				return a;
			}

			Color textColour(Look l, State st, bool checked) const override
			{
				Color c = res::fileColour(l == Look::kInset || l == Look::kMenu || l == Look::kField || l == Look::kMessage || l == Look::kNamePlate ? "Secondary" : "Main");
				Rect a;
				// The skins make disabled text black, which reads stronger than enabled text; we dim the normal colour instead.
				if (const Skin* k = skin(l)) { k->text({ 0, 0, 100, 100 }, 1, st == State::kDisabled ? State::kNormal : st, checked, a, c); }
				return st == State::kDisabled ? c.alpha(0.45f) : c;
			}

			Color colour(const std::string& name, const Color& def) const override
			{
				// Text on the game's parchment insets keeps its ink; other names the game's file lacks (TipGood) come from our palette.
				if (name == "DialogueText" || name == "DialogueChoice") { return res::fileColour("Secondary", def); }
				// The stats window's parchment: the original's red-brown titles and muted labels (kenshi-inspect stats_window_katanas); our modifier
				// colours darkened to read on it.
				static const std::pair<const char*, const char*> kParchment[] = { { "StatGroup", "#492620" }, { "StatLabel", "#5a4f45" },
					{ "StatUp", "#3d5a1c" }, { "StatDown", "#8c2a1c" }, { "SquadFull", "#59231a" },
					{ "TipMarkup", "#59231a" }, { "HudBad", "#59231a" }, { "HudTag", "#492620" }, { "HudFaint", "#444444" },
					{ "ProspectLow", "#59231a" }, { "ProspectMid", "#140806" }, { "ProspectHigh", "#a8b774" }, { "ProspectZone", "#492620" }, { "ProspectEnv", "#afa68b" }, { "BuildingCrafting", "#492620" }, { "HudBuildingName", "#492620" }, { "HudMoney", "#140806" },
					{ "BuildPicked", "#bdb3a8" }, { "BuildLabel", "#140806" }, { "BuildKey", "#afa68b" },	// Build*: kinspect build.md
					{ "ResearchName", "#492620" }, { "ResearchMissing", "#59231a" }, { "ResearchDim", "#444444" }, { "ResearchUnlock", "#a8b774" }, { "ResearchGood", "#a8b774" },
					{ "ResearchBlocked", "#59231a" }, { "QueueText", "#b7a7a7" } };	// Research*: the exe's Title / Bad / Greyed / Good (kenshi-inspect research25_static); QueueText: Kenshi_ItemListItem's
					// SquadFull, TipMarkup, Hud*: the original's ink (kenshi-inspect squads13, prices15, hud_panel16)
				for (const auto& [n, hex] : kParchment)
				{
					Color c = def;
					if (name == n && Color::parse(hex, c)) { return res::fileColour(name, c); }
				}
				return res::fileColour(name, flat_.colour(name, def));
			}

		private:
			FlatStyle flat_;

			static const Skin* skin(Look l)
			{
				static const std::map<Look, const char*> kSkins = {
					{ Look::kWindow, "Kenshi_GenericWindowSkin" }, { Look::kPanel, "Kenshi_FloatingPanelSkin" }, { Look::kHudPanel, "Kenshi_TimeMoneyPanelSkin" }, { Look::kSpeedPanel, "Kenshi_TimeMoneyPanelSkin" }, { Look::kBuildPanel, "Kenshi_BuildPanelSkin" }, { Look::kMessage, "Kenshi_MessageSkin" }, { Look::kNamePlate, "Kenshi_NamePanelSkin" },
					{ Look::kStatsPanel, "Kenshi_StatsPanelSkin" }, { Look::kExtStatsPanel, "Kenshi_ExtendedStatsPanelSkin" }, { Look::kExtStatsButton, "Kenshi_ExtendedStatsButton" },
					{ Look::kInset, "Kenshi_GenericTextBoxSkin" }, { Look::kMenu, "Kenshi_GenericTextBoxSkin" }, { Look::kField, "Kenshi_GenericTextBoxFlatSkin" }, { Look::kSlotBox, "Kenshi_GenericTextBoxFlatSkin" },
					{ Look::kInput, "Kenshi_GenericInputBoxSkin" }, { Look::kButton, "Kenshi_Button1Skin" }, { Look::kTabTop, "Kenshi_Button1Skin" }, { Look::kTabSide, "Kenshi_Button1Skin" }, { Look::kToggle, "Kenshi_TickButton1Skin" },
					{ Look::kTick, "Kenshi_TickBoxSkin" }, { Look::kSlot, "Kenshi_InventorySlotSkin" }, { Look::kCell, "Kenshi_InventorySlotSkin" }, { Look::kHeader, "Kenshi_GenericWindowHeaderSkin" }, { Look::kClose, "Kenshi_CloseButtonSkin" }, { Look::kPin, "Kenshi_PinButtonSkin" }, { Look::kPortrait, "Kenshi_PortraitFrameSkin" },
					{ Look::kBarFill, "Kenshi_LifeBarSkin" }, { Look::kBarStun, "Kenshi_LifeBarStunnedSkin" }, { Look::kBarHurt, "Kenshi_LifeBarHurtSkin" },
					{ Look::kBarHealed, "Kenshi_LifeBarHealedSkin" }, { Look::kBarSplint, "Kenshi_LifeBarSplintSkin" }, { Look::kBarRobot, "Kenshi_LifeBarRobotSkin" },
					{ Look::kBarCrushed, "Kenshi_LifeBarCrushedSkin" },
					{ Look::kTrack, "Kenshi_SliderSkin" }, { Look::kThumb, "Kenshi_SliderButtonSkin" }, { Look::kArrowLeft, "Kenshi_SpeedArrowLeftSkin" }, { Look::kScrollThumb, "Kenshi_SmallBarSkin" },
					{ Look::kScrollUp, "Kenshi_SmallArrowUpSkin" }, { Look::kScrollDown, "Kenshi_SmallArrowDownSkin" },
					{ Look::kScrollLeft, "Kenshi_SmallArrowLeftSkin" }, { Look::kScrollRight, "Kenshi_SmallArrowRightSkin" },
					{ Look::kArrowRight, "Kenshi_SpeedArrowRightSkin" }, { Look::kSpeech, "Kenshi_SpeechPanelSkin" }, { Look::kPanelThin, "Kenshi_FloatingPanelThinSkin" },
					{ Look::kProgressTrack, "Kenshi_ProgressBarEmptySkin" }, { Look::kProgressFill, "Kenshi_ProgressBarFilledSkin" }, { Look::kProgressFrame, "Kenshi_ProgressBarSkin" }, { Look::kPause, "Kenshi_TimeSpeedButton1Skin" },
					{ Look::kSpeed1, "Kenshi_TimeSpeedButton2Skin" }, { Look::kSpeed2, "Kenshi_TimeSpeedButton3Skin" }, { Look::kSpeed3, "Kenshi_TimeSpeedButton4Skin" }, { Look::kBuild, "Kenshi_BuildButtonSkin" },
					{ Look::kRuleTop, "Kenshi_WhiteLine" }, { Look::kRuleBottom, "Kenshi_WhiteLine2" },
				};
				auto it = kSkins.find(l);
				return it == kSkins.end() ? nullptr : res::skin(it->second);
			}
		};

		std::unique_ptr<Style> active_;
		FlatStyle fallback_;	// before init and after shutdown
	}

	const Style& style() { return active_ ? *active_ : fallback_; }

	namespace styles
	{
		void init()
		{
			if (kStyle.get() == "original") { active_ = std::make_unique<SkinStyle>(); }
			else { active_ = std::make_unique<FlatStyle>(); }
		}

		void shutdown() { active_.reset(); }
	}

	void drawTag(const Rect& px, std::string_view text, TagState s, const Font* font, Color ink)
	{
		const Font* f = font ? font : fonts::get("Kenshi_StandardFont_Medium");
		const bool hot = s == TagState::kHover || s == TagState::kStealing;
		const Color steal = style().colour("DropLabelSteal", { 1, 0.4f, 0.4f, 1 });
		style().draw(Look::kTag, px, px.h / 30, hot ? State::kHighlighted : State::kNormal, s == TagState::kFriendly);
		if (s == TagState::kStealing || s == TagState::kOwned) { gfx::frame(px, std::max(1.0f, std::round(px.h / (s == TagState::kOwned ? 15 : 30))), steal); }	// owned: twice as thick, no hover frame inside it
		if (!f || text.empty()) { return; }
		const Color c = s == TagState::kStealing ? steal : s == TagState::kHover ? style().colour("TagHoverText", {}) : ink.a > 0 ? ink : style().colour(s == TagState::kFriendly ? "Good" : "Main", {});
		f->draw(std::round(px.x + (px.w - f->width(text)) / 2), std::round(px.y + (px.h - f->lineHeight()) / 2), text, c);
	}
}
