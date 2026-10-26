// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2013 celeron55, Perttu Ahola <celeron55@gmail.com>

#pragma once

#include "inventorymanager.h"
#include <IGUIElement.h>
#include <IGUIEnvironment.h>
#include "irr_v2d.h"
#if IS_VOPI_ENGINE
#include "StyleSpec.h"
#include <array>
#endif


class GUIFormSpecMenu;

class GUIInventoryList : public gui::IGUIElement
{
public:
	struct ItemSpec
	{
		ItemSpec() = default;

		ItemSpec(const InventoryLocation &a_inventoryloc,
				const std::string &a_listname,
				s32 a_i,
#if IS_VOPI_ENGINE
				const v2s32 slotsize,
				const v2s32 slot_pos) :
#else
				const v2s32 slotsize) :
#endif
			inventoryloc(a_inventoryloc),
			listname(a_listname),
			i(a_i),
#if IS_VOPI_ENGINE
			slotsize(slotsize),
			position(slot_pos)
#else
			slotsize(slotsize)
#endif
		{
		}

		bool operator==(const ItemSpec& other) const
		{
			return inventoryloc == other.inventoryloc &&
					listname == other.listname && i == other.i;
		}

		bool isValid() const { return i != -1; }

#if IS_VOPI_ENGINE
		v2s32 getAbsolutePosition() const
		{
			return position;
		}
#endif

		InventoryLocation inventoryloc;
		std::string listname;
		s32 i = -1;
		v2s32 slotsize;
#if IS_VOPI_ENGINE
		v2s32 position;
		// The list draws its own image under this slot while its stack is
		// picked (hasOwnPickedSlotImage), so the menu's shared image for a
		// selected slot is not drawn over it.
		bool own_picked_image = false;
#endif
	};

	// options for inventorylists that are setable with the lua api
	struct Options {
		// whether a one-pixel border for the slots should be drawn and its color
		bool slotborder = false;
		video::SColor slotbordercolor = video::SColor(200, 0, 0, 0);
		// colors for normal and highlighted slot background
		video::SColor slotbg_n = video::SColor(255, 128, 128, 128);
		video::SColor slotbg_h = video::SColor(255, 192, 192, 192);
	};

	GUIInventoryList(gui::IGUIEnvironment *env,
		gui::IGUIElement *parent,
		s32 id,
		const core::rect<s32> &rectangle,
		InventoryManager *invmgr,
		const InventoryLocation &inventoryloc,
		const std::string &listname,
		const v2s32 &geom,
		const s32 start_item_i,
		const v2s32 &slot_size,
		const v2f32 &slot_spacing,
		GUIFormSpecMenu *fs_menu,
		const Options &options,
		gui::IGUIFont *font);

	void draw() override;

	bool OnEvent(const SEvent &event) override;

	bool isPointInside(const core::position2d<s32> &point) const override;

	const InventoryLocation &getInventoryloc() const
	{
		return m_inventoryloc;
	}

	const std::string &getListname() const
	{
		return m_listname;
	}

	void setSlotBGColors(const video::SColor &slotbg_n, const video::SColor &slotbg_h)
	{
		m_options.slotbg_n = slotbg_n;
		m_options.slotbg_h = slotbg_h;
	}

	void setSlotBorders(bool slotborder, const video::SColor &slotbordercolor)
	{
		m_options.slotborder = slotborder;
		m_options.slotbordercolor = slotbordercolor;
	}

	const v2s32 getSlotSize() const noexcept
	{
		return m_slot_size;
	}

	// returns -1 if not item is at pos p
	s32 getItemIndexAtPos(v2s32 p) const;

#if IS_VOPI_ENGINE
	core::rect<s32> getSlotRect(s32 item_i) const;

	// Takes the slot background images (bgimg, bgimg_middle, bgimg_outset)
	// from the list's style, per state as returned by
	// GUIFormSpecMenu::getStyleForElement. imgsize: pixels per formspec
	// coordinate, as for the list's size.
	void setSlotImages(const std::array<StyleSpec, StyleSpec::NUM_STATES> &styles,
			ISimpleTextureSource *tsrc, v2s32 imgsize);

	// Whether list:pressed sets an image of its own for the slot whose stack
	// is picked, rather than inheriting the default one.
	bool hasOwnPickedSlotImage() const { return m_own_picked_image; }
#endif
private:
	InventoryManager *m_invmgr;
	const InventoryLocation m_inventoryloc;
	const std::string m_listname;

	// the specified width and height of the shown inventorylist in itemslots
	const v2s32 m_geom;
	// the first item's index in inventory
	const s32 m_start_item_i;

	// specifies how large the slot rects are
	const v2s32 m_slot_size;
	// specifies how large the space between slots is (space between is spacing-size)
	const v2f32 m_slot_spacing;

	// the GUIFormSpecMenu can have an item selected and co.
	GUIFormSpecMenu *m_fs_menu;

	Options m_options;

	// the font
	gui::IGUIFont *m_font;

	// the index of the hovered item; -1 if no item is hovered
	s32 m_hovered_i;

	// we do not want to write a warning on every draw
	bool m_already_warned;

#if IS_VOPI_ENGINE
	struct SlotImage
	{
		// nullptr: the slot is filled with its listcolors[] colour
		video::ITexture *texture = nullptr;
		// 9-slice middle rect in texture pixels; empty: scaled whole
		core::rect<s32> middle;
		// Screen pixels the image reaches past the slot: left, top, right,
		// bottom; negative draws it inside the slot.
		std::array<s32, 4> outset{};

		bool reachesPast() const
		{
			return texture && (outset[0] > 0 || outset[1] > 0 ||
					outset[2] > 0 || outset[3] > 0);
		}
	};
	// By slot state, the index of slotImageIndex()
	std::array<SlotImage, 4> m_slot_images;
	bool m_own_picked_image = false;
	// Whether the image of any state reaches past its slot
	bool m_image_reaches_past = false;

	static size_t slotImageIndex(bool hovered, bool picked)
	{
		return (hovered ? 1 : 0) | (picked ? 2 : 0);
	}

	// Layer 0 and the border of every slot, drawn before any stack
	void drawSlotImages(video::IVideoDriver *driver, Inventory *inv, s32 list_size);
	// What clips the list apart from its own rect, as
	// IGUIElement::recalculateAbsolutePosition works it out
	core::rect<s32> getParentClippingRect() const;
#endif
};
