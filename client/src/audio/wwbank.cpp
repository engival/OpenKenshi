#include "audio/wwbank.hpp"
#include <cstring>
#include <fstream>

namespace audio
{
	static uint32_t le32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24; }

	bool Bank::load(const std::string& path)
	{
		std::ifstream in(path, std::ios::binary);
		if (!in) { error_ = "cannot open " + path; return false; }
		this->path = path;
		in.seekg(0, std::ios::end);
		file_.resize(size_t(in.tellg()));
		in.seekg(0);
		in.read(reinterpret_cast<char*>(file_.data()), std::streamsize(file_.size()));
		if (!in) { error_ = "read failed: " + path; return false; }

		bool haveData = false;
		const uint8_t* didx = nullptr;
		size_t didxSize = 0;
		for (size_t off = 0; off + 8 <= file_.size();)
		{
			const uint8_t* p = file_.data() + off;
			uint32_t size = le32(p + 4);
			if (off + 8 + size > file_.size()) { error_ = "truncated chunk in " + path; return false; }
			if (!memcmp(p, "BKHD", 4) && size >= 16) { version = le32(p + 8); id = le32(p + 12); feedback = le32(p + 20); }
			else if (!memcmp(p, "DIDX", 4)) { didx = p + 8; didxSize = size; }
			else if (!memcmp(p, "DATA", 4)) { dataOff_ = off + 8; dataSize_ = size; haveData = true; }
			else if (!memcmp(p, "HIRC", 4)) { hircOff_ = off + 8; hircSize_ = size; }
			off += 8 + size;
		}
		if (!version) { error_ = "no BKHD in " + path; return false; }
		for (size_t i = 0; didx && i + 12 <= didxSize; i += 12)
		{
			Media m { le32(didx + i), le32(didx + i + 4), le32(didx + i + 8) };
			if (!haveData || size_t(m.offset) + m.size > dataSize_) { error_ = "media " + std::to_string(m.id) + " outside DATA in " + path; return false; }
			media.push_back(m);
		}
		return true;
	}

	const uint8_t* Bank::mediaData(uint32_t mediaId, uint32_t& size) const
	{
		for (const Media& m : media)
		{
			if (m.id == mediaId) { size = m.size; return mediaData(m); }
		}
		size = 0;
		return nullptr;
	}
}
