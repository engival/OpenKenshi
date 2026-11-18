#include "audio/wwvorbis.hpp"
#include "audio/bits.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vorbis/codec.h>

namespace audio
{
	static const uint8_t kCodebookData[] = {
#include "wwise_codebooks.inc"	// generated from third_party/wwise_codebooks/packed_codebooks_aoTuV_603.bin
	};

	static uint16_t le16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
	static uint32_t le32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24; }

	// Largest r with r^dims <= entries (Vorbis I spec, lookup1_values).
	static uint32_t lookup1Values(uint32_t entries, uint32_t dims)
	{
		uint32_t r = uint32_t(std::floor(std::pow(double(entries), 1.0 / dims)));
		auto fits = [&](uint32_t v) { uint64_t acc = 1; for (uint32_t i = 0; i < dims; i++) { acc *= v; if (acc > entries) return false; } return true; };
		while (fits(r + 1)) r++;
		while (r && !fits(r)) r--;
		return r;
	}

	const Codebooks& Codebooks::standard()
	{
		static Codebooks books = [] { Codebooks b; b.load(kCodebookData, sizeof kCodebookData); return b; }();
		return books;
	}

	bool Codebooks::load(const uint8_t* data, size_t size)
	{
		offsets_.clear();
		if (size < 4) return false;
		uint32_t table = le32(data + size - 4);
		if (table > size - 4 || (size - table) % 4) return false;
		data_ = data;
		// The table's last entry is the table offset itself, which closes the final book.
		for (size_t p = table; p < size; p += 4) offsets_.push_back(le32(data + p));
		return true;
	}

	// Packed book: 4-bit dimensions, 14-bit entries, codeword lengths in a narrower field, 1-bit lookup type.
	bool Codebooks::rebuild(uint32_t i, BitWriter& out) const
	{
		if (i >= count() || offsets_[i + 1] < offsets_[i]) return false;
		size_t size = offsets_[i + 1] - offsets_[i];
		BitReader in(data_ + offsets_[i], size);
		uint32_t dims = in.get(4), entries = in.get(14);
		out.put(0x564342, 24);
		out.put(dims, 16);
		out.put(entries, 24);
		uint32_t ordered = in.get(1);
		out.put(ordered, 1);
		if (ordered)
		{
			out.put(in.get(5), 5);
			uint32_t cur = 0;
			while (cur < entries)
			{
				int bits = ilog(entries - cur);
				uint32_t n = in.get(bits);
				out.put(n, bits);
				cur += n;	// may be 0: no codewords of this length
			}
			if (cur > entries) return false;
		} else {
			int lenBits = int(in.get(3));
			uint32_t sparse = in.get(1);
			if (lenBits == 0 || lenBits > 5) return false;
			out.put(sparse, 1);
			for (uint32_t e = 0; e < entries; e++)
			{
				if (sparse)
				{
					uint32_t present = in.get(1);
					out.put(present, 1);
					if (!present) continue;
				}
				out.put(in.get(lenBits), 5);
			}
		}
		uint32_t lookup = in.get(1);
		out.put(lookup, 4);
		if (lookup)
		{
			out.put(in.get(32), 32);	// minimum
			out.put(in.get(32), 32);	// delta
			uint32_t valueBits = in.get(4);
			out.put(valueBits, 4);
			out.put(in.get(1), 1);	// sequence_p
			for (uint32_t q = lookup1Values(entries, dims); q; q--) out.put(in.get(int(valueBits) + 1), int(valueBits) + 1);
		}
		// The packed books are byte padded with at least one spare bit.
		return !in.overrun() && in.bitPos() / 8 + 1 == size;
	}

	struct WemDecoder::State
	{
		vorbis_info vi;
		vorbis_comment vc;
		vorbis_dsp_state vd;
		vorbis_block vb;
		bool dsp = false;
		std::vector<uint8_t> packet;	// the rebuilt audio packet handed to libvorbis

		State() { vorbis_info_init(&vi); vorbis_comment_init(&vc); }
		~State()
		{
			if (dsp) { vorbis_block_clear(&vb); vorbis_dsp_clear(&vd); }
			vorbis_comment_clear(&vc);
			vorbis_info_clear(&vi);
		}
	};

	WemDecoder::WemDecoder() = default;
	WemDecoder::~WemDecoder() = default;

	static void putHeader(BitWriter& w, int type)
	{
		w.put(uint32_t(type), 8);
		w.putBytes("vorbis", 6);
	}

	bool WemDecoder::open(const uint8_t* wem, size_t size)
	{
		st_ = std::make_unique<State>();
		packets_.clear();
		info_ = {};
		error_.clear();
		if (size < 12 || memcmp(wem, "RIFF", 4) || memcmp(wem + 8, "WAVE", 4)) { error_ = "not a RIFF/WAVE wem"; return false; }

		const uint8_t* fmt = nullptr;
		uint32_t fmtSize = 0, dataSize = 0;
		for (size_t off = 12; off + 8 <= size;)
		{
			const uint8_t* p = wem + off;
			uint32_t cs = le32(p + 4);
			if (off + 8 + cs > size) { error_ = "truncated RIFF chunk"; return false; }
			if (!memcmp(p, "fmt ", 4)) { fmt = p + 8; fmtSize = cs; }
			else if (!memcmp(p, "data", 4)) { data_ = p + 8; dataSize = cs; }
			else if (!memcmp(p, "smpl", 4) && cs >= 0x34 && le32(p + 8 + 0x1c) >= 1)
			{
				info_.loopStart = le32(p + 8 + 0x2c);
				info_.loopEnd = le32(p + 8 + 0x30) + 1;	// smpl end is the last looped sample (632879743: end = sampleCount - 1)
			}
			off += 8 + cs + (cs & 1);
		}
		if (!fmt || !data_) { error_ = "missing fmt or data chunk"; return false; }
		if (le16(fmt) != 0xFFFF) { error_ = "codec " + std::to_string(le16(fmt)) + " is not Wwise Vorbis"; return false; }
		if (fmtSize != 0x42) { error_ = "unsupported fmt size " + std::to_string(fmtSize) + " (only 0x42 with inline vorb)"; return false; }

		info_.channels = le16(fmt + 2);
		info_.sampleRate = le32(fmt + 4);
		uint32_t avgBytes = le32(fmt + 8);
		const uint8_t* vorb = fmt + 0x18;
		info_.sampleCount = le32(vorb);
		// ww2ogg's observation: these values at vorb+4 mark files whose audio packets keep the standard first byte.
		uint32_t modSignal = le32(vorb + 4);
		modPackets_ = modSignal != 0x4A && modSignal != 0x4B && modSignal != 0x69 && modSignal != 0x70;
		uint32_t setupOff = le32(vorb + 0x10), audioOff = le32(vorb + 0x14);
		int bs0 = vorb[0x28], bs1 = vorb[0x29];
		if (info_.channels < 1 || info_.channels > 8 || !info_.sampleRate) { error_ = "bad channel count or rate"; return false; }
		if (info_.loopEnd > info_.sampleCount) info_.loopEnd = info_.sampleCount;

		// Packets: 16-bit size, no granule.
		if (setupOff + 2 > dataSize) { error_ = "setup packet outside data"; return false; }
		uint32_t setupSize = le16(data_ + setupOff);
		if (setupOff + 2 + setupSize != audioOff) { error_ = "first audio packet does not follow the setup packet"; return false; }
		for (uint32_t off = audioOff; off < dataSize;)
		{
			if (off + 2 > dataSize) { error_ = "truncated packet header"; return false; }
			uint32_t ps = le16(data_ + off);
			if (off + 2 + ps > dataSize) { error_ = "truncated audio packet"; return false; }
			packets_.push_back({ off + 2, ps });
			off += 2 + ps;
		}

		BitWriter id, comment, setup;
		putHeader(id, 1);
		id.put(0, 32);
		id.put(uint32_t(info_.channels), 8);
		id.put(info_.sampleRate, 32);
		id.put(0, 32);
		id.put(avgBytes * 8, 32);
		id.put(0, 32);
		id.put(uint32_t(bs0), 4);
		id.put(uint32_t(bs1), 4);
		id.put(1, 1);

		static const char kVendor[] = "kenshi kclient (Wwise Vorbis, rebuilt on load)";
		putHeader(comment, 3);
		comment.put(sizeof kVendor - 1, 32);
		comment.putBytes(kVendor, sizeof kVendor - 1);
		comment.put(0, 32);
		comment.put(1, 1);

		putHeader(setup, 5);
		if (!rebuildSetup(data_ + setupOff + 2, setupSize, setup)) return false;

		BitWriter* hdr[3] = { &id, &comment, &setup };
		for (int i = 0; i < 3; i++)
		{
			ogg_packet op {};
			op.packet = hdr[i]->bytes.data();
			op.bytes = long(hdr[i]->bytes.size());
			op.b_o_s = i == 0;
			op.packetno = i;
			int r = vorbis_synthesis_headerin(&st_->vi, &st_->vc, &op);
			if (r) { error_ = "libvorbis rejected rebuilt header " + std::to_string(i) + " (" + std::to_string(r) + ")"; return false; }
		}
		if (vorbis_synthesis_init(&st_->vd, &st_->vi) || vorbis_block_init(&st_->vd, &st_->vb)) { error_ = "vorbis_synthesis_init failed"; return false; }
		st_->dsp = true;
		next_ = 0;
		produced_ = 0;
		prevLong_ = false;
		return true;
	}

	// Wwise setup: codebook indices, then floors/residues/mappings/modes with the type fields and the time-domain placeholders dropped.
	bool WemDecoder::rebuildSetup(const uint8_t* p, size_t size, BitWriter& out)
	{
		BitReader in(p, size);
		auto copy = [&](int n) { uint32_t v = in.get(n); out.put(v, n); return v; };
		auto fail = [&](const char* why) { error_ = std::string("setup: ") + why; return false; };

		uint32_t books = copy(8) + 1;
		for (uint32_t i = 0; i < books; i++)
		{
			uint32_t idx = in.get(10);
			if (!Codebooks::standard().rebuild(idx, out)) return fail(("bad external codebook " + std::to_string(idx)).c_str());
		}
		out.put(0, 6);	// time domain count - 1
		out.put(0, 16);	// the one (unused) time transform

		uint32_t floors = copy(6) + 1;
		for (uint32_t f = 0; f < floors; f++)
		{
			out.put(1, 16);	// always floor 1
			uint32_t parts = copy(5);
			std::vector<uint32_t> partClass(parts);
			uint32_t maxClass = 0;
			for (uint32_t& c : partClass) { c = copy(4); maxClass = std::max(maxClass, c); }
			std::vector<uint32_t> classDims(maxClass + 1);
			for (uint32_t c = 0; c <= maxClass; c++)
			{
				classDims[c] = copy(3) + 1;
				uint32_t subs = copy(2);
				if (subs && copy(8) >= books) return fail("floor masterbook out of range");
				for (uint32_t k = 0; k < (1u << subs); k++)
				{
					if (int(copy(8)) - 1 >= int(books)) return fail("floor subclass book out of range");
				}
			}
			copy(2);	// multiplier - 1
			int rangeBits = int(copy(4));
			for (uint32_t c : partClass)
			{
				for (uint32_t k = 0; k < classDims[c]; k++) copy(rangeBits);
			}
		}

		uint32_t residues = copy(6) + 1;
		for (uint32_t r = 0; r < residues; r++)
		{
			uint32_t type = in.get(2);
			if (type > 2) return fail("residue type");
			out.put(type, 16);
			copy(24);	// begin
			copy(24);	// end
			copy(24);	// partition size - 1
			uint32_t classes = copy(6) + 1;
			if (copy(8) >= books) return fail("residue classbook out of range");
			std::vector<uint32_t> cascade(classes);
			for (uint32_t& c : cascade)
			{
				c = copy(3);
				if (copy(1)) c |= copy(5) << 3;
			}
			for (uint32_t c : cascade)
			{
				for (int k = 0; k < 8; k++)
				{
					if ((c >> k & 1) && copy(8) >= books) return fail("residue book out of range");
				}
			}
		}

		uint32_t mappings = copy(6) + 1;
		int chanBits = ilog(uint32_t(info_.channels - 1));
		for (uint32_t m = 0; m < mappings; m++)
		{
			out.put(0, 16);	// always mapping 0
			uint32_t submaps = copy(1) ? copy(4) + 1 : 1;
			if (copy(1))
			{
				uint32_t steps = copy(8) + 1;
				for (uint32_t s = 0; s < steps; s++)
				{
					uint32_t mag = copy(chanBits), ang = copy(chanBits);
					if (mag == ang || mag >= uint32_t(info_.channels) || ang >= uint32_t(info_.channels)) return fail("bad channel coupling");
				}
			}
			if (copy(2)) return fail("mapping reserved bits set");
			if (submaps > 1)
			{
				for (int c = 0; c < info_.channels; c++)
				{
					if (copy(4) >= submaps) return fail("mapping mux out of range");
				}
			}
			for (uint32_t s = 0; s < submaps; s++)
			{
				copy(8);	// time config (unused)
				if (copy(8) >= floors) return fail("mapping floor out of range");
				if (copy(8) >= residues) return fail("mapping residue out of range");
			}
		}

		uint32_t modes = copy(6) + 1;
		modeLong_.assign(modes, 0);
		modeBits_ = ilog(modes - 1);
		for (uint32_t m = 0; m < modes; m++)
		{
			modeLong_[m] = uint8_t(copy(1));
			out.put(0, 16);	// window type
			out.put(0, 16);	// transform type
			if (copy(8) >= mappings) return fail("mode mapping out of range");
		}
		out.put(1, 1);	// framing

		if (in.overrun() || (in.bitPos() + 7) / 8 != size) return fail("did not consume exactly the setup packet");
		return true;
	}

	// Feeds the next audio packet; false at the end.
	bool WemDecoder::nextPacket()
	{
		if (next_ >= packets_.size()) return false;
		const Packet& pk = packets_[next_];
		const uint8_t* src = data_ + pk.offset;
		ogg_packet op {};
		if (modPackets_ && pk.size)
		{
			// Restore the packet-type bit, and for long blocks the previous/next window flags, ahead of the mode number.
			BitWriter w;
			BitReader first(src, 1);
			uint32_t mode = first.get(modeBits_);
			if (mode >= modeLong_.size()) { error_ = "audio packet mode out of range"; next_ = packets_.size(); return false; }
			w.put(0, 1);
			w.put(mode, modeBits_);
			if (modeLong_[mode])
			{
				bool nextLong = false;
				if (next_ + 1 < packets_.size() && packets_[next_ + 1].size)
				{
					BitReader peek(data_ + packets_[next_ + 1].offset, 1);
					uint32_t nm = peek.get(modeBits_);
					nextLong = nm < modeLong_.size() && modeLong_[nm];
				}
				w.put(prevLong_, 1);
				w.put(nextLong, 1);
			}
			prevLong_ = modeLong_[mode];
			w.put(first.get(8 - modeBits_), 8 - modeBits_);
			for (uint32_t i = 1; i < pk.size; i++) w.put(src[i], 8);
			st_->packet = std::move(w.bytes);
			op.packet = st_->packet.data();
			op.bytes = long(st_->packet.size());
		} else {
			op.packet = const_cast<uint8_t*>(src);
			op.bytes = long(pk.size);
		}
		op.packetno = long(next_ + 3);
		op.granulepos = -1;
		op.e_o_s = next_ + 1 == packets_.size();
		next_++;
		if (vorbis_synthesis(&st_->vb, &op) == 0) vorbis_synthesis_blockin(&st_->vd, &st_->vb);
		return true;
	}

	size_t WemDecoder::read(float* out, size_t frames)
	{
		if (!st_ || !st_->dsp) return 0;
		size_t done = 0;
		int ch = info_.channels;
		while (done < frames && produced_ < info_.sampleCount)
		{
			float** pcm;
			int avail = vorbis_synthesis_pcmout(&st_->vd, &pcm);
			if (avail <= 0)
			{
				if (!nextPacket()) break;
				continue;
			}
			size_t n = std::min({ size_t(avail), frames - done, size_t(info_.sampleCount - produced_) });
			for (size_t i = 0; i < n; i++)
			{
				for (int c = 0; c < ch; c++) out[(done + i) * ch + c] = pcm[c][i];
			}
			vorbis_synthesis_read(&st_->vd, int(n));
			done += n;
			produced_ += uint32_t(n);
		}
		return done;
	}

	void WemDecoder::rewind()
	{
		if (!st_ || !st_->dsp) return;
		vorbis_synthesis_restart(&st_->vd);
		next_ = 0;
		produced_ = 0;
		prevLong_ = false;
	}

	void WemDecoder::seek(uint32_t sample)
	{
		if (sample < produced_) rewind();
		float scratch[1024 * 8];
		size_t chunk = sizeof scratch / sizeof scratch[0] / size_t(std::max(info_.channels, 1));
		while (produced_ < sample)
		{
			if (!read(scratch, std::min(chunk, size_t(sample - produced_)))) break;
		}
	}
}
