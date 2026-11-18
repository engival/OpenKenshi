#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

// SIMLINK's little-endian frame payload reader and frame writer (docs/formats/SIMLINK.md section 2), shared by the decoder and the debug view.
namespace simlink::io
{

// Little-endian reader over one frame payload; any overrun sets bad and returns zeros.
struct Reader
{
	const uint8_t* p;
	size_t n, at = 0;
	bool bad = false;
	bool take(void* out, size_t k)
	{
		if (bad || n - at < k) { bad = true; memset(out, 0, k); return false; }
		memcpy(out, p + at, k);
		at += k;
		return true;
	}
	template <class T> T get() { T v; take(&v, sizeof v); return v; }
	uint8_t u8() { return get<uint8_t>(); }
	uint16_t u16() { return get<uint16_t>(); }
	uint32_t u32() { return get<uint32_t>(); }
	uint64_t u64() { return get<uint64_t>(); }
	int16_t i16() { return get<int16_t>(); }
	int32_t i32() { return get<int32_t>(); }
	int64_t i64() { return get<int64_t>(); }
	float f32() { return get<float>(); }
	std::string str()
	{
		const uint16_t k = u16();
		if (bad || n - at < k) { bad = true; return {}; }
		std::string s((const char*)p + at, k);
		at += k;
		return s;
	}
};

struct Writer
{
	std::vector<uint8_t>& out;
	size_t start;
	Writer(std::vector<uint8_t>& o, uint16_t type) : out(o), start(o.size()) { put<uint32_t>(0); put(type); }
	~Writer() { const uint32_t len = (uint32_t)(out.size() - start - 4); memcpy(out.data() + start, &len, 4); }
	template <class T> void put(T v) { const uint8_t* b = (const uint8_t*)&v; out.insert(out.end(), b, b + sizeof v); }
	void str(const std::string& s) { put((uint16_t)s.size()); out.insert(out.end(), s.begin(), s.end()); }
	void ids(const std::vector<uint64_t>& v) { put((uint16_t)v.size()); for (uint64_t id : v) { put(id); } }
};

} // namespace simlink::io
