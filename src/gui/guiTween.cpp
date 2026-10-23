// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 VOPI Team

#include "guiTween.h"

#include "client/client.h"
#include "client/guiscalingfilter.h"
#include "client/item_visuals_manager.h"
#include "drawItemStack.h"
#include "settings.h"
#include "util/numeric.h"
#include <ITexture.h>
#include <IVideoDriver.h>
#include <algorithm>
#include <cmath>

namespace {

// Keeps pixel coordinates far inside s32
constexpr f32 MAX_PX = 1.0e7f;

s32 toPx(f32 v)
{
	return (s32)std::floor(std::clamp(v, -MAX_PX, MAX_PX) + 0.5f);
}

// The inset drawItemStack gives an inventory image. It adds the padding to
// the upper-left corner, takes it from the lower-right one and drops the
// fractions: on screen that is floor(padding) on one side and ceil(padding)
// on the other.
core::rect<s32> imageInset(const core::rect<s32> &rect, f32 percent)
{
	const f32 padding = (rect.getWidth() + rect.getHeight()) / 2.0f * percent / 100.0f;
	const s32 low = (s32)std::floor(padding);
	const s32 high = (s32)std::ceil(padding);
	return core::rect<s32>(rect.UpperLeftCorner.X + low, rect.UpperLeftCorner.Y + low,
			rect.LowerRightCorner.X - high, rect.LowerRightCorner.Y - high);
}

} // namespace

GUITween::GUITween(gui::IGUIEnvironment *env, gui::IGUIElement *parent, s32 id,
		const core::rect<s32> &rest, const v2s32 &unit_px, Client *client) :
	gui::IGUIElement(gui::EGUIET_ELEMENT, env, parent, id, rest),
	m_rest(rest), m_unit_px(unit_px), m_client(client),
	m_image_padding_percent(rangelim(
			g_settings->getFloat("image_icon_padding_percent"), 0.0f, 90.0f))
{
}

void GUITween::update(u64 now_ms)
{
	if (!m_start_ms)
		m_start_ms = now_ms;
	const u64 elapsed = now_ms > *m_start_ms ? now_ms - *m_start_ms : 0;
	m_state = m_spec.evaluate(elapsed);
	m_hidden = m_state.alpha <= 0.0f;

	v2s32 origin(0, 0);
	if (m_anchor) {
		// Follows the anchor wherever it is drawn, e.g. inside a scroll
		// container, and disappears with it
		if (!m_anchor->isTrulyVisible() ||
				m_anchor->getAbsoluteClippingRect().getArea() <= 0)
			m_hidden = true;
		origin = m_anchor->getAbsolutePosition().UpperLeftCorner;
		if (Parent)
			origin -= Parent->getAbsolutePosition().UpperLeftCorner;
	} else if (!m_anchor_name.empty()) {
		// The anchor is missing: there is no place to show it
		m_hidden = true;
	}
	if (m_hidden)
		return;

	// Width and height first, so that scale 1 keeps the rest size exactly
	const s32 width = toPx(m_rest.getWidth() * m_state.scale.X);
	const s32 height = toPx(m_rest.getHeight() * m_state.scale.Y);
	if (width <= 0 || height <= 0) {
		m_hidden = true;
		return;
	}
	const f32 center_x = origin.X +
			((f32)m_rest.UpperLeftCorner.X + m_rest.LowerRightCorner.X) * 0.5f +
			m_state.offset.X * m_unit_px.X;
	const f32 center_y = origin.Y +
			((f32)m_rest.UpperLeftCorner.Y + m_rest.LowerRightCorner.Y) * 0.5f +
			m_state.offset.Y * m_unit_px.Y;
	const s32 left = toPx(center_x - width * 0.5f);
	const s32 top = toPx(center_y - height * 0.5f);

	const core::rect<s32> rect(left, top, left + width, top + height);
	if (rect != RelativeRect)
		setRelativePosition(rect);
}

