#include "portraits.hpp"
#include "drawstats.hpp"

#include <chrono>
#include <cmath>

#include "characters.hpp"
#include "portrait.glsl.h"

namespace
{
	double now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
}

void Portraits::init()
{
	auto colour = [](int size, sg_pixel_format fmt, const char* label, sg_image& img, sg_view& tex, sg_view& att)
	{
		sg_image_desc id = {};
		id.usage.color_attachment = true;
		id.width = id.height = size;
		id.pixel_format = fmt;
		id.sample_count = 1;
		id.label = label;
		img = sg_make_image(&id);
		sg_view_desc vd = {};
		vd.texture.image = img;
		tex = sg_make_view(&vd);
		vd = {};
		vd.color_attachment.image = img;
		att = sg_make_view(&vd);
	};
	colour(kAtlas, SG_PIXELFORMAT_RGBA8, "portraits", atlas_, atlasTex_, atlasAtt_);
	colour(kCell, SG_PIXELFORMAT_RGBA16F, "portrait-scratch", scratch_, scratchTex_, scratchAtt_);	// the character pipelines' colour format
	sg_image_desc id = {};
	id.usage.depth_stencil_attachment = true;
	id.width = id.height = kCell;
	id.pixel_format = SG_PIXELFORMAT_DEPTH_STENCIL;
	id.sample_count = 1;
	id.label = "portrait-depth";
	depth_ = sg_make_image(&id);
	sg_view_desc vd = {};
	vd.depth_stencil_attachment.image = depth_;
	depthAtt_ = sg_make_view(&vd);
	sg_sampler_desc sd = {};
	sd.min_filter = sd.mag_filter = SG_FILTER_LINEAR;
	sd.wrap_u = sd.wrap_v = SG_WRAP_CLAMP_TO_EDGE;
	smp_ = sg_make_sampler(&sd);
	shd_ = sg_make_shader(portrait_shader_desc(sg_query_backend()));
	sg_pipeline_desc pd = {};
	pd.shader = shd_;
	pd.colors[0].pixel_format = SG_PIXELFORMAT_RGBA8;
	pd.depth.pixel_format = SG_PIXELFORMAT_NONE;
	pd.sample_count = 1;
	pd.label = "portrait-copy";
	pip_ = sg_make_pipeline(&pd);

	// deferred.material as rtt.hlsl reads it (lighting.inc's terms): lightDirection (1, 1, 2) towards the light, lightColour 0.7, exposure 0.5;
	// the IBL multipliers are 1 (sun y 0.41 saturates the 5 sunY + 0.2 clamp); fog off (start / end far past farClip 400)
	const float l = std::sqrt(6.0f);
	light_.v[0][0] = 1 / l;
	light_.v[0][1] = 1 / l;
	light_.v[0][2] = 2 / l;
	for (int k = 0; k < 3; k++) { light_.v[1][k] = 0.7f; light_.v[7][k] = 1; }
	light_.v[1][3] = 1;
	light_.v[2][3] = 1;	// envColour.w
	light_.v[3][3] = 1;	// ambientParams.w (the sun term's scale)
	light_.v[4][3] = 0.5f;	// exposure
	light_.v[5][0] = 1e9f;
	light_.v[5][1] = 2e9f;
}

void Portraits::shutdown()
{
	sg_destroy_pipeline(pip_);
	sg_destroy_shader(shd_);
	sg_destroy_sampler(smp_);
	for (sg_view v : { atlasTex_, atlasAtt_, scratchTex_, scratchAtt_, depthAtt_ }) { sg_destroy_view(v); }
	for (sg_image i : { atlas_, scratch_, depth_ }) { sg_destroy_image(i); }
	std::lock_guard<std::mutex> lock(doneMtx_);
	done_.clear();
}

std::vector<PortraitDone> Portraits::take()
{
	std::vector<PortraitDone> out;
	std::lock_guard<std::mutex> lock(doneMtx_);
	out.swap(done_);
	return out;
}

