#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

// Vorbis bit packing: values are stored LSB first, filling each byte from bit 0 up (Vorbis I spec section 2).
namespace audio
{
	class BitReader
	{
	public:
		BitReader(const uint8_t* data, size_t size) : data_(data), size_(size) {}

		// Reads n <= 32 bits; past the end reads zeros and sets overrun().
		uint32_t get(int n)
		{
			uint32_t v = 0;
			for (int i = 0; i < n; i++)
			{
				size_t byte = pos_ >> 3;
				if (byte >= size_) { overrun_ = true; pos_++; continue; }
				v |= uint32_t((data_[byte] >> (pos_ & 7)) & 1) << i;
				pos_++;
			}
			return v;
		}
		size_t bitPos() const { return pos_; }
		bool overrun() const { return overrun_; }

	private:
		const uint8_t* data_;
		size_t size_, pos_ = 0;
		bool overrun_ = false;
	};

	class BitWriter
	{
	public:
		void put(uint32_t v, int n)
		{
			for (int i = 0; i < n; i++)
			{
				if ((pos_ & 7) == 0) bytes.push_back(0);
				bytes.back() |= uint8_t(((v >> i) & 1) << (pos_ & 7));
				pos_++;
			}
		}
		void putBytes(const char* s, size_t n) { for (size_t i = 0; i < n; i++) put(uint8_t(s[i]), 8); }
		size_t bitPos() const { return pos_; }

		std::vector<uint8_t> bytes;

	private:
		size_t pos_ = 0;
	};

	// Vorbis ilog: bits needed to hold v (ilog(0) = 0).
	inline int ilog(uint32_t v)
	{
		int n = 0;
		while (v) { n++; v >>= 1; }
		return n;
	}
}
