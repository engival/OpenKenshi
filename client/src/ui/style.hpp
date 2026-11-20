#pragma once
#include <string>
#include <string_view>

#include "ui/font.hpp"
#include "ui/gfx.hpp"
#include "ui/skin.hpp"

// How widgets look (docs/UI.md "Styles"). Widgets and screens name a role (Look); the active Style draws it. Layouts are ours and never depend on
// a style's art, so a style only supplies stretchable pieces. Styles: "flat" (the default: our own drawing, colours from ui.colour.* options) and
// "original" (the game's MyGUI skins from the install). Chosen by the ui.style option at ui::init; a change takes effect at the next start.
namespace ui
{
	enum class Look
	{
		kNone,
		kWindow,	// a screen-level window (CaptionWindow: its body)
		kHeader,	// a CaptionWindow's header bar
		kClose,		// a window's close button
		kPin,		// a window's pin button; checked = pinned
		kTarget,	// an item window's quick right-click target toggle (ours, no skin in either style): a ringed dot; checked = marked
		kPanel,		// a floating panel (dialogue, HUD groups)
		kHudPanel,	// the small HUD boxes (time / speed)
		kSpeedPanel,	// the HUD speed buttons' box: kHudPanel with its top-left corner cut (the diamonds sit by it)
		kBuildPanel,	// the HUD build button's box (BuildPanel)
		kMessage,	// the HUD message box (Kenshi_MessageSkin)
		kNamePlate,	// the HUD status panel's name plate (Kenshi_NamePanelSkin: the plate in its lower two thirds, posts above)
		kStatsPanel, kExtStatsPanel,	// the HUD status panel: stats only / the state box above them (labels on the left, a value column on the right)
		kExtStatsButton,	// the status panel's extend button; checked = collapsed (the arrow points down)
		kInset,		// a sunken text area (dialogue text, menus)
		kField,		// a flat label box (names, money)
		kInput,		// an edit box (highlighted = focused)
		kButton,
		kReset,		// a small flat reset button: a circular arrow, dim until hovered (an option back to its default)
		kTabTop, kTabSide,	// a Tabs strip's tab (an accent underline when checked) / a side list's row (an accent bar on its left)
		kToggle,	// a button with a light (combat toggles); checked = on
		kTick,		// a tick box's box
		kSlot,		// an equipment slot
		kCell,		// one cell of an item grid; disabled = nothing may go there (the original's red cells)
		kPortrait,	// a portrait frame; checked = selected
		kBarFill, kBarStun, kBarHurt, kBarHealed, kBarSplint, kBarRobot, kBarCrushed,	// health bar layers (the medical slat's)
		kTrack, kThumb, kArrowLeft, kArrowRight,	// slider parts
		kScrollTrack, kScrollThumb, kScrollUp, kScrollDown, kScrollLeft, kScrollRight,	// a scrollbar's parts (track and thumb across the short side)
		kSpeech,	// a speech bubble
		kPanelThin,	// a thin floating panel (Kenshi_FloatingPanelThinSkin: the world's progress bars)
		kProgressTrack, kProgressFill, kProgressFrame,	// Kenshi_ProgressBarFill's parts: the empty track, the fill, the frame drawn over both
		kMenu,		// a popup menu
		kTooltip,
		kTag,		// a small label in the world (item and character names); checked = friendly
		kRuleTop, kRuleBottom,	// the brushed lines above and below the zone banner (Kenshi_WhiteLine / WhiteLine2)
		kPause, kSpeed1, kSpeed2, kSpeed3,	// time buttons; checked = the current speed
		kBuild,		// the HUD build button: a hammer and a pick
		kSlotBox,	// a machine window's slot / crafting / furnace panel: flat draws nothing (the column sits on the window), original as kField
		kSlotBar,	// a machine window's progress bar: only its natural height differs (flat 6, frameless; original 24, the skinned bar)
		kMapFrame,	// the minimap's frame: the panel's border with no body (flat: edge, hairline, a thin band of panel grey; original: the floating panel
				// skin); textArea gives the inside, where the map goes
	};

	class Style
	{
	public:
		virtual ~Style() = default;
		// The role's look in px (framebuffer pixels); scale = pixels per unit.
		virtual void draw(Look l, const Rect& px, float scale, State s, bool checked, const Color& tint = {}) const = 0;
		// The role's own size in units; 0 on a side = no preference.
		virtual Size natural(Look l) const = 0;
		// Where text goes on the role and in what colour.
		virtual Rect textArea(Look l, const Rect& px, float scale, State s, bool checked) const { (void)l; (void)scale; (void)s; (void)checked; return px; }
		virtual Color textColour(Look l, State s, bool checked) const = 0;
		// Named colours ("Main", "Secondary", "Title", "Good", "Bad", "BadBright", "Special", ...).
		virtual Color colour(const std::string& name, const Color& def) const = 0;
	};

	const Style& style();

	namespace styles
	{
		void init();	// picks the style from ui.style (after res::load)
		void shutdown();
	}

	// A world tag (host overlay: item labels, name tags) in the active style, text centred. Between gfx::begin and gfx::end.
	// kStealing: hovered, but taking it is theft (the game's DropItemLabel*SelectedStealing pair: frame and text DropLabelSteal).
	// ink: the text's colour while not hovered (an item label's ItemStandard / ItemGrade0..4 / ItemArtifact); alpha 0: the state's own.
	// kStealing: hovered and taking it is theft (red frame, red text); kOwned: someone else's, not hovered (the red frame on the normal
	// background, the ink as passed: the original with ALT, user 2026-11-05).
	enum class TagState { kNormal, kHover, kFriendly, kStealing, kOwned };
	void drawTag(const Rect& px, std::string_view text, TagState s, const Font* font = nullptr, Color ink = {});
}