void GUITween::draw()
{
	if (!IsVisible || m_hidden)
		return;

	video::IVideoDriver *driver = Environment->getVideoDriver();
	const core::rect<s32> *clip = NoClip ? nullptr : &AbsoluteClippingRect;
	const core::rect<s32> &dest = AbsoluteRect;
	const core::dimension2d<s32> rest_size(m_rest.getWidth(), m_rest.getHeight());
	const u32 alpha = (u32)std::lround(m_state.alpha * 255.0f);

	if (m_item.empty()) {
		if (m_texture) {
			drawFrame(m_texture, m_state.frame, m_spec.frames, m_spec.columns,
					dest, rest_size, video::SColor(alpha, 255, 255, 255), clip);
		}
		return;
	}

	if (!m_client)
		return;

	ItemVisualsManager *visuals = m_client->getItemVisualsManager();
	video::ITexture *image = visuals->getInventoryTexture(m_item, m_client);
	if (!image) {
		// A mesh has no alpha: it is drawn opaque until alpha reaches 0
		drawItemStack(driver, nullptr, m_item, dest, clip, m_client, IT_ROT_NONE);
		return;
	}

	video::SColor color = visuals->getItemstackColor(m_item, m_client);
	color.setAlpha(color.getAlpha() * alpha / 255);
	drawFrame(image, 0, 1, 1, imageInset(dest, m_image_padding_percent),
			imageInset(core::rect<s32>(core::position2d<s32>(0, 0), rest_size),
					m_image_padding_percent).getSize(),
			color, clip);

	if (video::ITexture *overlay = visuals->getInventoryOverlayTexture(m_item, m_client)) {
		drawFrame(overlay, 0, 1, 1, dest, rest_size,
				video::SColor(alpha, 255, 255, 255), clip);
	}
}

void GUITween::drawFrame(video::ITexture *texture, u32 frame, u32 frames, u32 columns,
		const core::rect<s32> &dest, const core::dimension2d<s32> &rest_size,
		video::SColor color, const core::rect<s32> *clip)
{
	if (dest.getWidth() <= 0 || dest.getHeight() <= 0)
		return;
	const core::dimension2d<u32> size = texture->getOriginalSize();
	if (size.Width == 0 || size.Height == 0)
		return;

	video::IVideoDriver *driver = Environment->getVideoDriver();

	// The atlas grid, cut as animated_image[] cuts it: frames left to right,
	// then top to bottom, never a cell narrower than a pixel
	const u32 cols = std::clamp<u32>(columns, 1, size.Width);
	const u32 rows = std::clamp<u32>((frames + columns - 1) / columns, 1, size.Height);
	frame = std::min(frame, cols * rows - 1);
	core::dimension2d<s32> cell(size.Width / cols, size.Height / rows);

	// The GUI scaling filter scales the whole grid once, to the rest size of
	// a frame: every frame and every position then reuses one cached texture,
	// and only a scale other than 1 is left to the GPU. Scaling each frame to
	// each drawn size would cache a texture per frame and per size.
	video::ITexture *atlas = texture;
	const core::dimension2du max = driver->getMaxTextureSize();
	if (rest_size.Width > 0 && rest_size.Height > 0 &&
			(u64)rest_size.Width * cols <= max.Width &&
			(u64)rest_size.Height * rows <= max.Height) {
		video::ITexture *scaled = guiScalingResizeCached(driver, texture,
				core::rect<s32>(0, 0, cell.Width * cols, cell.Height * rows),
				core::rect<s32>(0, 0, rest_size.Width * cols, rest_size.Height * rows));
		if (scaled && scaled != texture) {
			atlas = scaled;
			cell = rest_size;
		}
	}

	const core::rect<s32> src(
			core::position2d<s32>((frame % cols) * cell.Width, (frame / cols) * cell.Height),
			cell);
	const video::SColor colors[] = {color, color, color, color};

	// At rest it is drawn as image[] draws: from the atlas scaled to the rest
	// size, pixel for pixel, or from the texture itself, sampled nearest
	if (dest.getSize() == src.getSize() || dest.getSize() == rest_size) {
		driver->draw2DImage(atlas, dest, src, clip, colors, true);
		return;
	}

	// Scaled away from rest. Nearest sampling, the 2D default, would drop or
	// double whole rows and columns of the image as the scale changes.
	video::SMaterial &material = driver->getMaterial2D();
	const video::SMaterial saved = material;
	video::SMaterialLayer &layer = material.TextureLayers[0];
	layer.MinFilter = video::ETMINF_LINEAR_MIPMAP_NEAREST;
	layer.MagFilter = video::ETMAGF_LINEAR;
	layer.TextureWrapU = video::ETC_CLAMP_TO_EDGE;
	layer.TextureWrapV = video::ETC_CLAMP_TO_EDGE;
	driver->enableMaterial2D(true);
	driver->draw2DImage(atlas, dest, src, clip, colors, true);
	driver->enableMaterial2D(false);
	material = saved;
}