bool PortraitTable::get(uint64_t key, int index, uint32_t version, uint32_t serial, float uv[4])
{
	constexpr int kPerRow = Portraits::kPerRow;
	Cell& c = cells_[key];
	c.used = frame_;
	const double t = now();
	if (c.version != version || c.serial != serial)
	{
		c.version = version;
		c.serial = serial;
		c.dirty = true;
		c.due = c.ready ? t + 1.0 : t;
	}
	if (c.dirty && t >= c.due && index >= 0)
	{
		bool queued = false;
		for (Job& j : queue_) { if (j.key == key) { j.index = index; queued = true; } }
		if (!queued) { queue_.push_back({ key, index }); }
		c.dirty = false;
	}
	if (c.cell < 0 || !c.ready) { return false; }
	// The copy's viewport row 0 is the atlas's bottom (a bottom left viewport), the image upright in it (portrait.glsl reads the scratch through
	// origin.inc). GL stores the bottom row first (texture v 0), so the cell's top is at v (row + 1) / kPerRow; a top left origin backend
	// (sokol's features.origin_top_left: Vulkan, D3D, Metal) stores it last, so the cell's top is at 1 - (row + 1) / kPerRow.
	const float col = (float)(c.cell % kPerRow), row = (float)(c.cell / kPerRow);
	uv[0] = col / kPerRow;
	uv[1] = topLeft_ ? 1.0f - (row + 1) / kPerRow : (row + 1) / kPerRow;
	uv[2] = (col + 1) / kPerRow;
	uv[3] = topLeft_ ? 1.0f - row / kPerRow : row / kPerRow;
	return true;
}

int PortraitTable::takeCell()
{
	if (next_ < Portraits::kCells) { return next_++; }
	// all taken: the least recently shown portrait gives up its cell
	auto oldest = cells_.end();
	for (auto it = cells_.begin(); it != cells_.end(); ++it)
	{
		if (it->second.cell >= 0 && (oldest == cells_.end() || it->second.used < oldest->second.used)) { oldest = it; }
	}
	const int cell = oldest->second.cell;
	cells_.erase(oldest);
	return cell;
}

void PortraitTable::send(std::vector<PortraitJob>& out, int budget)
{
	out.clear();
	frame_++;
	for (; budget > 0 && !queue_.empty(); budget--)
	{
		const Job job = queue_.front();
		queue_.pop_front();
		if (!cells_.count(job.key)) { continue; }	// its cell went to another (takeCell)
		if (cells_[job.key].cell < 0)
		{
			const int cell = takeCell();	// may erase another entry: look this one up again
			cells_[job.key].cell = cell;
		}
		const Cell& c = cells_[job.key];
		PortraitJob& o = out.emplace_back();
		o.key = job.key;
		o.index = job.index;
		o.serial = c.serial;
		o.cell = c.cell;
	}
}

void PortraitTable::apply(const std::vector<PortraitDone>& done)
{
	// in reverse, so the jobs the budget left go back to the queue's front in their order
	for (auto it = done.rbegin(); it != done.rend(); ++it)
	{
		const auto found = cells_.find(it->key);
		if (found == cells_.end() || found->second.cell != it->cell) { continue; }	// its cell went to another since
		Cell& c = found->second;
		if (it->result == PortraitDone::kDrawn) { c.ready = true; }
		else if (it->result == PortraitDone::kNotPlaced) { c.dirty = true; }	// not placed yet: ask again
		else
		{
			bool queued = false;
			for (const Job& j : queue_) { queued = queued || j.key == it->key; }
			if (!queued) { queue_.push_front({ it->key, -1 }); }
		}
	}
}

void PortraitTable::clear()
{
	cells_.clear();
	queue_.clear();
	frame_ = 0;
	next_ = 0;
}

void Portraits::pose(Characters& chars, std::vector<PortraitJob>& jobs)
{
	int posed = 0;
	for (PortraitJob& job : jobs)
	{
		// the render side's order (one buffer set per portrait drawn: Characters::renderPortrait): past the frame's sets a job does not run,
		// placed or not; a job that is not placed takes no set
		job.run = posed < Characters::kMaxPortraits;
		job.posed = false;
		job.frame.clear();
		if (!job.run) { continue; }
		job.posed = chars.portraitFrame((size_t)job.index, serialYaw(job.serial), serialPitch(job.serial), job.target, job.eye, job.frame);
		if (job.posed) { posed++; }
	}
}

