#pragma once
// The character creator's model (docs/CLIENT.md "Character creator"): the edited Spec, the race's editor choices and the preview character the
// edits rebuild (Characters::replace). No UI and no sim link here: the host and the creator screen call it, a script can drive it.
#include <cstdint>
#include <string>

#include "characters/characters.hpp"

class Creator
{
public:
	// Adds the preview character at pos facing yaw (a default of `race`: head 0, the sliders at their mids, no hair). False with err set.
	bool open(Characters& chars, const double pos[3], float yaw, const std::string& race, bool female, std::string& err);
	bool active() const { return chars_ != nullptr; }
	void close() { chars_ = nullptr; }	// the preview stays in Characters until its owner rebuilds the set (SimView::hold)
	size_t index() const { return index_; }		// the preview in Characters
	Characters::Spec& spec() { return spec_; }	// edit, then apply()
	const Characters::RaceChoices& choices() const { return choices_; }
	const std::vector<Characters::EditorRange>& ranges() const { return choices_.ranges[spec_.female ? 1 : 0]; }
	// The original's Skin Tone slider top for this race and sex, in integer tenths of a palette step: 10 per gap between the race's <Colour>
	// swatches (50 for the human 6), 0 without one. spec().skin (0..1) is skinMax() / 10 along the palette, so a body saved above the top keeps
	// its colour and only the slider's range is capped (kenshi-k16.2.432.3).
	int skinMax() const;
	bool clothes = true;	// the CLOTHES toggle: false hides the worn armour and weapons on the preview (visual only: spec() keeps them; hair and beard stay)
	bool apply(std::string& err);			// rebuilds the preview from spec()
	// A new race or sex resets the race-dependent choices (head, hair, beard, hair colour, sliders); false (nothing changed) for an unknown race.
	bool setRace(const std::string& race, bool female, std::string& err);
	// A non-editable member's preview (an animal in the squad): the race's default look in the creator scene (the edited member's look is the
	// host's, so nothing is lost), keeping the preview's place and its standing idle. The wire Spec the sim sent is not touched. apply() after.
	bool defaultLook(const std::string& race, bool female, std::string& err);
	float value(const std::string& slider) const;	// the slider's value (its mid when unset)
	void setValue(const std::string& slider, float v);	// clamped to the range; apply() after
	// The original's RANDOMISE (findings/charedit.md; see the .cpp): every Range by its mid +- variation with random_group shared, a head, weighted hair and beard, hair colour, skin tone. apply() after.
	// part: kAll, or one creator tab: kBody (the Body sliders, skin), kFace (the Face sliders, head), kHair (hair, beard, colour)
	enum Part { kAll = -1, kBody, kFace, kHair };
	void randomize(uint32_t seed, int part = kAll);
	std::string specString() const;	// the preview as a --character SPEC (export)
	std::string name;	// the character's name (the creator's NAME box)

private:
	Characters* chars_ = nullptr;
	size_t index_ = 0;
	Characters::Spec spec_;
	Characters::RaceChoices choices_;
};
