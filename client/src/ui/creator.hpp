#pragma once
#include <functional>
#include <string>
#include <vector>

#include "ui/widget.hpp"

// The character creator (docs/UI.md), content after the game's editor (Kenshi_CharacterEditor.layout), layout ours. Three parts across the screen:
// the left column (name, race / subrace / gender, import / export / clothes, change character, the race's description and stats, cancel / confirm),
// the host's 3D preview in the middle, and the right column (BODY / FACE / HAIR tabs over scrolling slider and picker pages, randomise all /
// randomise / reset). Like the Inventory it knows nothing about SIMLINK: the host fills a CreatorState every frame and applies CreatorActions.
namespace ui
{
	enum CreatorPage { kCreatorBody, kCreatorFace, kCreatorHair };	// the right column's tabs, in order

	struct CreatorSlider
	{
		std::string category;	// "Body" on BODY, "Face" on FACE; any other category on BODY under its own heading
		std::string name;		// shown, and handed back in setSlider
		float min = 0, max = 1, value = 0;
		float step = 0;			// 0 continuous; 1 an integer choice (idle stance): the value box shows whole numbers
		bool operator==(const CreatorSlider&) const = default;
	};

	struct CreatorRaceGroup	// a RACE_GROUP and its races (Human: Greenlander, Scorchlander)
	{
		std::string name;
		std::vector<std::string> races;
		bool operator==(const CreatorRaceGroup&) const = default;
	};

	struct CreatorStat
	{
		std::string label, value;	// "Hit Points", "100"; "Farming", "+20%"
		bool operator==(const CreatorStat&) const = default;
	};

	struct CreatorState
	{
		bool valid = false;	// false: nothing to show yet
		std::string name;
		std::vector<CreatorRaceGroup> groups;
		int group = 0, race = 0;	// race: index into groups[group].races
		int sex = 0;	// 0 male, 1 female
		bool bothSexes = true;	// false: the race has one sex; the gender picker is disabled
		// A non-editable member (an animal in the squad) is in no race group: RACE / SUBRACE / GENDER show these texts instead of the list's entry
		// (the animal's race name; no subrace or sex). Empty: the list's.
		std::string raceText, subraceText, genderText;
		int character = 0, characters = 1;	// CHANGE CHARACTER ("i/n", wrapping): index into the squad being made, and its size; hidden with one
		bool editable = true;	// false (an animal in the squad): only the stepper, CONFIRM and turning the preview
		bool clothes = true;	// the CLOTHES toggle
		std::string description;
		std::vector<CreatorStat> stats;
		int heads = 0, head = 0;	// head count and index (shown 1-based) on FACE
		// Index 0 of hair / beard is "none" (its name is shown; an empty name shows NONE). An empty list disables the picker (no beards for women).
		std::vector<std::string> hairs, beards, hairColours;
		int hair = 0, beard = 0, hairColour = 0;
		std::vector<CreatorSlider> sliders;	// skin tone and idle stance too: Body sliders ahead of the ranges
		bool operator==(const CreatorState&) const = default;
	};

	// Pickers wrap around and send the new index; sliders send every value while dragged (or a typed value, clamped). The screen shows the
	// state, so the host's answer (next frame) is what stays. done / cancel do not hide the screen: the host does (it may refuse an empty name).
	struct CreatorActions
	{
		std::function<void(const std::string&)> setName;	// on Enter or when the name box loses focus
		std::function<void(int)> setGroup, setRace, setSex, setCharacter, setHead, setHair, setBeard, setHairColour;
		std::function<void(const std::string& name, float value)> setSlider;
		std::function<void(bool)> setClothes;
		std::function<void(int page)> randomize;	// the shown tab (CreatorPage)
		std::function<void()> randomizeAll, resetDefaults, importCharacter, exportCharacter, done;
		std::function<void()> cancel;	// CANCEL and Escape; null (the new game's editor): no CANCEL, Escape goes on to the host; setCancel changes it
	};