void Portraits::update(Characters& chars, const std::vector<PortraitJob>& jobs)
{
	std::vector<PortraitDone> done;
	done.reserve(jobs.size());
	for (const PortraitJob& job : jobs)
	{
		if (!job.run || !chars.portraitReady())	// one buffer set per portrait drawn in a frame (Characters::renderPortrait)
		{
			done.push_back({ job.key, job.cell, PortraitDone::kNotRun });
			continue;
		}
		if (!job.posed)
		{
			done.push_back({ job.key, job.cell, PortraitDone::kNotPlaced });
			continue;
		}
		const double* target = job.target;
		const double* eye = job.eye;
		const int cell = job.cell;

		// Camera::lookAt with the fixed yaw axis y; camera-relative like the main view (the eye is the origin)
		double f[3] = { target[0] - eye[0], target[1] - eye[1], target[2] - eye[2] };
		const double fl = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
		for (double& v : f) { v /= fl; }
		double r[3] = { -f[2], 0, f[0] };
		const double rl = std::sqrt(r[0] * r[0] + r[2] * r[2]);
		r[0] /= rl;
		r[2] /= rl;
		const double u[3] = { r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0] };
		float view[16] = {};
		for (int i = 0; i < 3; i++)
		{
			view[i * 4 + 0] = (float)r[i];
			view[i * 4 + 1] = (float)u[i];
			view[i * 4 + 2] = (float)-f[i];
		}
		view[15] = 1;
		const float t = 1.0f / std::tan(kFovY * 3.14159265f / 360.0f);
		float proj[16] = {};
		proj[0] = t;
		proj[5] = t;
		proj[11] = -1;
#if defined(SOKOL_VULKAN)
		proj[10] = kFar / (kNear - kFar);
		proj[14] = kFar * kNear / (kNear - kFar);
#else
		proj[10] = (kFar + kNear) / (kNear - kFar);
		proj[14] = 2 * kFar * kNear / (kNear - kFar);
#endif
		float mvp[16];
		for (int col = 0; col < 4; col++)
		{
			for (int row = 0; row < 4; row++)
			{
				float s = 0;
				for (int k = 0; k < 4; k++) { s += proj[k * 4 + row] * view[col * 4 + k]; }
				mvp[col * 4 + row] = s;
			}
		}

		sg_pass p = {};
		p.action.colors[0].load_action = SG_LOADACTION_CLEAR;
		p.action.colors[0].clear_value = { 0, 0, 0, 0 };
		p.action.depth.load_action = SG_LOADACTION_CLEAR;
		p.action.depth.clear_value = 1.0f;
		p.attachments.colors[0] = scratchAtt_;
		p.attachments.depth_stencil = depthAtt_;
		sg_begin_pass(&p);
		chars.renderPortrait(job.frame, mvp, eye, light_);
		sg_end_pass();

		p = {};
		p.action.colors[0].load_action = atlasCleared_ ? SG_LOADACTION_LOAD : SG_LOADACTION_CLEAR;
		p.action.colors[0].clear_value = { 0, 0, 0, 0 };
		p.attachments.colors[0] = atlasAtt_;
		sg_begin_pass(&p);
		sg_apply_viewport((cell % kPerRow) * kCell, (cell / kPerRow) * kCell, kCell, kCell, false);
		drawstats::pipeline(pip_);
		sg_bindings b = {};
		b.views[VIEW_scratch_tex] = scratchTex_;
		b.samplers[SMP_smp] = smp_;
		drawstats::bindings(&b);
		drawstats::draw(0, 3, 1);
		sg_end_pass();
		atlasCleared_ = true;
		done.push_back({ job.key, job.cell, PortraitDone::kDrawn });
	}
	std::lock_guard<std::mutex> lock(doneMtx_);
	done_.insert(done_.end(), done.begin(), done.end());
}
