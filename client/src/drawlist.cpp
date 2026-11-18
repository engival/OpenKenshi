#include "drawlist.hpp"

#include <cassert>
#include <utility>

namespace dl
{
	void List::begin(int w, int h)
	{
		w_ = w;
		h_ = h;
		clip_ = { 0, 0, (float)w, (float)h };
		tex_ = 0;
		pipe_ = Pipe::kUi;
		layer_ = kLayerInherit;
		open_ = false;
		cmds_.clear();
		verts_.clear();
		hosts_.clear();
	}

	void List::beginPrim(Prim p)
	{
		assert(!open_);
		Cmd c;
		c.kind = Cmd::Kind::kDraw;
		c.prim = p;
		c.pipe = pipe_;
		c.layer = layer_;
		c.tex = tex_;
		c.clip = clip_;
		c.first = (uint32_t)verts_.size();
		cmds_.push_back(c);
		open_ = true;
	}

	void List::endPrim()
	{
		assert(open_ && !cmds_.empty());
		Cmd& c = cmds_.back();
		c.count = (uint32_t)verts_.size() - c.first;
		open_ = false;
	}

	void List::host(const Rect& r, std::function<void(float, float, float, float)> draw)
	{
		assert(!open_);
		if (!draw) { return; }
		Cmd c;
		c.kind = Cmd::Kind::kHost;
		c.pipe = pipe_;
		c.layer = layer_;
		c.clip = clip_;
		c.host = (uint32_t)hosts_.size();
		hosts_.push_back({ r, std::move(draw) });
		cmds_.push_back(c);
	}

	void List::create(TexId id, int w, int h, std::vector<uint8_t> rgba, int mips, Filter f, bool updatable, const char* label)
	{
		Upload u;
		u.op = Upload::Op::kCreate;
		u.id = id;
		u.w = w;
		u.h = h;
		u.mips = mips < 1 ? 1 : mips;
		u.filter = f;
		u.updatable = updatable;
		u.label = label;
		u.rgba = std::move(rgba);
		ups_.push_back(std::move(u));
	}

	void List::update(TexId id, int x, int y, int w, int h, std::vector<uint8_t> rgba)
	{
		Upload u;
		u.op = Upload::Op::kUpdate;
		u.id = id;
		u.x = x;
		u.y = y;
		u.w = w;
		u.h = h;
		u.rgba = std::move(rgba);
		ups_.push_back(std::move(u));
	}

	void List::destroy(TexId id)
	{
		Upload u;
		u.op = Upload::Op::kDestroy;
		u.id = id;
		ups_.push_back(std::move(u));
	}

	std::vector<Upload> List::takeUploads() { return std::exchange(ups_, {}); }
}