	class Creator
	{
	public:
		// The pickers, for tests.
		enum PickerId { kGroup, kRace, kSex, kCharacter, kHead, kHair, kBeard, kHairColour };

		explicit Creator(CreatorActions actions);
		~Creator();
		void update(const CreatorState& s);	// every frame, before ui::frame (cheap when nothing changed)
		bool shown() const;
		void show(bool on);
		void setCancel(std::function<void()> cancel);	// per open: null (the new game's editor) = no CANCEL, Escape goes on to the host
		// Where the host draws the character, framebuffer pixels: the screen between the columns. Valid after ui::frame; empty while hidden.
		Rect previewRect() const;
		// For scripted tests.
		Widget* nameBox() const;
		Widget* pickerPrev(int i) const { return i >= 0 && i < (int)pickers_.size() ? pickers_[i].prev : nullptr; }
		Widget* pickerNext(int i) const { return i >= 0 && i < (int)pickers_.size() ? pickers_[i].next : nullptr; }
		Widget* pickerValue(int i) const { return i >= 0 && i < (int)pickers_.size() ? pickers_[i].value : nullptr; }	// a Label
		Widget* tab(int i) const { return i >= 0 && i < 3 ? tabs_[i] : nullptr; }
		int page() const { return page_; }
		Widget* slider(const std::string& name) const;	// a Slider; null when the state has none of that name
		Widget* sliderBox(const std::string& name) const;	// its value EditBox
		Widget* description() const;	// a Label
		Widget* statValue(size_t i) const { return i < stats_.size() ? stats_[i] : nullptr; }	// a Label
		Widget* clothesButton() const { return clothes_; }
		Widget* importButton() const { return import_; }
		Widget* exportButton() const { return export_; }
		Widget* randomAllButton() const { return randomAll_; }
		Widget* randomButton() const { return random_; }
		Widget* resetButton() const { return reset_; }
		Widget* doneButton() const { return done_; }
		Widget* cancelButton() const { return cancel_; }
		Widget* preview() const;

	private:
		class Preview;
		class Side;
		struct Picker { Button* prev; Label* value; Button* next; int index = 0, count = 0; std::function<void(int)> CreatorActions::* send = nullptr; };
		struct Row { std::string name; Slider* slider; EditBox* box; };
		Widget* screen_ = nullptr;
		Window* left_ = nullptr;
		Window* right_ = nullptr;
		Label* empty_ = nullptr;
		Widget* leftBody_ = nullptr;
		Widget* rightBody_ = nullptr;
		Widget* leftTop_ = nullptr;	// name, pickers, import / export / clothes, change character
		Widget* info_ = nullptr;	// description and stats, scrolling
		Widget* tabRow_ = nullptr;
		EditBox* name_ = nullptr;
		std::vector<Picker> pickers_;
		Button* tabs_[3] = {};
		Widget* pages_[3] = {};
		Widget* rowCols_[2] = {};	// the BODY and FACE pages' slider columns
		std::vector<Row> rows_;
		Label* desc_ = nullptr;
		Label *descHead_ = nullptr, *statHead_ = nullptr;
		Widget* statCol_ = nullptr;
		std::vector<Label*> stats_;	// value labels
		Preview* preview_ = nullptr;
		Button *clothes_ = nullptr, *import_ = nullptr, *export_ = nullptr;
		Button *randomAll_ = nullptr, *random_ = nullptr, *reset_ = nullptr, *done_ = nullptr, *cancel_ = nullptr;
		CreatorActions act_;
		CreatorState last_;
		int page_ = kCreatorBody;
		void showPage(int i);
		void addPicker(Box& col, const char* caption, std::function<void(int)> CreatorActions::* send);
		void step(Picker& p, int d);
		void setPicker(Picker& p, int index, int count, std::string text, bool usable);
		void setPicker(Picker& p, int index, const std::vector<std::string>& names);	// usable with two or more
		void rebuildSliders();
		void rebuildStats();
		void refresh(bool rows, bool stats);	// every widget from last_; rebuild the slider rows / stat rows too
	};
}
