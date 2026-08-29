// VOPI Engine
// SPDX-License-Identifier: Proprietary
// Copyright (C) 2026 VOPI Team

#if IS_VOPI_ENGINE

#include "icon_baker.h"

#include <cmath>
#include <cstring>
#include <set>
#include <vector>
#include <IVideoDriver.h>
#include <ITexture.h>
#include <IImage.h>
#include <IMesh.h>
#include <IMeshBuffer.h>
#include "client/client.h"
#include "client/renderingengine.h"
#include "client/wieldmesh.h"
#include "client/mesh.h"
#include "filesys.h"
#include "itemdef.h"
#include "itemgroup.h"
#include "nodedef.h"
#include "settings.h"
#include "util/numeric.h"
#include "util/string.h"
#include "log.h"

std::string g_dump_baked_icons_path;

// Clamp the bake resolution to power-of-two sizes: NPOT textures with
// mipmaps are not universally supported on GLES2 targets.
static u32 bakeResolution()
{
	const s32 requested = g_settings->getS32("inventory_icon_bake_resolution");
	u32 best = 32;
	for (u32 size : {32u, 64u, 128u, 256u, 512u}) {
		if ((s32)size <= requested)
			best = size;
	}
	return best;
}

/*
	Paints `color` into every transparent pixel within `radius` (euclidean,
	approximated) of an opaque pixel. Works uniformly on outer silhouettes
	and interior holes — the reason this replaced the mesh-space inverted
	hull. Implemented as a two-pass chamfer distance transform (5-7 mask,
	~2% radius error), O(w*h) regardless of the radius.
*/
static void dilateAlphaOutline(video::IImage *img, int radius, video::SColor color)
{
	const core::dimension2du dim = img->getDimension();
	const int w = dim.Width, h = dim.Height;
	u8 *data = (u8 *)img->getData();
	const u32 pitch = img->getPitch();

	// Distance map: 0 for opaque pixels, "infinity" elsewhere
	const s32 INF = 0x0FFFFFFF;
	std::vector<s32> dist((size_t)w * h);
	for (int y = 0; y < h; y++) {
		const u32 *row = (const u32 *)(data + (size_t)y * pitch);
		for (int x = 0; x < w; x++)
			dist[(size_t)y * w + x] = (row[x] >> 24) >= 128 ? 0 : INF;
	}

	// Chamfer 5-7: orthogonal step = 5, diagonal step = 7 (~5*sqrt(2))
	auto relax = [&](size_t idx, size_t nidx, s32 weight) {
		if (dist[nidx] + weight < dist[idx])
			dist[idx] = dist[nidx] + weight;
	};
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			const size_t i = (size_t)y * w + x;
			if (x > 0)
				relax(i, i - 1, 5);
			if (y > 0) {
				relax(i, i - w, 5);
				if (x > 0)
					relax(i, i - w - 1, 7);
				if (x < w - 1)
					relax(i, i - w + 1, 7);
			}
		}
	}
	for (int y = h - 1; y >= 0; y--) {
		for (int x = w - 1; x >= 0; x--) {
			const size_t i = (size_t)y * w + x;
			if (x < w - 1)
				relax(i, i + 1, 5);
			if (y < h - 1) {
				relax(i, i + w, 5);
				if (x < w - 1)
					relax(i, i + w + 1, 7);
				if (x > 0)
					relax(i, i + w - 1, 7);
			}
		}
	}

	const s32 threshold = radius * 5;
	const u32 outline = color.color | 0xFF000000;
	for (int y = 0; y < h; y++) {
		u32 *row = (u32 *)(data + (size_t)y * pitch);
		for (int x = 0; x < w; x++) {
			const s32 d = dist[(size_t)y * w + x];
			if (d > 0 && d <= threshold)
				row[x] = outline;
		}
	}
}

