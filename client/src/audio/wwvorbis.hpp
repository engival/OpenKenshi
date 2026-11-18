#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// Wwise Vorbis (.wem, codec 0xFFFF) decoder. Wwise strips Vorbis down: no identification/comment headers, a setup packet whose codebooks are 10-bit
// indices into the aoTuV 6.03 library (third_party/wwise_codebooks) and whose floor/residue/mapping/mode fields are narrowed, 2-byte packet headers,
// and audio packets without the packet-type bit and window flags. open() rebuilds standard Vorbis I headers in memory and each audio packet as it is
// fed to libvorbis; nothing is converted on disk. Format knowledge from hcs64/ww2ogg (BSD-3) and the Vorbis I spec, read as documentation; own code.
// Scope: the layout Kenshi uses (fmt size 0x42 with the vorb fields inside fmt, little endian).
namespace audio
{
	class BitWriter;

	class Codebooks
	{
	public:
		// The embedded aoTuV 6.03 library.
		static const Codebooks& standard();
		bool load(const uint8_t* data, size_t size);
		size_t count() const { return offsets_.size() ? offsets_.size() - 1 : 0; }
		// Writes codebook i in full Vorbis form. False if i is unknown or the packed book is malformed.
		bool rebuild(uint32_t i, BitWriter& out) const;

	private:
		const uint8_t* data_ = nullptr;
		std::vector<uint32_t> offsets_;
	};

	struct WemInfo
	{
		int channels = 0;
		uint32_t sampleRate = 0, sampleCount = 0;
		uint32_t loopStart = 0, loopEnd = 0;	// smpl chunk, loopEnd exclusive (0: no loop points)
	};

	class WemDecoder
	{
	public:
		WemDecoder();
		~WemDecoder();
		WemDecoder(const WemDecoder&) = delete;
		WemDecoder& operator=(const WemDecoder&) = delete;

		// Parses the wem and sets up libvorbis. The bytes must outlive the decoder (they belong to the Bank).
		bool open(const uint8_t* wem, size_t size);
		const std::string& error() const { return error_; }
		const WemInfo& info() const { return info_; }

		// Decodes up to `frames` interleaved float frames. Returns the frames written; 0 at the end of the stream (or after an error).
		size_t read(float* out, size_t frames);
		// Back to sample 0.
		void rewind();
		// To an absolute sample: rewinds when going back, then decodes and discards (Vorbis has no index; short SFX loops only pay this once per wrap).
		void seek(uint32_t sample);
		uint32_t position() const { return produced_; }

	private:
		bool rebuildSetup(const uint8_t* p, size_t size, BitWriter& out);
		bool nextPacket();

		struct Packet { uint32_t offset, size; };
		struct State;
		std::unique_ptr<State> st_;
		const uint8_t* data_ = nullptr;	// the data chunk payload
		std::vector<Packet> packets_;	// audio packets
		std::vector<uint8_t> modeLong_;	// blockflag per mode
		int modeBits_ = 0;
		bool modPackets_ = true, prevLong_ = false;
		size_t next_ = 0;
		uint32_t produced_ = 0;
		WemInfo info_;
		std::string error_;
	};
}
