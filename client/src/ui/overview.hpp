#pragma once
#include <functional>
#include <string>
#include <vector>

#include "ui/widget.hpp"

// The overview window (docs/UI.md), after the original's Kenshi_OverviewWindow (Kenshi_WindowCX, 0.451 x 0.735 of the screen: 866 x 794 at
// 1080 lines; TabsMain with 140-unit tabs): MAP / FACTION / RESEARCH / CRAFTING / SQUADS / DIALOGUE, and AI (not among the layout's six;
// kinspect saw it live, so the seven share the header). The game's window keys open it on
// their page (toggle(page): the same key again closes it). MAP: the host draws the map image (MapActions::draw) for the view the UI keeps
// (pan, zoom, O / - / +), and the UI draws the host's markers over it (MapState), after kinspect's live + static (findings/ui_captures/map.md,
// findings/map24_static.md); a right-click is a move order. SQUADS is ui::Squads' page; the others wait for the sim systems behind them.
namespace ui
{
	enum class OverviewPage { kMap, kFaction, kResearch, kCrafting, kSquads, kDialogue, kAi, kCount };

	struct OverviewActions
	{
		std::function<void()> close;	// the close button or Escape (the window hides itself first)
		std::function<void(OverviewPage)> page;	// a page shown (a tab, open, toggle)
	};

	// The view the host draws: the rect (pixels) with world (cx, cz) at its centre, pxPerUnit pixels a world unit, north (-z) up.
	struct MapFrame
	{
		float x = 0, y = 0, w = 0, h = 0;
		double cx = 0, cz = 0, pxPerUnit = 0;
	};

	enum class MapPlaceKind { kTown, kOutpost, kVillage, kCamp, kNest, kWorkcamp, kSmallPlace, kRuin };	// Kenshi_MapMarkers' frames
	enum class MapTier { kMin, kMid, kMax };	// Min always shows; Mid from detail level 1, Max from 2
	enum class MapRelation { kPlayer, kAlly, kNeutral, kEnemy };	// icon and label colour (MarkerPlayer / Ally / Neutral / Enemy)

	struct MapPlace	// a discovered place (undiscovered: not in the state)
	{
		std::string id;
		MapPlaceKind kind = MapPlaceKind::kTown;
		MapTier tier = MapTier::kMin;
		double x = 0, z = 0;	// world
		std::string name;	// the label (from level 1; camps and nests have none); discovered but not visited: the host's placeholder
		MapRelation relation = MapRelation::kNeutral;
		std::string tip;	// rich text, visited towns only; empty: no tooltip
		bool isNew = false;	// the NewDiscovered badge (the host clears it when the map closes)
		bool operator==(const MapPlace&) const = default;
	};

	struct MapState
	{
		double extent = 294912;	// the world square the map image covers, centred on 0, 0
		float texels = 8192;	// the map image's side: zoom 1 = a texel a pixel
		std::vector<MapPlace> places;
		std::vector<std::pair<double, double>> characters;	// the player's characters (x, z): dots from level 1, no label, no tooltip
		double cameraX = 0, cameraZ = 0;
		float cameraYaw = 0;	// radians, clockwise on the screen from north
		bool operator==(const MapState&) const = default;
	};

	// The MAP page's view, for the host to set a default and to save and restore it (k16.2.435): zoom in pixels a map texel (MapState::texels;
	// 0 = fit the whole world), (cx, cz) the world point at the view's centre.
	struct MapView
	{
		double zoom = 0;
		double cx = 0, cz = 0;
		bool operator==(const MapView&) const = default;
	};

	struct MapActions
	{
		// At the UI list's replay (render side), under the markers, popups and tooltips: draw the map with sokol-gl for the frame (the projection is the screen in
		// pixels, y down; clipped to the rect; the UI's state comes back afterwards).
		std::function<void(const MapFrame&)> draw;
		std::function<void(double x, double z, bool queue)> moveTo;	// a right-click on the map: the selection's move order (Shift: queued)
		std::function<void(const MapView&)> viewChanged;	// the player ended a change of the view (a wheel step, a drag or a scroll bar released, - / + / O)
	};

	class Overview
	{
	public:
		explicit Overview(OverviewActions actions = {}, MapActions map = {});
		~Overview();
		bool shown() const;
		bool shown(OverviewPage p) const;	// shown on that page
		void show(bool on);
		void open(OverviewPage p);	// shown, on that page, in front
		void toggle(OverviewPage p);	// shown on p: hides; else open(p)
		OverviewPage page() const;
		Widget& pageWidget(OverviewPage p) const;	// where a screen puts its content (it fills the page)
		void setCaption(OverviewPage p, const std::string& text);	// the window's caption on that page (RESEARCH: level and rate); empty: the tab's name
		const std::string& caption() const;	// the one showing
		void setMap(MapActions map) { map_ = std::move(map); }
		void updateMap(const MapState& s);	// every frame while shown (the original refreshes the dots once a second)
		void centreMap();	// on the camera, the zoom kept (O, and opening the MAP page until the host sets a view)
		MapView mapView() const;	// the view showing (zoom 0 while fitted)
		void setMapView(const MapView& v);	// shown from now on, clamped to the image (valid before the page was ever shown); opening the page keeps it
		// For scripted tests.
		Widget* window() const;
		Widget* tab(OverviewPage p) const;
		Widget* mapArea() const;
		Widget* mapButton(int i) const;	// 0 O, 1 -, 2 +
		MapFrame mapFrame() const;	// the view as the host gets it (valid after layout)
		double mapZoom() const;	// pixels a map texel
		int mapLevel() const;	// detail level 0..2
		Rect mapMarker(size_t place) const;	// place i's icon, pixels; empty while hidden at this level
		bool mapLabel(size_t place) const;	// its label shows
		bool mapMoveShown() const;	// the move marker is up
		ScrollBar* mapBar(int i) const;	// 0 vertical (right), 1 horizontal (bottom); hidden while the image fits
		Rect mapBadge(size_t place) const;	// the NewDiscovered frame's rect (the icon's) while new and shown, else empty
		static const char* mapLabelFont();
		static float mapCameraUnits();
	private:
		class MapArea;
		CaptionWindow* win_ = nullptr;
		TabView* tabs_ = nullptr;
		MapArea* mapView_ = nullptr;
		bool hostView_ = false;	// the host set a view: opening the page no longer centres it
		Button* mapButtons_[3] = {};
		OverviewActions act_;
		MapActions map_;
		MapState mapState_;
		std::string captions_[(int)OverviewPage::kCount];
	};
}