/*
	Copies the RGB of a neighboring opaque pixel into fully transparent
	pixels adjacent to opaque ones (alpha stays 0), so bilinear filtering
	does not blend icon edges toward black.
*/
static void bleedEdges(video::IImage *img)
{
	const core::dimension2du dim = img->getDimension();
	const int w = dim.Width, h = dim.Height;
	u8 *data = (u8 *)img->getData();
	const u32 pitch = img->getPitch();

	auto px = [&](int x, int y) -> u32 * {
		return (u32 *)(data + (size_t)y * pitch) + x;
	};

	std::vector<u8> opaque((size_t)w * h);
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++)
			opaque[(size_t)y * w + x] = (*px(x, y) >> 24) >= 128;

	static const int neigh[8][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1},
			{-1, -1}, {1, -1}, {-1, 1}, {1, 1}};
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			if (opaque[(size_t)y * w + x])
				continue;
			for (const auto &n : neigh) {
				const int nx = x + n[0], ny = y + n[1];
				if (nx < 0 || ny < 0 || nx >= w || ny >= h)
					continue;
				if (opaque[(size_t)ny * w + nx]) {
					*px(x, y) = *px(nx, ny) & 0x00FFFFFF;
					break;
				}
			}
		}
	}
}

video::IImage *bakeItemIconImage(video::IVideoDriver *driver, ItemMesh *imesh,
		u32 supersample)
{
	if (!imesh || !imesh->mesh)
		return nullptr;
	if (!driver->queryFeature(video::EVDF_RENDER_TO_TARGET))
		return nullptr;

	const u32 size = bakeResolution();
	// Render larger and box-downsample: cheap supersampling that smooths
	// mesh edges without increasing the final image size.
	const u32 render_size = std::min(size * std::max(supersample, 1u), 2048u);

	// Shared scratch RTT, reused for every bake. Looked up by name so no
	// dangling pointer is kept across texture cache clears.
	const std::string rtt_name = "__icon_bake_scratch_" + itos(render_size);
	video::ITexture *rtt = driver->findTexture(rtt_name.c_str());
	if (!rtt)
		rtt = driver->addRenderTargetTexture(
				core::dimension2du(render_size, render_size),
				rtt_name.c_str(), video::ECF_A8R8G8B8);
	if (!rtt) {
		warningstream << "bakeItemIconImage(): failed to create render target"
				<< std::endl;
		return nullptr;
	}

	const core::rect<s32> old_viewport = driver->getViewPort();
	const core::matrix4 old_proj = driver->getTransform(video::ETS_PROJECTION);
	const core::matrix4 old_view = driver->getTransform(video::ETS_VIEW);
	const core::matrix4 old_world = driver->getTransform(video::ETS_WORLD);

	if (!driver->setRenderTarget(rtt, video::ECBF_COLOR | video::ECBF_DEPTH,
			video::SColor(0, 0, 0, 0)))
		return nullptr;

	// Same ortho frame as drawItemStack's direct mesh path (createItemMesh
	// fitted the mesh into [-1,1] already), widened by the configured
	// margin so the model keeps a breathing edge inside the canvas — the
	// outline needs room and slots read better when icons do not touch
	// their borders. The draw-time image padding applies on top of this.
	const f32 bake_margin = rangelim(g_settings->getFloat(
			"inventory_icon_bake_margin_percent"), 0.0f, 25.0f);
	const f32 frame = 2.0f / (1.0f - 2.0f * bake_margin / 100.0f);
	core::matrix4 proj;
	proj.buildProjectionMatrixOrthoLH(frame, frame, -1.0f, 100.0f);
	driver->setTransform(video::ETS_PROJECTION, proj);
	driver->setTransform(video::ETS_VIEW, core::IdentityMatrix);
	driver->setTransform(video::ETS_WORLD, core::IdentityMatrix);
	driver->setViewPort(core::rect<s32>(0, 0, (s32)render_size, (s32)render_size));

	// Colorize like drawItemStack's mesh path, but with a white base color:
	// per-stack tinting is applied to the 2D quad at draw time instead of
	// being baked into the texture.
	scene::IMesh *mesh = imesh->mesh;
	const u32 mc = std::min<u32>(mesh->getMeshBufferCount(),
			imesh->buffer_info.size());
	for (u32 j = 0; j < mc; ++j) {
		scene::IMeshBuffer *buf = mesh->getMeshBuffer(j);
		video::SColor c(255, 255, 255, 255);

		auto &p = imesh->buffer_info[j];
		p.applyOverride(c);

		if (p.needColorize(c)) {
			if (imesh->needs_shading) {
				f32 ambient_light = 0.5f;
				v3f dir_light(-0.6f, -1.2f, 0.4f);
				dir_light.normalize();
				dir_light *= 0.7f;
				colorizeMeshBuffer(buf, c, ambient_light, dir_light);
			} else {
				setMeshBufferColor(buf, c);
			}
		}

		// Animated tiles: bake the first frame (icons are static)
		if (p.animation_info)
			p.animation_info->updateTexture(buf->getMaterial(), 0.0f);

		driver->setMaterial(buf->getMaterial());
		driver->drawMeshBuffer(buf);
	}

	driver->setRenderTarget(nullptr, video::ECBF_NONE);
	driver->setTransform(video::ETS_PROJECTION, old_proj);
	driver->setTransform(video::ETS_VIEW, old_view);
	driver->setTransform(video::ETS_WORLD, old_world);
	driver->setViewPort(old_viewport);

	// Read the rendered icon back (one-time cost per item; the GL path
	// handles RTT flipping and format conversion internally).
	const u8 *src = (const u8 *)rtt->lock(video::ETLM_READ_ONLY);
	if (!src) {
		warningstream << "bakeItemIconImage(): lock failed" << std::endl;
		return nullptr;
	}

	std::vector<u32> tmp((size_t)render_size * render_size);
	const u32 spitch = rtt->getPitch();
	for (u32 y = 0; y < render_size; y++)
		memcpy(&tmp[(size_t)y * render_size], src + (size_t)y * spitch,
				render_size * 4);
	rtt->unlock();

	// Premultiplied box downsample to the final size: color is the
	// alpha-weighted average, so the transparent background does not
	// darken icon edges.
	video::IImage *img = driver->createImage(video::ECF_A8R8G8B8,
			core::dimension2du(size, size));
	if (!img) {
		warningstream << "bakeItemIconImage(): image allocation failed"
				<< std::endl;
		return nullptr;
	}
	const u32 dpitch = img->getPitch();
	u8 *dst = (u8 *)img->getData();
	const u32 f = render_size / size;
	for (u32 y = 0; y < size; y++) {
		u32 *drow = (u32 *)(dst + (size_t)y * dpitch);
		for (u32 x = 0; x < size; x++) {
			u32 sum_a = 0, sum_r = 0, sum_g = 0, sum_b = 0;
			for (u32 sy = 0; sy < f; sy++)
			for (u32 sx = 0; sx < f; sx++) {
				const u32 px = tmp[(size_t)(y * f + sy) * render_size
						+ (x * f + sx)];
				const u32 a = px >> 24;
				sum_a += a;
				sum_r += ((px >> 16) & 0xFF) * a;
				sum_g += ((px >> 8) & 0xFF) * a;
				sum_b += (px & 0xFF) * a;
			}
			if (sum_a == 0) {
				drow[x] = 0;
				continue;
			}
			drow[x] = ((sum_a / (f * f)) << 24) | ((sum_r / sum_a) << 16)
					| ((sum_g / sum_a) << 8) | (sum_b / sum_a);
		}
	}

	if (g_settings->getBool("inventory_mesh_outline")) {
		const f32 percent = rangelim(
				g_settings->getFloat("inventory_mesh_outline_percent"),
				0.0f, 20.0f);
		const int radius = (int)std::lround(percent / 100.0f * size);
		video::SColor color(255, 0, 0, 0);
		parseColorString(g_settings->get("inventory_mesh_outline_color"),
				color, true);
		if (radius > 0)
			dilateAlphaOutline(img, radius, color);
	}
	bleedEdges(img);
	return img;
}

