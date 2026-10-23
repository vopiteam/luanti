// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 VOPI Team

#pragma once

#include "irrlichttypes_bloated.h"
#include "inventory.h"
#include "tweenSpec.h"
#include <IGUIElement.h>
#include <IGUIEnvironment.h>
#include <optional>
#include <string>

class Client;

namespace video
{
	class ITexture;
}

// The tween[] formspec element: a texture or an item that moves, scales and
// fades along a keyframe timeline, optionally playing a sprite animation.
// It never takes input. GUIFormSpecMenu calls update() once per frame before
// drawing, so that the element's rect is its animated rect and the usual
// culling and clipping apply to it.
class GUITween : public gui::IGUIElement
{
public:
	// `rest` is the rect at rest, relative to the parent, or to the anchor
	// when one is set.
	GUITween(gui::IGUIEnvironment *env, gui::IGUIElement *parent, s32 id,
			const core::rect<s32> &rest, const v2s32 &unit_px, Client *client);

	void setSpec(TweenSpec &&spec) { m_spec = std::move(spec); }
	void setTexture(video::ITexture *texture) { m_texture = texture; }
	void setItem(const ItemStack &item) { m_item = item; }

	const std::string &getAnchorName() const { return m_anchor_name; }
	void setAnchorName(const std::string &name) { m_anchor_name = name; }
	// The element whose top-left corner the rest rect is relative to. It must
	// live as long as this element: both belong to one formspec.
	void setAnchor(gui::IGUIElement *anchor) { m_anchor = anchor; }

	// Time the animation started, unset until the first update()
	std::optional<u64> getStartTime() const { return m_start_ms; }
	void setStartTime(u64 start_ms) { m_start_ms = start_ms; }

	// Places the element at its state at `now_ms`.
	void update(u64 now_ms);

	void draw() override;

private:
	void drawFrame(video::ITexture *texture, u32 frame, u32 frames, u32 columns,
			const core::rect<s32> &dest, const core::dimension2d<s32> &rest_size,
			video::SColor color, const core::rect<s32> *clip);

	core::rect<s32> m_rest;
	v2s32 m_unit_px;
	Client *m_client;
	f32 m_image_padding_percent;

	TweenSpec m_spec;
	video::ITexture *m_texture = nullptr;
	ItemStack m_item;

	std::string m_anchor_name;
	gui::IGUIElement *m_anchor = nullptr;

	std::optional<u64> m_start_ms;
	TweenState m_state;
	bool m_hidden = false;
};
