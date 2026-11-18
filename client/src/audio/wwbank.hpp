#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Wwise soundbank (.bnk) container: chunk index plus embedded media (docs/reference/AUDIO.md). Format knowledge from bnnm/wwiser, read as documentation;
// tools/wwise_bnk.py is the reference parser for the HIRC objects. Kenshi ships bank version 112 (SFX_Bank.bnk is 88); every media file is embedded in DATA.
namespace audio
{
	struct Media { uint32_t id, offset, size; };	// offset into the DATA payload

	class Bank
	{
	public:
		bool load(const std::string& path);
		const std::string& error() const { return error_; }

		uint32_t version = 0, id = 0, feedback = 0;	// BKHD; feedback adds a bus id to every node base
		std::string path;
		std::vector<Media> media;	// DIDX order

		// Embedded .wem bytes for a media id, or nullptr.
		const uint8_t* mediaData(uint32_t mediaId, uint32_t& size) const;
		const uint8_t* mediaData(const Media& m) const { return file_.data() + dataOff_ + m.offset; }
		// HIRC payload (object count + objects), empty if absent.
		const uint8_t* hirc(uint32_t& size) const { size = hircSize_; return hircSize_ ? file_.data() + hircOff_ : nullptr; }

	private:
		std::vector<uint8_t> file_;
		size_t dataOff_ = 0, dataSize_ = 0, hircOff_ = 0, hircSize_ = 0;
		std::string error_;
	};
}
