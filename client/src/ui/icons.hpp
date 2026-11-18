#pragma once
#include <functional>
#include <string>

#include "ui/widget.hpp"

// Pictures that are not in the atlas (docs/UI.md "Item grids"): item icons and other loose PNGs, each its own texture, loaded on first use and
// kept until shutdown. A path is relative to the game's data folder ("icons/1012-gamedata.base.686-gamedata.base.png",
// "items/icons/default_icon.png"); an absolute path is used as it is (res::file("gfx/figure.png") for themed gui art).
namespace ui
{
	namespace icons
	{
		struct Picture
		{
			dl::TexId tex = 0;	// recorded as an upload (gfx::uploads), drawn trilinear; 0: none
			int w = 0, h = 0;	// texels
			explicit operator bool() const { return tex != 0; }
		};

		// Turns a data-relative path into a file; the default is res::dataFile. The host replaces it to look in mod folders too.
		void setResolver(std::function<std::string(const std::string&)> resolve);
		// The picture; an empty one (logged once) when the file is missing or unreadable, and for an empty path.
		const Picture& get(const std::string& path);
		// The game's icon cache: "icons/<item>.<material>.png" from the record string ids (ITEM / ARMOUR / WEAPON ... and its material); some
		// names carry a third id ("...<material>.54583-rebirth.mod.png", meaning not yet read). The game writes these itself for items with
		// "auto icon image"; the others name their PNG in the record's "icon" field (recordPath).
		std::string cached(const std::string& item, const std::string& material, const std::string& variant = {});
		// A record's file field as an icons:: path: ".\data\items\icons\LeatherShirt.png" -> "items/icons/LeatherShirt.png".
		std::string recordPath(const std::string& field);
		// Draws p over r (keepAspect: fitted and centred); flipH mirrors it left to right.
		void draw(const Picture& p, const Rect& r, const Color& c = {}, bool flipH = false, bool keepAspect = true);
		void shutdown();	// records every picture's destroy
	}

	// A loose picture as a widget: the robotics figure, a portrait.
	class Picture : public Widget
	{
	public:
		explicit Picture(std::string path = {}) : path(std::move(path)) {}
		std::string path;	// icons:: path
		Color tint;
		bool flipH = false, keepAspect = true;

	protected:
		void paint() const override;
	};
}