// Plain texture file name: no path separators or texture modifiers.
static bool isPlainPngName(const std::string &s)
{
	if (s.size() < 5 || !str_ends_with(s, ".png"))
		return false;
	for (char c : s) {
		const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
				|| (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
		if (!ok)
			return false;
	}
	return true;
}

bool dumpBakedIcons(Client *client, const std::string &out_dir)
{
	video::IVideoDriver *driver = RenderingEngine::get_video_driver();
	IItemDefManager *idef = client->idef();
	const NodeDefManager *ndef = client->getNodeDefManager();

	std::set<std::string> names;
	idef->getAll(names);

	u32 written = 0, errors = 0, shared = 0, candidates = 0;
	std::set<std::string> produced;

	for (const std::string &name : names) {
		// Builtin specials (air, ignore, unknown, hand) have no namespace
		const size_t colon = name.find(':');
		if (colon == std::string::npos)
			continue;

		const ItemDefinition &def = idef->get(name);
		const int rot = itemgroup_get(def.groups, "icon_bake");

		if (rot <= 0) {
			// A generic-drawtype node with neither an inventory image nor
			// the icon_bake group renders as a live 3D mesh in slots —
			// most likely an unmigrated icon, so report it. Nodes hidden
			// from players (runtime states, multiblock helpers) are not
			// icon candidates.
			if (def.type == ITEM_NODE && def.inventory_image.name.empty() &&
					itemgroup_get(def.groups,
						"not_in_creative_inventory") <= 0) {
				const ContentFeatures &f = ndef->get(name);
				if (f.drawtype != NDT_AIRLIKE &&
						f.drawtype != NDT_PLANTLIKE &&
						f.drawtype != NDT_PLANTLIKE_ROOTED) {
					actionstream << "dumpBakedIcons(): candidate without an"
							" icon (no icon_bake group): " << name
							<< std::endl;
					candidates++;
				}
			}
			continue;
		}

		auto fail = [&](const char *why) {
			errorstream << "dumpBakedIcons(): " << name << ": " << why
					<< std::endl;
			errors++;
		};

		if (def.type != ITEM_NODE) {
			fail("icon_bake is set on a non-node item");
			continue;
		}
		if (def.inventory_image.name.empty()) {
			fail("icon_bake is set but inventory_image is empty");
			continue;
		}
		if (!isPlainPngName(def.inventory_image.name)) {
			fail("inventory_image is not a plain .png file name");
			continue;
		}
		if (ndef->get(name).drawtype == NDT_AIRLIKE) {
			fail("airlike nodes have nothing to bake");
			continue;
		}
		if (!produced.insert(def.inventory_image.name).second) {
			// Several nodes may deliberately share one icon file; it is
			// baked from the alphabetically first node that declares it.
			actionstream << "dumpBakedIcons(): " << name << " reuses "
					<< def.inventory_image.name << std::endl;
			shared++;
			continue;
		}

		// Empty animations force the generic-node branch of
		// createItemMesh() even though inventory_image is declared: it
		// names the file generated here, which may not exist yet.
		AnimationInfo no_anim;
		ItemMesh imesh;
		createItemMesh(client, def, no_anim, no_anim, &imesh);
		video::IImage *img = imesh.mesh ?
				bakeItemIconImage(driver, &imesh, 4) : nullptr;

		bool ok = false;
		if (img) {
			const std::string mod_dir = out_dir + DIR_DELIM
					+ name.substr(0, colon);
			fs::CreateAllDirs(mod_dir);
			const std::string path = mod_dir + DIR_DELIM
					+ def.inventory_image.name;
			ok = driver->writeImageToFile(img, path.c_str());
			img->drop();
		}
		if (imesh.mesh)
			imesh.mesh->drop();

		if (ok)
			written++;
		else
			fail("mesh creation, bake or PNG write failed");
	}

	actionstream << "dumpBakedIcons(): wrote " << written << " icons ("
			<< shared << " shared references) into " << out_dir << ", "
			<< errors << " errors, " << candidates
			<< " unmarked candidates" << std::endl;
	actionstream << (errors ? "ICON DUMP FAILED" : "ICON DUMP OK")
			<< std::endl;
	return errors == 0;
}

#endif
