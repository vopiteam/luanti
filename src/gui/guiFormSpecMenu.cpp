// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2013 celeron55, Perttu Ahola <celeron55@gmail.com>


#include <cstdlib>
#include <algorithm>
#include <iterator>
#include <limits>
#include "guiFormSpecMenu.h"
#include "EGUIElementTypes.h"
#include "itemdef.h"
#include "gamedef.h"
#include "client/keycode.h"
#include "gui/guiTable.h"
#include <IGUIButton.h>
#include <IGUICheckBox.h>
#include <IGUIComboBox.h>
#include <IGUIEditBox.h>
#include <IGUIFont.h>
#include <IGUITabControl.h>
#include <IGUIImage.h>
#include <AnimatedMeshSceneNode.h>
#include "client/renderingengine.h"
#include "client/joystick_controller.h"
#include "log.h"
#include "drawItemStack.h"
#include "gettext.h"
#include "mainmenumanager.h"
#include "porting.h"
#include "settings.h"
#include "client/client.h"
#include "client/fontengine.h"
#include "client/sound.h"
#include "util/numeric.h"
#include "util/screenshot.h"
#include "util/string.h" // for parseColorString()
#include "irrlicht_changes/static_text.h"
#include "client/guiscalingfilter.h"
#include "guiAnimatedImage.h"
#include "guiBackgroundImage.h"
#include "guiBox.h"
#include "guiButton.h"
#include "guiButtonImage.h"
#include "guiButtonItemImage.h"
#include "guiButtonKey.h"
#include "guiCheckBox.h"
#include "guiEditBoxWithScrollbar.h"
#include "guiInventoryList.h"
#include "guiItemImage.h"
#include "guiMapElement.h"
#include "guiScrollContainer.h"
#include "guiHyperText.h"
#include "guiScene.h"

#define MY_CHECKPOS(a,b)													\
	if (v_pos.size() != 2) {												\
		errorstream<< "Invalid pos for element " << a << " specified: \""	\
			<< parts[b] << "\"" << std::endl;								\
			return;															\
	}

#define MY_CHECKGEOM(a,b)													\
	if (v_geom.size() != 2) {												\
		errorstream<< "Invalid geometry for element " << a <<				\
			" specified: \"" << parts[b] << "\"" << std::endl;				\
			return;															\
	}

#define MY_CHECKCLIENT(a) \
	if (!m_client) { \
		errorstream << "Attempted to use element " << a << " with m_client == nullptr." << std::endl; \
		return; \
	}

// Element ID of the "Proceed" button shown for sizeless formspecs
constexpr s32 ID_PROCEED_BTN = 257;

/*
	GUIFormSpecMenu
*/
static unsigned int font_line_height(gui::IGUIFont *font)
{
	return font->getDimension(L"Ay").Height + font->getKerning(L'A').Y;
}

gui::IGUIFont *GUIFormSpecMenu::getScaledDefaultFont() const
{
	if (m_font_scale != 1.0f) {
		unsigned base_size = g_fontengine->getFontSize(FM_Standard);
		FontSpec spec((unsigned)std::round(base_size * m_font_scale),
			FM_Standard, false, false);
		return g_fontengine->getFont(spec);
	}
	return g_fontengine->getFont();
}

gui::IGUIFont *GUIFormSpecMenu::getScaledStyleFont(const StyleSpec &style) const
{
	gui::IGUIFont *sf = style.getFont(m_font_scale);
	return sf ? sf : getScaledDefaultFont();
}

gui::IGUIFont *GUIFormSpecMenu::getScaledTooltipFont() const
{
#if IS_VOPI_ENGINE
	// VOPI: tooltips otherwise use m_font (the imgsize-scaled UI font), which is
	// oversized on fullscreen formspecs. Render the tooltip at a fraction of that
	// size so hint popups stay smaller than the surrounding UI. The main menu and
	// the in-game formspecs have a different imgsize, so they need separate ratios
	// to look right: use the in-game ratio when a Client exists (inventory/craft),
	// the menu ratio otherwise. Falls back to m_font when font scaling is inactive
	// (m_font_scale == 1, e.g. the upstream path).
	if (m_font_scale != 1.0f) {
		const float ratio = m_client ? VOPI_TOOLTIP_FONT_RATIO_INGAME
		                             : VOPI_TOOLTIP_FONT_RATIO;
		const unsigned base_size = g_fontengine->getFontSize(FM_Standard);
		FontSpec spec((unsigned)std::round(base_size * m_font_scale * ratio),
			FM_Standard, false, false);
		return g_fontengine->getFont(spec);
	}
#endif
	return m_font;
}

inline u32 clamp_u8(s32 value)
{
	return (u32) MYMIN(MYMAX(value, 0), 255);
}

GUIFormSpecMenu::GUIFormSpecMenu(JoystickController *joystick,
		gui::IGUIElement *parent, s32 id, IMenuManager *menumgr,
		Client *client, gui::IGUIEnvironment *guienv, ISimpleTextureSource *tsrc,
		ISoundManager *sound_manager, IFormSource *fsrc, TextDest *tdst,
		const std::string &formspecPrepend, bool remap_dbl_click):
	GUIModalMenu(guienv, parent, id, menumgr, remap_dbl_click),
	m_invmgr(client),
	m_tsrc(tsrc),
	m_sound_manager(sound_manager),
	m_client(client),
	m_formspec_prepend(formspecPrepend),
	m_form_src(fsrc),
	m_text_dst(tdst),
	m_joystick(joystick)
{
	current_keys_pending.key_down = false;
	current_keys_pending.key_up = false;
	current_keys_pending.key_enter = false;

	m_tooltip_show_delay = (u32)g_settings->getS32("tooltip_show_delay");
	m_tooltip_append_itemname = g_settings->getBool("tooltip_append_itemname");
}

GUIFormSpecMenu::~GUIFormSpecMenu()
{
	removeAll();

	delete m_form_src;
	delete m_text_dst;
}

void GUIFormSpecMenu::create(GUIFormSpecMenu *&cur_formspec, Client *client,
	gui::IGUIEnvironment *guienv, JoystickController *joystick, IFormSource *fs_src,
	TextDest *txt_dest, const std::string &formspecPrepend, ISoundManager *sound_manager)
{
	if (cur_formspec && (cur_formspec->getReferenceCount() == 1
			|| cur_formspec->getParent() == nullptr)) {
		/*
			Drop the formspec if:
			- refcount == 1: no parent, no focus, only our reference remains
			- parent == nullptr: removed from GUI tree (e.g. by quitMenu) but
			  Irrlicht internals may still hold extra references keeping
			  refcount > 1. Reusing such an orphaned formspec would leave it
			  invisible since it's not in the GUI tree.
		*/
		cur_formspec->drop();
		cur_formspec = nullptr;
	}

	if (cur_formspec == nullptr) {
		cur_formspec = new GUIFormSpecMenu(joystick, guiroot, -1, &g_menumgr,
			client, guienv, client->getTextureSource(), sound_manager, fs_src,
			txt_dest, formspecPrepend);

		/*
			Caution: do not call (*cur_formspec)->drop() here --
			the reference might outlive the menu, so we will
			periodically check if *cur_formspec is the only
			remaining reference (i.e. the menu was removed)
			and delete it in that case.
		*/
	} else {
		cur_formspec->setFormspecPrepend(formspecPrepend);
		cur_formspec->setFormSource(fs_src);
		cur_formspec->setTextDest(txt_dest);
	}

	cur_formspec->doPause = false;
}

#if IS_VOPI_ENGINE && (defined(__ANDROID__) || defined(__IOS__))
void GUIFormSpecMenu::removeItemSelectBackground()
{
	if (m_selected_item_bg) {
		m_selected_item_bg->remove();
		m_selected_item_bg->drop();
		m_selected_item_bg = nullptr;
	}
}
#endif

void GUIFormSpecMenu::removeTooltip()
{
	if (m_tooltip_element) {
		m_tooltip_element->remove();
		m_tooltip_element->drop();
		m_tooltip_element = nullptr;
	}

#if IS_VOPI_ENGINE
	m_tooltip_bg.remove();
#endif
}

void GUIFormSpecMenu::setInitialFocus()
{
	if (m_held_mouse_button != BET_OTHER) {
		// Ongoing inventory list interaction (they have no `FieldSpec::fname` to focus).
		Environment->setFocus(this);
		return;
	}

	// Set initial focus according to following order of precedence:
	// 1. first empty editbox
	// 2. first editbox
	// 3. first table
	// 4. last button
	// 5. first focusable (not statictext, not tabheader)
	// 6. first child element

	const auto& children = getChildren();

	// 1. first empty editbox
	for (gui::IGUIElement *it : children) {
		if (it->getType() == gui::EGUIET_EDIT_BOX
				&& it->getText()[0] == 0) {
			Environment->setFocus(it);
			return;
		}
	}

	// 2. first editbox
	for (gui::IGUIElement *it : children) {
		if (it->getType() == gui::EGUIET_EDIT_BOX) {
			Environment->setFocus(it);
			return;
		}
	}

	// 3. first table
	for (gui::IGUIElement *it : children) {
		if (it->getType() == gui::EGUIET_TABLE) {
			Environment->setFocus(it);
			return;
		}
	}

	// 4. last button
	for (auto it = children.rbegin(); it != children.rend(); ++it) {
		if ((*it)->getType() == gui::EGUIET_BUTTON) {
			Environment->setFocus(*it);
			return;
		}
	}

	// 5. first focusable (not statictext, not tabheader)
	for (gui::IGUIElement *it : children) {
		if (it->getType() != gui::EGUIET_STATIC_TEXT &&
			it->getType() != gui::EGUIET_TAB_CONTROL) {
			Environment->setFocus(it);
			return;
		}
	}

	// 6. first child element
	if (children.empty())
		Environment->setFocus(this);
	else
		Environment->setFocus(children.front());
}

GUITable* GUIFormSpecMenu::getTable(const std::string &tablename)
{
	for (auto &table : m_tables) {
		if (tablename == table.first.fname)
			return table.second;
	}
	return 0;
}

std::vector<std::string>* GUIFormSpecMenu::getDropDownValues(const std::string &name)
{
	for (auto &dropdown : m_dropdowns) {
		if (name == dropdown.first.fname)
			return &dropdown.second;
	}
	return NULL;
}

// This will only return a meaningful value if called after drawMenu().
core::rect<s32> GUIFormSpecMenu::getAbsoluteRect()
{
	core::rect<s32> rect = AbsoluteRect;
	rect.UpperLeftCorner.Y += m_tabheader_upper_edge;
	return rect;
}

v2s32 GUIFormSpecMenu::getElementBasePos(const std::vector<std::string> *v_pos)
{
	v2f32 pos_f = v2f32(padding.X, padding.Y) + pos_offset * spacing;
	if (v_pos) {
		pos_f.X += stof((*v_pos)[0]) * spacing.X;
		pos_f.Y += stof((*v_pos)[1]) * spacing.Y;
	}
	return v2s32(pos_f.X, pos_f.Y);
}

v2s32 GUIFormSpecMenu::getRealCoordinateBasePos(const std::vector<std::string> &v_pos)
{
	return v2s32((stof(v_pos[0]) + pos_offset.X) * imgsize.X,
		(stof(v_pos[1]) + pos_offset.Y) * imgsize.Y);
}

v2s32 GUIFormSpecMenu::getRealCoordinateGeometry(const std::vector<std::string> &v_geom)
{
	return v2s32(stof(v_geom[0]) * imgsize.X, stof(v_geom[1]) * imgsize.Y);
}

bool GUIFormSpecMenu::precheckElement(const std::string &name, const std::string &element,
	size_t args_min, size_t args_max, std::vector<std::string> &parts)
{
	parts = split(element, ';');
	if (parts.size() >= args_min && (parts.size() <= args_max || m_formspec_version > FORMSPEC_API_VERSION))
		return true;

	errorstream << "Invalid " << name << " element(" << parts.size() << "): '" << element << "'" << std::endl;
	return false;
}

void GUIFormSpecMenu::parseSize(parserData* data, const std::string &element)
{
	// Note: do not use precheckElement due to "," separator.
	std::vector<std::string> parts = split(element,',');

	if (((parts.size() == 2) || parts.size() == 3) ||
		((parts.size() > 3) && (m_formspec_version > FORMSPEC_API_VERSION)))
	{
		if (parts[1].find(';') != std::string::npos)
			parts[1] = parts[1].substr(0,parts[1].find(';'));

		data->invsize.X = MYMAX(0, stof(parts[0]));
		data->invsize.Y = MYMAX(0, stof(parts[1]));

		lockSize(false);
		if (!g_settings->getBool("touch_gui") && parts.size() == 3) {
			if (parts[2] == "true") {
				lockSize(true,v2u32(800,600));
			}
		}
		data->explicit_size = true;
		return;
	}
	errorstream<< "Invalid size element (" << parts.size() << "): '" << element << "'"  << std::endl;
}

void GUIFormSpecMenu::parseContainer(parserData* data, const std::string &element)
{
	std::vector<std::string> parts = split(element, ',');

	if (parts.size() >= 2) {
		if (parts[1].find(';') != std::string::npos)
			parts[1] = parts[1].substr(0, parts[1].find(';'));

		container_stack.push(pos_offset);
		pos_offset.X += stof(parts[0]);
		pos_offset.Y += stof(parts[1]);
		return;
	}
	errorstream<< "Invalid container start element (" << parts.size() << "): '" << element << "'"  << std::endl;
}

void GUIFormSpecMenu::parseContainerEnd(parserData* data, const std::string &)
{
	if (container_stack.empty()) {
		errorstream<< "Invalid container end element, no matching container start element"  << std::endl;
	} else {
		pos_offset = container_stack.top();
		container_stack.pop();
	}
}

void GUIFormSpecMenu::parseScrollContainer(parserData *data, const std::string &element)
{
	std::vector<std::string> parts;
	if (!precheckElement("scroll_container start", element, 4, 6, parts))
		return;

	std::vector<std::string> v_pos  = split(parts[0], ',');
	std::vector<std::string> v_geom = split(parts[1], ',');
	std::string scrollbar_name = parts[2];
	std::string orientation = parts[3];
	f32 scroll_factor = 0.1f;
	if (parts.size() >= 5 && !parts[4].empty())
		scroll_factor = stof(parts[4]);

	std::optional<s32> content_padding_px;
	if (parts.size() >= 6 && !parts[5].empty()) {
		std::vector<std::string> v_size = { parts[5], parts[5] };
		content_padding_px = getRealCoordinateGeometry(v_size)[orientation == "vertical" ? 1 : 0];
	}

	MY_CHECKPOS("scroll_container", 0);
	MY_CHECKGEOM("scroll_container", 1);

	v2s32 pos = getRealCoordinateBasePos(v_pos);
	v2s32 geom = getRealCoordinateGeometry(v_geom);

	if (orientation == "vertical")
		scroll_factor *= -imgsize.Y;
	else if (orientation == "horizontal")
		scroll_factor *= -imgsize.X;
	else
		warningstream << "GUIFormSpecMenu::parseScrollContainer(): "
				<< "Invalid scroll_container orientation: " << orientation
				<< std::endl;

	// old parent (at first: this)
	// ^ is parent of clipper
	// ^ is parent of mover
	// ^ is parent of other elements

	// make clipper
	core::rect<s32> rect_clipper = core::rect<s32>(pos, pos + geom);

	gui::IGUIElement *clipper = new gui::IGUIElement(EGUIET_ELEMENT, Environment,
			data->current_parent, 0, rect_clipper);

	// make mover
	FieldSpec spec_mover(
		"",
		L"",
		L"",
		258 + m_fields.size()
	);

	core::rect<s32> rect_mover = core::rect<s32>(0, 0, geom.X, geom.Y);

	GUIScrollContainer *mover = new GUIScrollContainer(Environment,
			clipper, spec_mover.fid, rect_mover, orientation, scroll_factor);
	mover->setContentPadding(content_padding_px);

	data->current_parent = mover;

	m_scroll_containers.emplace_back(scrollbar_name, mover);

	m_fields.push_back(spec_mover);

	clipper->drop();

	// remove interferring offset of normal containers
	container_stack.push(pos_offset);
	pos_offset.X = 0.0f;
	pos_offset.Y = 0.0f;
}

void GUIFormSpecMenu::parseScrollContainerEnd(parserData *data, const std::string &)
{
	if (data->current_parent == this || data->current_parent->getParent() == this ||
			container_stack.empty()) {
		errorstream << "Invalid scroll_container end element, "
				<< "no matching scroll_container start element" << std::endl;
		return;
	}

	if (pos_offset.getLengthSQ() != 0.0f) {
		// pos_offset is only set by containers and scroll_containers.
		// scroll_containers always set it to 0,0 which means that if it is
		// not 0,0, it is a normal container that was opened last, not a
		// scroll_container
		errorstream << "Invalid scroll_container end element, "
				<< "an inner container was left open" << std::endl;
		return;
	}

	data->current_parent = data->current_parent->getParent()->getParent();
	pos_offset = container_stack.top();
	container_stack.pop();
}

void GUIFormSpecMenu::parseList(parserData *data, const std::string &element)
{
	MY_CHECKCLIENT("list");

	std::vector<std::string> parts;
	if (!precheckElement("list", element, 4, 5, parts))
		return;

	std::string location = parts[0];
	std::string listname = parts[1];
	std::vector<std::string> v_pos  = split(parts[2],',');
	std::vector<std::string> v_geom = split(parts[3],',');
	std::string startindex;
	if (parts.size() == 5)
		startindex = parts[4];

	MY_CHECKPOS("list",2);
	MY_CHECKGEOM("list",3);

	InventoryLocation loc;

	if (location == "context" || location == "current_name")
		loc = m_current_inventory_location;
	else
		loc.deSerialize(location);

	v2s32 geom;
	geom.X = stoi(v_geom[0]);
	geom.Y = stoi(v_geom[1]);

	s32 start_i = 0;
	if (!startindex.empty())
		start_i = stoi(startindex);

	if (geom.X < 0 || geom.Y < 0 || start_i < 0) {
		errorstream << "Invalid list element: '" << element << "'"  << std::endl;
		return;
	}

	if (!data->explicit_size)
		warningstream << "invalid use of list without a size[] element" << std::endl;

	FieldSpec spec(
		"",
		L"",
		L"",
		258 + m_fields.size(),
		3
	);

	auto style = getDefaultStyleForElement("list", spec.fname);

	v2f32 slot_scale = style.getVector2f(StyleSpec::SIZE, v2f32(0, 0));
	v2f32 slot_size(
		slot_scale.X <= 0 ? imgsize.X : std::max<f32>(slot_scale.X * imgsize.X, 1),
		slot_scale.Y <= 0 ? imgsize.Y : std::max<f32>(slot_scale.Y * imgsize.Y, 1)
	);

	v2f32 slot_spacing = style.getVector2f(StyleSpec::SPACING, v2f32(-1, -1));
	v2f32 default_spacing = data->real_coordinates ?
			v2f32(imgsize.X * 0.25f, imgsize.Y * 0.25f) :
			v2f32(spacing.X - imgsize.X, spacing.Y - imgsize.Y);

	slot_spacing.X = slot_spacing.X < 0 ? default_spacing.X :
			imgsize.X * slot_spacing.X;
	slot_spacing.Y = slot_spacing.Y < 0 ? default_spacing.Y :
			imgsize.Y * slot_spacing.Y;

	slot_spacing += slot_size;

	v2s32 pos = data->real_coordinates ? getRealCoordinateBasePos(v_pos) :
			getElementBasePos(&v_pos);

	core::rect<s32> rect = core::rect<s32>(pos.X, pos.Y,
			pos.X + (geom.X - 1) * slot_spacing.X + slot_size.X,
			pos.Y + (geom.Y - 1) * slot_spacing.Y + slot_size.Y);

	GUIInventoryList *e = new GUIInventoryList(Environment, data->current_parent,
			spec.fid, rect, m_invmgr, loc, listname, geom, start_i,
			v2s32(slot_size.X, slot_size.Y), slot_spacing, this,
			data->inventorylist_options, m_font);

	e->setNotClipped(style.getBool(StyleSpec::NOCLIP, false));

	m_inventorylists.push_back(e);
	m_fields.push_back(spec);
}

void GUIFormSpecMenu::parseListRing(parserData *data, const std::string &element)
{
	MY_CHECKCLIENT("listring");

	std::vector<std::string> parts = split(element, ';');

	if (parts.size() == 2) {
		std::string location = parts[0];
		std::string listname = parts[1];

		InventoryLocation loc;

		if (location == "context" || location == "current_name")
			loc = m_current_inventory_location;
		else
			loc.deSerialize(location);

		m_inventory_rings.emplace_back(loc, listname);
		return;
	}

	if (element.empty() && m_inventorylists.size() > 1) {
		size_t siz = m_inventorylists.size();
		// insert the last two inv list elements into the list ring
		const GUIInventoryList *spa = m_inventorylists[siz - 2];
		const GUIInventoryList *spb = m_inventorylists[siz - 1];
		m_inventory_rings.emplace_back(spa->getInventoryloc(), spa->getListname());
		m_inventory_rings.emplace_back(spb->getInventoryloc(), spb->getListname());
		return;
	}

	errorstream<< "Invalid list ring element(" << parts.size() << ", "
		<< m_inventorylists.size() << "): '" << element << "'"  << std::endl;
}

void GUIFormSpecMenu::parseCheckbox(parserData* data, const std::string &element)
{
	std::vector<std::string> parts;
#if IS_VOPI_ENGINE
	// VOPI: optional params — 5 & 6: custom unchecked/checked textures; 7: align
	// (left|center|right, anchors the box+label group on pos.X); 8: box size
	// (coordinate units, overrides the skin checkbox width); 9: label vertical
	// nudge (coordinate units, + = down). Label colour/font via style[<name>;...].
	if (!precheckElement("checkbox", element, 3, 9, parts))
		return;
#else
	if (!precheckElement("checkbox", element, 3, 4, parts))
		return;
#endif

	std::vector<std::string> v_pos = split(parts[0],',');
	std::string name = parts[1];
	std::string label = parts[2];
	std::string selected;

	if (parts.size() >= 4)
		selected = parts[3];

	MY_CHECKPOS("checkbox",0);

	bool fselected = false;

	if (selected == "true")
		fselected = true;

	// Style fetched EARLY so the label is MEASURED with the same font it will be
	// DRAWN with (font_size-scaled) — keeps the rect / centering exact.
	auto style = getDefaultStyleForElement("checkbox", name);
#if IS_VOPI_ENGINE
	// Use the style font scaled by the formspec font scale (m_font_scale), so the
	// checkbox label tracks the UI size like every other formspec font. Both the
	// measurement below and the GUICheckBox override font use cb_font, so the
	// label rect and the drawn text stay in sync. getScaledStyleFont falls back
	// to the scaled default font, so cb_font is never null.
	gui::IGUIFont *cb_font = getScaledStyleFont(style);
#else
	gui::IGUIFont *cb_font = m_font;
#endif

	std::wstring wlabel = translate_string(utf8_to_wide(unescape_string(label)));
	const core::dimension2d<u32> label_size = cb_font->getDimension(wlabel.c_str());
	s32 cb_size = Environment->getSkin()->getSize(gui::EGDS_CHECK_BOX_WIDTH);
#if IS_VOPI_ENGINE
	// VOPI: optional box size (field 8) in coordinate units → px. Overrides the
	// skin width for layout (rect / y_center / centering) AND the drawn box
	// (passed to GUICheckBox below), so both stay in sync.
	if (parts.size() >= 8 && !parts[7].empty()) {
		// Clamp: box size is untrusted formspec input; cap it so a huge value
		// can't drive a multi-million-px rescale (OOM/DoS) or overflow the cast.
		const f32 box_units = rangelim(stof(parts[7]), 0.0f, 20.0f);
		if (box_units > 0.0f)
			cb_size = (s32)(box_units * (f32)imgsize.Y);
	}
	// VOPI: optional label vertical nudge (field 9) in coordinate units → px
	// (+ = down). Lets the label line up with the box despite font metrics.
	s32 cb_text_voffset = 0;
	if (parts.size() >= 9 && !parts[8].empty())
		// Clamp: untrusted; bound the nudge so a large value can't overflow the cast.
		cb_text_voffset = (s32)(rangelim(stof(parts[8]), -20.0f, 20.0f) * (f32)imgsize.Y);
#endif
	s32 y_center = (std::max(label_size.Height, (u32)cb_size) + 1) / 2;

	v2s32 pos;
	core::rect<s32> rect;

	// VOPI: total group width (box + 7px gap + measured label). The label width
	// is known here (client-side font metric), so we can anchor the whole group
	// left (default), centered, or right on pos.X via the optional `align` field.
	const s32 cb_total_w = label_size.Width + cb_size + 7;
#if IS_VOPI_ENGINE
	const std::string cb_align = parts.size() >= 7 ? parts[6] : "left";
	auto cb_anchor_x = [&](s32 px) -> s32 {
		if (cb_align == "center") return px - cb_total_w / 2;
		if (cb_align == "right")  return px - cb_total_w;
		return px;
	};
#endif

	if (data->real_coordinates) {
		pos = getRealCoordinateBasePos(v_pos);
#if IS_VOPI_ENGINE
		const s32 x0 = cb_anchor_x(pos.X);
#else
		const s32 x0 = pos.X;
#endif
		rect = core::rect<s32>(
				x0,
				pos.Y - y_center,
				x0 + cb_total_w,
				pos.Y + y_center
			);
	} else {
		pos = getElementBasePos(&v_pos);
#if IS_VOPI_ENGINE
		const s32 x0 = cb_anchor_x(pos.X);
#else
		const s32 x0 = pos.X;
#endif
		rect = core::rect<s32>(
				x0,
				pos.Y + imgsize.Y / 2 - y_center,
				x0 + cb_total_w,
				pos.Y + imgsize.Y / 2 + y_center
			);
	}

	FieldSpec spec(
			name,
			wlabel, //Needed for displaying text on MSVC
			wlabel,
			258+m_fields.size()
		);

	spec.ftype = f_CheckBox;

	gui::IGUICheckBox *e;
#if IS_VOPI_ENGINE
	// VOPI: always use GUICheckBox so the style label font/colour, box-size and
	// vertical-nudge apply uniformly — whether or not custom textures are given.
	// Optional fields 5 & 6 supply unchecked/checked textures (both required to
	// replace the skin box); without them GUICheckBox renders the default box.
	std::string tex_unchecked = parts.size() >= 5 ? unescape_string(parts[4]) : "";
	std::string tex_checked   = parts.size() >= 6 ? unescape_string(parts[5]) : "";
	{
		GUICheckBox *ce = new GUICheckBox(fselected, Environment,
				data->current_parent, spec.fid, rect);
		ce->setText(spec.flabel.c_str());
		if (!tex_unchecked.empty() && !tex_checked.empty()) {
			video::ITexture *tu = m_tsrc->getTexture(tex_unchecked);
			video::ITexture *tc = m_tsrc->getTexture(tex_checked);
			if (!tu)
				errorstream << "checkbox[]: unable to load texture: "
						<< tex_unchecked << std::endl;
			if (!tc)
				errorstream << "checkbox[]: unable to load texture: "
						<< tex_checked << std::endl;
			ce->setImages(tu, tc);
		}
		ce->setBoxSize(cb_size);   // skin width, or the field-8 override above
		ce->setOverrideFont(cb_font);  // font_size-scaled label font (style)
		ce->setTextVOffset(cb_text_voffset);  // field-9 vertical nudge
		// Label colour via style textcolor; default = skin button-text colour,
		// so an unset textcolor keeps stock appearance.
		ce->setOverrideColor(style.getColor(StyleSpec::TEXTCOLOR,
				Environment->getSkin()->getColor(gui::EGDC_BUTTON_TEXT)));
		ce->drop();
		e = ce;
	}
#else
	e = Environment->addCheckBox(fselected, rect,
			data->current_parent, spec.fid, spec.flabel.c_str());
#endif

	spec.sound = style.get(StyleSpec::Property::SOUND, "");

	e->setNotClipped(style.getBool(StyleSpec::NOCLIP, false));

	if (spec.fname == m_focused_element) {
		Environment->setFocus(e);
	}

	e->grab();
	m_checkboxes.emplace_back(spec, e);
	m_fields.push_back(spec);
}

void GUIFormSpecMenu::parseRealCoordinates(parserData* data, const std::string &element)
{
	data->real_coordinates = is_yes(element);
}

#if IS_VOPI_ENGINE
void GUIFormSpecMenu::parseScrollBar(parserData* data, const std::string &element)
{
	std::vector<std::string> parts = split(element,';');

	if (parts.size() >= 5) {
		std::vector<std::string> v_pos = split(parts[0],',');
		std::vector<std::string> v_geom = split(parts[1],',');
		std::string name = parts[3];
		std::string value = parts[4];
		std::vector<std::string> textures;

		if (parts.size() == 6)
			textures = split(parts[5], ',');

		MY_CHECKPOS("scrollbar",0);
		MY_CHECKGEOM("scrollbar",1);

		v2s32 pos;
		v2s32 dim;

		if (data->real_coordinates) {
			pos = getRealCoordinateBasePos(v_pos);
			dim = getRealCoordinateGeometry(v_geom);
		} else {
			pos = getElementBasePos(&v_pos);
			dim.X = stof(v_geom[0]) * spacing.X;
			dim.Y = stof(v_geom[1]) * spacing.Y;
		}

		core::rect<s32> rect =core::rect<s32>(pos.X, pos.Y, pos.X + dim.X, pos.Y + dim.Y);

		FieldSpec spec(
			name,
			L"",
			L"",
			258+m_fields.size()
		);

		bool is_horizontal = true;

		if (parts[2] == "vertical")
			is_horizontal = false;

		spec.ftype = f_ScrollBar;
		spec.send  = true;
		GUIScrollBar *e = new GUIScrollBar(Environment, data->current_parent,
				   spec.fid, rect, is_horizontal, m_tsrc);

		auto style = getDefaultStyleForElement("scrollbar", name);
		e->setNotClipped(style.getBool(StyleSpec::NOCLIP, false));
		e->setArrowsVisible(data->scrollbar_options.arrow_visiblity);

		s32 max = data->scrollbar_options.max;
		s32 min = data->scrollbar_options.min;

		e->setMax(max);
		e->setMin(min);

		spec.aux_f32 = stoi(value);
		e->setPos(spec.aux_f32);

		e->setSmallStep(data->scrollbar_options.small_step);
		e->setLargeStep(data->scrollbar_options.large_step);

		s32 scrollbar_size = is_horizontal ? dim.X : dim.Y;

		e->setPageSize(scrollbar_size * (max - min + 1) / data->scrollbar_options.thumb_size);

		std::vector<video::ITexture *> itextures;

		// The custom-texture draw path indexes [0..3] (plus [4],[5] for a
		// 3-part thumb, guarded by size); a partial set from a served formspec
		// would read past the vector. Require the full bg/thumb/top/bottom set,
		// otherwise fall back to the style[]/stock textures.
		if (textures.size() < 4) {
			e->setStyle(style, m_tsrc);
		} else {
			for (u32 i = 0; i < textures.size(); ++i)
				itextures.push_back(m_tsrc->getTexture(textures[i]));
			e->setTextures(itextures);
		}
		if (spec.fname == m_focused_element) {
			Environment->setFocus(e);
		}
		m_scrollbars.emplace_back(spec,e);
		m_fields.push_back(spec);
		return;
	}
	errorstream << "Invalid scrollbar element(" << parts.size() << "): '" << element
		<< "'" << std::endl;
}
#else
void GUIFormSpecMenu::parseScrollBar(parserData* data, const std::string &element)
{
	std::vector<std::string> parts;
	if (!precheckElement("scrollbar", element, 5, 5, parts))
		return;

	std::vector<std::string> v_pos = split(parts[0],',');
	std::vector<std::string> v_geom = split(parts[1],',');
	std::string name = parts[3];
	std::string value = parts[4];

	MY_CHECKPOS("scrollbar",0);
	MY_CHECKGEOM("scrollbar",1);

	v2s32 pos;
	v2s32 dim;

	if (data->real_coordinates) {
		pos = getRealCoordinateBasePos(v_pos);
		dim = getRealCoordinateGeometry(v_geom);
	} else {
		pos = getElementBasePos(&v_pos);
		dim.X = stof(v_geom[0]) * spacing.X;
		dim.Y = stof(v_geom[1]) * spacing.Y;
	}

	core::rect<s32> rect =
			core::rect<s32>(pos.X, pos.Y, pos.X + dim.X, pos.Y + dim.Y);

	FieldSpec spec(
			name,
			L"",
			L"",
			258+m_fields.size()
		);

	bool is_horizontal = true;

	if (parts[2] == "vertical")
		is_horizontal = false;

	spec.ftype = f_ScrollBar;
	spec.send  = true;
	GUIScrollBar *e = new GUIScrollBar(Environment, data->current_parent,
			spec.fid, rect, is_horizontal, m_tsrc);

	auto style = getDefaultStyleForElement("scrollbar", name);
	e->setNotClipped(style.getBool(StyleSpec::NOCLIP, false));
	e->setArrowsVisible(data->scrollbar_options.arrow_visiblity);

	s32 max = data->scrollbar_options.max;
	s32 min = data->scrollbar_options.min;

	e->setMax(max);
	e->setMin(min);

	// Preserve for min/max values defined by `scroll_container[]`.
	spec.aux_f32 = stoi(value); // scroll position
	e->setPos(spec.aux_f32);

	e->setSmallStep(data->scrollbar_options.small_step);
	e->setLargeStep(data->scrollbar_options.large_step);

	s32 scrollbar_size = is_horizontal ? dim.X : dim.Y;

	e->setPageSize(scrollbar_size * (max - min + 1) / data->scrollbar_options.thumb_size);

	if (spec.fname == m_focused_element) {
		Environment->setFocus(e);
	}

	m_scrollbars.emplace_back(spec,e);
	m_fields.push_back(spec);
}
#endif

void GUIFormSpecMenu::parseScrollBarOptions(parserData* data, const std::string &element)
{
	std::vector<std::string> parts = split(element, ';');

	if (parts.size() == 0) {
		warningstream << "Invalid scrollbaroptions element(" << parts.size() << "): '" <<
			element << "'"  << std::endl;
		return;
	}

	for (const std::string &i : parts) {
		std::vector<std::string> options = split(i, '=');

		if (options.size() != 2) {
			warningstream << "Invalid scrollbaroptions option syntax: '" <<
				element << "'" << std::endl;
			continue; // Go to next option
		}

		if (options[0] == "max") {
			data->scrollbar_options.max = stoi(options[1]);
			continue;
		} else if (options[0] == "min") {
			data->scrollbar_options.min = stoi(options[1]);
			continue;
		} else if (options[0] == "smallstep") {
			int value = stoi(options[1]);
			data->scrollbar_options.small_step = value < 0 ? 10 : value;
			continue;
		} else if (options[0] == "largestep") {
			int value = stoi(options[1]);
			data->scrollbar_options.large_step = value < 0 ? 100 : value;
			continue;
		} else if (options[0] == "thumbsize") {
			int value = stoi(options[1]);
			data->scrollbar_options.thumb_size = value <= 0 ? 1 : value;
			continue;
		} else if (options[0] == "arrows") {
			auto value = trim(options[1]);
			if (value == "hide")
				data->scrollbar_options.arrow_visiblity = GUIScrollBar::HIDE;
			else if (value == "show")
				data->scrollbar_options.arrow_visiblity = GUIScrollBar::SHOW;
			else // Auto hide/show
				data->scrollbar_options.arrow_visiblity = GUIScrollBar::DEFAULT;
			continue;
		}

		warningstream << "Invalid scrollbaroptions option(" << options[0] <<
			"): '" << element << "'" << std::endl;
	}
}

void GUIFormSpecMenu::parseImage(parserData* data, const std::string &element)
{
	std::vector<std::string> parts;
	if (!precheckElement("image", element, 2, 4, parts))
		return;

	size_t offset = parts.size() >= 3;

	std::vector<std::string> v_pos = split(parts[0],',');
	MY_CHECKPOS("image", 0);

	std::vector<std::string> v_geom;
	if (parts.size() >= 3) {
		v_geom = split(parts[1],',');
		MY_CHECKGEOM("image", 1);
	}

	std::string name = unescape_string(parts[1 + offset]);
	video::ITexture *texture = m_tsrc->getTexture(name);

	v2s32 pos;
	v2s32 geom;

	if (parts.size() < 3) {
		if (texture != nullptr) {
			core::dimension2du dim = texture->getOriginalSize();
			geom.X = dim.Width;
			geom.Y = dim.Height;
		} else {
			geom = v2s32(0);
		}
	}

	if (data->real_coordinates) {
		pos = getRealCoordinateBasePos(v_pos);
		if (parts.size() >= 3)
			geom = getRealCoordinateGeometry(v_geom);
	} else {
		pos = getElementBasePos(&v_pos);
		if (parts.size() >= 3) {
			geom.X = stof(v_geom[0]) * (float)imgsize.X;
			geom.Y = stof(v_geom[1]) * (float)imgsize.Y;
		}
	}

	if (!data->explicit_size)
		warningstream << "Invalid use of image without a size[] element" << std::endl;

	FieldSpec spec(
		name,
		L"",
		L"",
		258 + m_fields.size(),
		1
	);

	core::rect<s32> rect = core::rect<s32>(pos, pos + geom);

	core::rect<s32> middle;
	if (parts.size() >= 4)
		parseMiddleRect(parts[3], &middle);

	// Temporary fix for issue #12581 in 5.6.0.
	// Use legacy image when not rendering 9-slice image because GUIAnimatedImage
	// uses NNAA filter which causes visual artifacts when image uses alpha blending.

	gui::IGUIElement *e;
	if (middle.getArea() > 0) {
		GUIAnimatedImage *image = new GUIAnimatedImage(Environment, data->current_parent,
			spec.fid, rect);

		image->setTexture(texture);
		image->setMiddleRect(middle);
		e = image;
	}
	else {
		gui::IGUIImage *image = Environment->addImage(rect, data->current_parent, spec.fid, nullptr, true);
		image->setImage(texture);
		image->setScaleImage(true);
		image->grab(); // compensate for drop in addImage
		e = image;
	}

	auto style = getDefaultStyleForElement("image", spec.fname);
	e->setNotClipped(style.getBool(StyleSpec::NOCLIP, m_formspec_version < 3));

	// Animated images should let events through
	m_clickthrough_elements.push_back(e);

	m_fields.push_back(spec);
}

void GUIFormSpecMenu::parseAnimatedImage(parserData *data, const std::string &element)
{
	std::vector<std::string> parts;
#if IS_VOPI_ENGINE
	// VOPI: allow a 10th param (column count for a 2D grid atlas).
	if (!precheckElement("animated_image", element, 6, 10, parts))
		return;
#else
	if (!precheckElement("animated_image", element, 6, 9, parts))
		return;
#endif

	std::vector<std::string> v_pos  = split(parts[0], ',');
	std::vector<std::string> v_geom = split(parts[1], ',');
	std::string name = parts[2];
	std::string texture_name = unescape_string(parts[3]);
	s32 frame_count = stoi(parts[4]);
	s32 frame_duration = stoi(parts[5]);

	MY_CHECKPOS("animated_image", 0);
	MY_CHECKGEOM("animated_image", 1);

	v2s32 pos;
	v2s32 geom;

	if (data->real_coordinates) {
		pos = getRealCoordinateBasePos(v_pos);
		geom = getRealCoordinateGeometry(v_geom);
	} else {
		pos = getElementBasePos(&v_pos);
		geom.X = stof(v_geom[0]) * (float)imgsize.X;
		geom.Y = stof(v_geom[1]) * (float)imgsize.Y;
	}

	if (!data->explicit_size)
		warningstream << "Invalid use of animated_image without a size[] element"
				<< std::endl;

	FieldSpec spec(
		name,
		L"",
		L"",
		258 + m_fields.size()
	);
	spec.ftype = f_AnimatedImage;
	spec.send = true;

	core::rect<s32> rect = core::rect<s32>(pos, pos + geom);

	core::rect<s32> middle;
	if (parts.size() >= 8)
		parseMiddleRect(parts[7], &middle);

	GUIAnimatedImage *e = new GUIAnimatedImage(Environment, data->current_parent,
		spec.fid, rect);

	e->setTexture(m_tsrc->getTexture(texture_name));
	e->setMiddleRect(middle);
	e->setFrameDuration(frame_duration);
	e->setFrameCount(frame_count);
	if (parts.size() >= 7)
		e->setFrameIndex(stoi(parts[6]) - 1);
	// Optional 9th param: loop flag. Default true (upstream behaviour); set
	// to false for a one-shot animation that holds on its last frame.
	if (parts.size() >= 9)
		e->setLoop(is_yes(parts[8]));
#if IS_VOPI_ENGINE
	// VOPI: Optional 10th param: column count for a 2D grid atlas (frames packed
	// left-to-right, then top-to-bottom). Default 1 = vertical strip. A grid
	// keeps long animations within the GPU's max texture size.
	if (parts.size() >= 10) {
		s32 columns = stoi(parts[9]);
		e->setColumns(columns);
		video::ITexture *tex = e->getTexture();
		if (tex && columns > 1) {
			const core::dimension2d<u32> ts = tex->getOriginalSize();
			// 64-bit ceil: frame_count is the raw (unclamped, possibly negative
			// or huge) parts[4] value and columns is server-controlled, so the
			// s32 numerator (frame_count + columns - 1) could overflow. The
			// grid_rows > 0 guard below still drops a nonsensical grid.
			const s32 grid_rows = (s32)(((s64)frame_count + columns - 1) / columns);
			if (grid_rows > 0 && (ts.Width % columns != 0 || ts.Height % grid_rows != 0))
				warningstream << "animated_image[" << name << "]: atlas "
					<< ts.Width << "x" << ts.Height << " is not evenly divisible by a "
					<< columns << "x" << grid_rows << " grid; frames may jitter."
					<< std::endl;
		}
	}
#endif

	auto style = getDefaultStyleForElement("animated_image", spec.fname, "image");
	e->setNotClipped(style.getBool(StyleSpec::NOCLIP, false));

	// Animated images should let events through
	m_clickthrough_elements.push_back(e);

	m_fields.push_back(spec);
}

void GUIFormSpecMenu::parseItemImage(parserData* data, const std::string &element)
{
	std::vector<std::string> parts;
	if (!precheckElement("item_image", element, 3, 3, parts))
		return;

	std::vector<std::string> v_pos = split(parts[0],',');
	std::vector<std::string> v_geom = split(parts[1],',');
	std::string name = parts[2];

	MY_CHECKPOS("item_image",0);
	MY_CHECKGEOM("item_image",1);

	v2s32 pos;
	v2s32 geom;

	if (data->real_coordinates) {
		pos = getRealCoordinateBasePos(v_pos);
		geom = getRealCoordinateGeometry(v_geom);
	} else {
		pos = getElementBasePos(&v_pos);
		geom.X = stof(v_geom[0]) * (float)imgsize.X;
		geom.Y = stof(v_geom[1]) * (float)imgsize.Y;
	}

	if(!data->explicit_size)
		warningstream << "invalid use of item_image without a size[] element" << std::endl;

	FieldSpec spec(
		"",
		L"",
		L"",
		258 + m_fields.size(),
		2
	);
	spec.ftype = f_ItemImage;

	GUIItemImage *e = new GUIItemImage(Environment, data->current_parent, spec.fid,
			core::rect<s32>(pos, pos + geom), name, m_font, m_client);
	auto style = getDefaultStyleForElement("item_image", spec.fname);
	e->setNotClipped(style.getBool(StyleSpec::NOCLIP, false));

	// item images should let events through
	m_clickthrough_elements.push_back(e);

	m_fields.push_back(spec);
}

void GUIFormSpecMenu::parseButton(parserData* data, const std::string &element)
{
	int expected_parts = (data->type == "button_url" || data->type == "button_url_exit") ? 5 : 4;
	std::vector<std::string> parts;
	if (!precheckElement("button", element, expected_parts, expected_parts, parts))
		return;

	std::vector<std::string> v_pos = split(parts[0],',');
	std::vector<std::string> v_geom = split(parts[1],',');
	std::string name = parts[2];
	std::string label = parts[3];
	std::string url;
	if (data->type == "button_url" || data->type == "button_url_exit")
		url = parts[4];

	MY_CHECKPOS("button",0);
	MY_CHECKGEOM("button",1);

	v2s32 pos;
	v2s32 geom;
	core::rect<s32> rect;

	if (data->real_coordinates) {
		pos = getRealCoordinateBasePos(v_pos);
		geom = getRealCoordinateGeometry(v_geom);
		rect = core::rect<s32>(pos.X, pos.Y, pos.X+geom.X,
			pos.Y+geom.Y);
	} else {
		pos = getElementBasePos(&v_pos);
		geom.X = (stof(v_geom[0]) * spacing.X) - (spacing.X - imgsize.X);
		pos.Y += (stof(v_geom[1]) * (float)imgsize.Y)/2;

		rect = core::rect<s32>(pos.X, pos.Y - m_btn_height,
					pos.X + geom.X, pos.Y + m_btn_height);
	}

	if(!data->explicit_size)
		warningstream << "invalid use of button without a size[] element" << std::endl;

	std::wstring wlabel = translate_string(utf8_to_wide(unescape_string(label)));

	FieldSpec spec(
		name,
		wlabel,
		L"",
		258 + m_fields.size()
	);
	spec.ftype = f_Button;
	if (data->type == "button_exit" || data->type == "button_url_exit")
		spec.is_exit = true;
	if (data->type == "button_url" || data->type == "button_url_exit")
		spec.url = url;

	GUIButton *e;

	if (data->type == "button_key") {
		spec.ftype = f_Unknown;
		e = GUIButtonKey::addButton(Environment, rect, m_tsrc,
				data->current_parent, spec.fid, spec.flabel.c_str());
	} else {
		e = GUIButton::addButton(Environment, rect, m_tsrc,
				data->current_parent, spec.fid, spec.flabel.c_str());
	}

	auto style = getStyleForElement(data->type, name, (data->type != "button") ? "button" : "");

	spec.sound = style[StyleSpec::STATE_DEFAULT].get(StyleSpec::Property::SOUND, "");

	e->setFontScale(m_font_scale);
	e->setStyles(style);

	if (spec.fname == m_focused_element) {
		Environment->setFocus(e);
	}

	m_fields.push_back(spec);
}

bool GUIFormSpecMenu::parseMiddleRect(const std::string &value, core::rect<s32> *parsed_rect)
{
	core::rect<s32> rect;
	std::vector<std::string> v_rect = split(value, ',');

	if (v_rect.size() == 1) {
		s32 x = stoi(v_rect[0]);
		rect.UpperLeftCorner = core::vector2di(x, x);
		rect.LowerRightCorner = core::vector2di(-x, -x);
	} else if (v_rect.size() == 2) {
		s32 x = stoi(v_rect[0]);
		s32 y =	stoi(v_rect[1]);
		rect.UpperLeftCorner = core::vector2di(x, y);
		rect.LowerRightCorner = core::vector2di(-x, -y);
		// `-x` is interpreted as `w - x`
	} else if (v_rect.size() == 4) {
		rect.UpperLeftCorner = core::vector2di(stoi(v_rect[0]), stoi(v_rect[1]));
		rect.LowerRightCorner = core::vector2di(stoi(v_rect[2]), stoi(v_rect[3]));
	} else {
		warningstream << "Invalid rectangle string format: \"" << value
				<< "\"" << std::endl;
		return false;
	}

	*parsed_rect = rect;

	return true;
}

void GUIFormSpecMenu::parseBackground(parserData* data, const std::string &element)
{
	std::vector<std::string> parts;
	if (!precheckElement("background", element, 3, 5, parts))
		return;

	std::vector<std::string> v_pos = split(parts[0],',');
	std::vector<std::string> v_geom = split(parts[1],',');
	std::string name = unescape_string(parts[2]);

	MY_CHECKPOS("background",0);
	MY_CHECKGEOM("background",1);

	v2s32 pos;
	v2s32 geom;

	if (data->real_coordinates) {
		pos = getRealCoordinateBasePos(v_pos);
		geom = getRealCoordinateGeometry(v_geom);
	} else {
		pos = getElementBasePos(&v_pos);
		pos.X -= (spacing.X - (float)imgsize.X) / 2;
		pos.Y -= (spacing.Y - (float)imgsize.Y) / 2;

		geom.X = stof(v_geom[0]) * spacing.X;
		geom.Y = stof(v_geom[1]) * spacing.Y;
	}

	bool clip = false;
	if (parts.size() >= 4 && is_yes(parts[3])) {
		if (data->real_coordinates) {
			pos = getRealCoordinateBasePos(v_pos) * -1;
			geom = v2s32(0, 0);
		} else {
			pos.X = stoi(v_pos[0]); //acts as offset
			pos.Y = stoi(v_pos[1]);
		}
		clip = true;
	}

	core::rect<s32> middle;
	if (parts.size() >= 5)
		parseMiddleRect(parts[4], &middle);

	if (!data->explicit_size && !clip)
		warningstream << "invalid use of unclipped background without a size[] element" << std::endl;

	FieldSpec spec(
		name,
		L"",
		L"",
		258 + m_fields.size()
	);

	core::rect<s32> rect{};
	v2s32 autoclip_offset{};
	if (!clip) {
		// no auto_clip => position like normal image
		rect = core::rect<s32>(pos, pos + geom);
	} else {
		// element will be auto-clipped when drawing
		autoclip_offset = pos;
	}

	GUIBackgroundImage *e = new GUIBackgroundImage(Environment, data->background_parent.get(),
			spec.fid, rect, name, middle, m_tsrc, clip, autoclip_offset);

	FATAL_ERROR_IF(!e, "Failed to create background formspec element");

	e->setNotClipped(true);

	m_fields.push_back(spec);
	e->drop();
}

void GUIFormSpecMenu::parseTableOptions(parserData* data, const std::string &element)
{
	std::vector<std::string> parts = split(element,';');

	data->table_options.clear();
	for (const std::string &part : parts) {
		// Parse table option
		std::string opt = unescape_string(part);
		data->table_options.push_back(GUITable::splitOption(opt));
	}
}

void GUIFormSpecMenu::parseTableColumns(parserData* data, const std::string &element)
{
	std::vector<std::string> parts = split(element,';');

	data->table_columns.clear();
	for (const std::string &part : parts) {
		std::vector<std::string> col_parts = split(part,',');
		GUITable::TableColumn column;
		// Parse column type
		if (!col_parts.empty())
			column.type = col_parts[0];
		// Parse column options
		for (size_t j = 1; j < col_parts.size(); ++j) {
			std::string opt = unescape_string(col_parts[j]);
			column.options.push_back(GUITable::splitOption(opt));
		}
		data->table_columns.push_back(column);
	}
}

void GUIFormSpecMenu::parseTable(parserData* data, const std::string &element)
{
	std::vector<std::string> parts;
	if (!precheckElement("table", element, 4, 5, parts))
		return;

	std::vector<std::string> v_pos = split(parts[0],',');
	std::vector<std::string> v_geom = split(parts[1],',');
	std::string name = parts[2];
	std::vector<std::string> items;
	if (!parts[3].empty())
		items = split(parts[3],',');
	std::string str_initial_selection;

	if (parts.size() >= 5)
		str_initial_selection = parts[4];

	MY_CHECKPOS("table",0);
	MY_CHECKGEOM("table",1);

	v2s32 pos;
	v2s32 geom;

	if (data->real_coordinates) {
		pos = getRealCoordinateBasePos(v_pos);
		geom = getRealCoordinateGeometry(v_geom);
	} else {
		pos = getElementBasePos(&v_pos);
		geom.X = stof(v_geom[0]) * spacing.X;
		geom.Y = stof(v_geom[1]) * spacing.Y;
	}

	core::rect<s32> rect = core::rect<s32>(pos.X, pos.Y, pos.X+geom.X, pos.Y+geom.Y);

	FieldSpec spec(
		name,
		L"",
		L"",
		258 + m_fields.size()
	);

	spec.ftype = f_Table;

	for (std::string &item : items) {
		item = wide_to_utf8(unescape_translate(utf8_to_wide(unescape_string(item))));
	}

	GUITable *e = new GUITable(Environment, data->current_parent, spec.fid,
			rect, m_tsrc);

	// Apply styling before calculating the cell sizes
	auto style = getDefaultStyleForElement("table", name);
#if IS_VOPI_ENGINE
	e->setStyle(style, m_font_scale);
#else
	e->setNotClipped(style.getBool(StyleSpec::NOCLIP, false));
	e->setOverrideFont(getScaledStyleFont(style));
#endif

	if (spec.fname == m_focused_element) {
		Environment->setFocus(e);
	}

	e->setTable(data->table_options, data->table_columns, items);

	if (data->table_dyndata.find(name) != data->table_dyndata.end()) {
		e->setDynamicData(data->table_dyndata[name]);
	}

	if (!str_initial_selection.empty() && str_initial_selection != "0")
		e->setSelected(stoi(str_initial_selection));

	m_tables.emplace_back(spec, e);
	m_fields.push_back(spec);
}

void GUIFormSpecMenu::parseTextList(parserData* data, const std::string &element)
{
	std::vector<std::string> parts;
	if (!precheckElement("textlist", element, 4, 6, parts))
		return;

	std::vector<std::string> v_pos = split(parts[0],',');
	std::vector<std::string> v_geom = split(parts[1],',');
	std::string name = parts[2];
	std::vector<std::string> items;
	if (!parts[3].empty())
		items = split(parts[3],',');
	std::string str_initial_selection;
	std::string str_transparent = "false";

	if (parts.size() >= 5)
		str_initial_selection = parts[4];

	if (parts.size() >= 6)
		str_transparent = parts[5];

	MY_CHECKPOS("textlist",0);
	MY_CHECKGEOM("textlist",1);

	v2s32 pos;
	v2s32 geom;

	if (data->real_coordinates) {
		pos = getRealCoordinateBasePos(v_pos);
		geom = getRealCoordinateGeometry(v_geom);
	} else {
		pos = getElementBasePos(&v_pos);
		geom.X = stof(v_geom[0]) * spacing.X;
		geom.Y = stof(v_geom[1]) * spacing.Y;
	}

	core::rect<s32> rect = core::rect<s32>(pos.X, pos.Y, pos.X+geom.X, pos.Y+geom.Y);

	FieldSpec spec(
		name,
		L"",
		L"",
		258 + m_fields.size()
	);

	spec.ftype = f_Table;

	for (std::string &item : items) {
		item = wide_to_utf8(unescape_translate(utf8_to_wide(unescape_string(item))));
	}

	//now really show list
	GUITable *e = new GUITable(Environment, data->current_parent, spec.fid,
			rect, m_tsrc);

	if (spec.fname == m_focused_element) {
		Environment->setFocus(e);
	}

	e->setTextList(items, is_yes(str_transparent));

	if (data->table_dyndata.find(name) != data->table_dyndata.end()) {
		e->setDynamicData(data->table_dyndata[name]);
	}

	if (!str_initial_selection.empty() && str_initial_selection != "0")
		e->setSelected(stoi(str_initial_selection));

	auto style = getDefaultStyleForElement("textlist", name);
#if IS_VOPI_ENGINE
	e->setStyle(style, m_font_scale);
#else
	e->setNotClipped(style.getBool(StyleSpec::NOCLIP, false));
	e->setOverrideFont(getScaledStyleFont(style));
#endif

	m_tables.emplace_back(spec, e);
	m_fields.push_back(spec);
}

void GUIFormSpecMenu::parseDropDown(parserData* data, const std::string &element)
{
	std::vector<std::string> parts;
	if (!precheckElement("dropdown", element, 5, 6, parts))
		return;

	std::vector<std::string> v_pos = split(parts[0], ',');
	std::string name = parts[2];
	std::vector<std::string> items = split(parts[3], ',');
	std::string str_initial_selection = parts[4];

	if (parts.size() >= 6 && is_yes(parts[5]))
		m_dropdown_index_event[name] = true;

	MY_CHECKPOS("dropdown",0);

	v2s32 pos;
	v2s32 geom;
	core::rect<s32> rect;

	if (data->real_coordinates) {
		std::vector<std::string> v_geom = split(parts[1],',');

		if (v_geom.size() == 1)
			v_geom.emplace_back("1");

		MY_CHECKGEOM("dropdown",1);

		pos = getRealCoordinateBasePos(v_pos);
		geom = getRealCoordinateGeometry(v_geom);
		rect = core::rect<s32>(pos.X, pos.Y, pos.X+geom.X, pos.Y+geom.Y);
	} else {
		pos = getElementBasePos(&v_pos);

		s32 width = stof(parts[1]) * spacing.Y;

		rect = core::rect<s32>(pos.X, pos.Y,
				pos.X + width, pos.Y + (m_btn_height * 2));
	}

	FieldSpec spec(
		name,
		L"",
		L"",
		258 + m_fields.size()
	);

	spec.ftype = f_DropDown;
	spec.send = true;

	//now really show list
	gui::IGUIComboBox *e = Environment->addComboBox(rect, data->current_parent,
			spec.fid);

	if (spec.fname == m_focused_element) {
		Environment->setFocus(e);
	}

	for (const std::string &item : items) {
		e->addItem(unescape_translate(unescape_string(
			utf8_to_wide(item))).c_str());
	}

	if (!str_initial_selection.empty())
		e->setSelected(stoi(str_initial_selection)-1);

	auto style = getDefaultStyleForElement("dropdown", name);

	spec.sound = style.get(StyleSpec::Property::SOUND, "");

	e->setNotClipped(style.getBool(StyleSpec::NOCLIP, false));

	m_fields.push_back(spec);

	m_dropdowns.emplace_back(spec, std::vector<std::string>());
	std::vector<std::string> &values = m_dropdowns.back().second;
	for (const std::string &item : items) {
		values.push_back(unescape_string(item));
	}
}

void GUIFormSpecMenu::parseFieldEnterAfterEdit(parserData *data, const std::string &element)
{
	std::vector<std::string> parts;
	if (!precheckElement("field_enter_after_edit", element, 2, 2, parts))
		return;

	field_enter_after_edit[parts[0]] = is_yes(parts[1]);
}

void GUIFormSpecMenu::parseFieldCloseOnEnter(parserData *data, const std::string &element)
{
	std::vector<std::string> parts;
	if (!precheckElement("field_close_on_enter", element, 2, 2, parts))
		return;

	field_close_on_enter[parts[0]] = is_yes(parts[1]);
}

void GUIFormSpecMenu::parsePwdField(parserData* data, const std::string &element)
{
	std::vector<std::string> parts;
	if (!precheckElement("pwdfield", element, 4, 4, parts))
		return;

	std::vector<std::string> v_pos = split(parts[0],',');
	std::vector<std::string> v_geom = split(parts[1],',');
	std::string name = parts[2];
	std::string label = parts[3];

	MY_CHECKPOS("pwdfield",0);
	MY_CHECKGEOM("pwdfield",1);

	v2s32 pos;
	v2s32 geom;

	if (data->real_coordinates) {
		pos = getRealCoordinateBasePos(v_pos);
		geom = getRealCoordinateGeometry(v_geom);
	} else {
		pos = getElementBasePos(&v_pos);
		pos -= padding;

		geom.X = (stof(v_geom[0]) * spacing.X) - (spacing.X - imgsize.X);

		pos.Y += (stof(v_geom[1]) * (float)imgsize.Y)/2;
		pos.Y -= m_btn_height;
		geom.Y = m_btn_height*2;
	}

	core::rect<s32> rect = core::rect<s32>(pos.X, pos.Y, pos.X+geom.X, pos.Y+geom.Y);

	std::wstring wlabel = translate_string(utf8_to_wide(unescape_string(label)));

	FieldSpec spec(
		name,
		wlabel,
		L"",
		258 + m_fields.size(),
		0,
		ECI_IBEAM
		);

	spec.send = true;
	gui::IGUIEditBox *e = Environment->addEditBox(0, rect, true,
			data->current_parent, spec.fid);

	if (spec.fname == m_focused_element) {
		Environment->setFocus(e);
	}

	if (label.length() >= 1) {
		int font_height = g_fontengine->getTextHeight();
		rect.UpperLeftCorner.Y -= font_height;
		rect.LowerRightCorner.Y = rect.UpperLeftCorner.Y + font_height;
		gui::StaticText::add(Environment, spec.flabel.c_str(), rect, false, true,
			data->current_parent, 0);
	}

	e->setPasswordBox(true,L'*');

	auto style = getDefaultStyleForElement("pwdfield", name, "field");
	e->setNotClipped(style.getBool(StyleSpec::NOCLIP, false));
	e->setDrawBorder(style.getBool(StyleSpec::BORDER, true));
	e->setOverrideColor(style.getColor(StyleSpec::TEXTCOLOR, video::SColor(0xFFFFFFFF)));
	e->setOverrideFont(getScaledStyleFont(style));

	SEvent evt;
	evt.EventType            = EET_KEY_INPUT_EVENT;
	evt.KeyInput.Key         = KEY_END;
	evt.KeyInput.Char        = 0;
	evt.KeyInput.Control     = false;
	evt.KeyInput.Shift       = false;
	evt.KeyInput.PressedDown = true;
	e->OnEvent(evt);

	// Note: Before 5.2.0 "parts.size() >= 5" resulted in a
	// warning referring to field_close_on_enter[]!

	m_fields.push_back(spec);
}

void GUIFormSpecMenu::createTextField(parserData *data, FieldSpec &spec,
	core::rect<s32> &rect, bool is_multiline)
{
	bool is_editable = !spec.fname.empty();
	if (!is_editable && !is_multiline) {
		// spec field id to 0, this stops submit searching for a value that isn't there
		gui::StaticText::add(Environment, spec.flabel.c_str(), rect, false, true,
				data->current_parent, 0);
		return;
	}

	if (is_editable) {
		spec.send = true;
	} else if (is_multiline &&
			spec.fdefault.empty() && !spec.flabel.empty()) {
		// Multiline textareas: swap default and label for backwards compat
		spec.flabel.swap(spec.fdefault);
	}


	auto style = getDefaultStyleForElement(is_multiline ? "textarea" : "field", spec.fname);

#if IS_VOPI_ENGINE
	GUIEditBoxWithScrollBar *box = nullptr;
#endif
	gui::IGUIEditBox *e = nullptr;
	if (is_multiline) {
#if IS_VOPI_ENGINE
		// scrollbar_visible=false drops the built-in scrollbar entirely; the
		// text keeps the full element width. Read-only boxes only: they stay
		// scrollable by touch drag / mouse wheel, while an editable box would
		// lose its last scroll affordance (selection owns its pointer drags
		// and the base wheel path needs a visible scrollbar).
		box = new GUIEditBoxWithScrollBar(spec.fdefault.c_str(), true, Environment,
				data->current_parent, spec.fid, rect, m_tsrc, is_editable,
				is_editable || style.getBool(StyleSpec::SCROLLBAR_VISIBLE, true));
		e = box;
#else
		e = new GUIEditBoxWithScrollBar(spec.fdefault.c_str(), true, Environment,
				data->current_parent, spec.fid, rect, m_tsrc, is_editable, true);
#endif
	} else if (is_editable) {
		e = Environment->addEditBox(spec.fdefault.c_str(), rect, true,
				data->current_parent, spec.fid);
		e->grab();
	}

	if (e) {
		if (is_editable && spec.fname == m_focused_element)
			Environment->setFocus(e);

		if (is_multiline) {
			e->setMultiLine(true);
			e->setWordWrap(true);
			e->setTextAlignment(gui::EGUIA_UPPERLEFT, gui::EGUIA_UPPERLEFT);
		} else {
			SEvent evt;
			evt.EventType            = EET_KEY_INPUT_EVENT;
			evt.KeyInput.Key         = KEY_END;
			evt.KeyInput.Char        = 0;
			evt.KeyInput.Control     = 0;
			evt.KeyInput.Shift       = 0;
			evt.KeyInput.PressedDown = true;
			e->OnEvent(evt);
		}

		e->setNotClipped(style.getBool(StyleSpec::NOCLIP, false));
		e->setOverrideColor(style.getColor(StyleSpec::TEXTCOLOR, video::SColor(0xFFFFFFFF)));
		bool border = style.getBool(StyleSpec::BORDER, true);
		e->setDrawBorder(border);
		e->setDrawBackground(border);
		e->setOverrideFont(getScaledStyleFont(style));

#if IS_VOPI_ENGINE
		if (box != nullptr)
			box->setScrollbarStyle(style, m_tsrc);
#endif
#if IS_VOPI_ENGINE && (defined(__ANDROID__) || defined(__IOS__))
		// Read-only textareas pan by touch drag: register them as gesture
		// targets (editable ones keep press-to-place-cursor semantics).
		if (box && !is_editable)
			m_scroll_textareas.push_back(box);
#endif

		e->drop();
	}

	if (!spec.flabel.empty()) {
		int font_height = g_fontengine->getTextHeight();
		rect.UpperLeftCorner.Y -= font_height;
		rect.LowerRightCorner.Y = rect.UpperLeftCorner.Y + font_height;
		IGUIElement *t = gui::StaticText::add(Environment, spec.flabel.c_str(),
				rect, false, true, data->current_parent, 0);

		if (t)
			t->setNotClipped(style.getBool(StyleSpec::NOCLIP, false));
	}
}

void GUIFormSpecMenu::parseSimpleField(parserData *data,
	std::vector<std::string> &parts)
{
	std::string name = parts[0];
	std::string label = parts[1];
	std::string default_val = parts[2];

	core::rect<s32> rect;

	if (data->explicit_size)
		warningstream << "invalid use of unpositioned \"field\" in inventory" << std::endl;

	v2s32 pos = getElementBasePos(nullptr);
	pos.Y = (data->simple_field_count + 2) * 60;
	v2s32 size = DesiredRect.getSize();

	rect = core::rect<s32>(
			size.X / 2 - 150,       pos.Y,
			size.X / 2 - 150 + 300, pos.Y + m_btn_height * 2
	);


	if (m_form_src)
		default_val = m_form_src->resolveText(default_val);


	std::wstring wlabel = translate_string(utf8_to_wide(unescape_string(label)));

	FieldSpec spec(
		name,
		wlabel,
		utf8_to_wide(unescape_string(default_val)),
		258 + m_fields.size(),
		0,
		ECI_IBEAM
	);

	createTextField(data, spec, rect, false);

	m_fields.push_back(spec);

	data->simple_field_count++;
}

void GUIFormSpecMenu::parseTextArea(parserData* data, std::vector<std::string>& parts,
		const std::string &type)
{
	std::vector<std::string> v_pos = split(parts[0],',');
	std::vector<std::string> v_geom = split(parts[1],',');
	std::string name = parts[2];
	std::string label = parts[3];
	std::string default_val = parts[4];

	MY_CHECKPOS(type,0);
	MY_CHECKGEOM(type,1);

	v2s32 pos;
	v2s32 geom;

	if (data->real_coordinates) {
		pos = getRealCoordinateBasePos(v_pos);
		geom = getRealCoordinateGeometry(v_geom);
	} else {
		pos = getElementBasePos(&v_pos);
		pos -= padding;

		geom.X = (stof(v_geom[0]) * spacing.X) - (spacing.X - imgsize.X);

		if (type == "textarea")
		{
			geom.Y = (stof(v_geom[1]) * (float)imgsize.Y) - (spacing.Y-imgsize.Y);
			pos.Y += m_btn_height;
		}
		else
		{
			pos.Y += (stof(v_geom[1]) * (float)imgsize.Y)/2;
			pos.Y -= m_btn_height;
			geom.Y = m_btn_height*2;
		}
	}

	core::rect<s32> rect = core::rect<s32>(pos.X, pos.Y, pos.X+geom.X, pos.Y+geom.Y);

	if(!data->explicit_size)
		warningstream << "invalid use of positioned " << type << " without a size[] element" << std::endl;

	if(m_form_src)
		default_val = m_form_src->resolveText(default_val);


	std::wstring wlabel = translate_string(utf8_to_wide(unescape_string(label)));

	FieldSpec spec(
		name,
		wlabel,
		utf8_to_wide(unescape_string(default_val)),
		258 + m_fields.size(),
		0,
		ECI_IBEAM
	);

	createTextField(data, spec, rect, type == "textarea");

	// Note: Before 5.2.0 "parts.size() >= 6" resulted in a
	// warning referring to field_close_on_enter[]!

	m_fields.push_back(spec);
}

void GUIFormSpecMenu::parseField(parserData* data, const std::string &element)
{
	std::vector<std::string> parts;
	if (!precheckElement(data->type, element, 3, 5, parts))
		return;

	if (parts.size() == 3 || parts.size() == 4) {
		parseSimpleField(data, parts);
		return;
	}

	// Else: >= 5 arguments in "parts"
	parseTextArea(data, parts, data->type);
}

void GUIFormSpecMenu::parseHyperText(parserData *data, const std::string &element)
{
	std::vector<std::string> parts;
	if (!precheckElement("hypertext", element, 4, 4, parts))
		return;

	std::vector<std::string> v_pos = split(parts[0], ',');
	std::vector<std::string> v_geom = split(parts[1], ',');
	std::string name = parts[2];
	std::string text = parts[3];

	MY_CHECKPOS("hypertext", 0);
	MY_CHECKGEOM("hypertext", 1);

	v2s32 pos;
	v2s32 geom;

	if (data->real_coordinates) {
		pos = getRealCoordinateBasePos(v_pos);
		geom = getRealCoordinateGeometry(v_geom);
	} else {
		pos = getElementBasePos(&v_pos);
		pos -= padding;

		geom.X = (stof(v_geom[0]) * spacing.X) - (spacing.X - imgsize.X);
		geom.Y = (stof(v_geom[1]) * (float)imgsize.Y) - (spacing.Y - imgsize.Y);
		pos.Y += m_btn_height;
	}

	core::rect<s32> rect = core::rect<s32>(pos.X, pos.Y, pos.X + geom.X, pos.Y + geom.Y);

	if(m_form_src)
		text = m_form_src->resolveText(text);

	FieldSpec spec(
		name,
		translate_string(utf8_to_wide(unescape_string(text))),
		L"",
		258 + m_fields.size()
	);

	spec.ftype = f_HyperText;

	auto style = getDefaultStyleForElement("hypertext", spec.fname);
	spec.sound = style.get(StyleSpec::Property::SOUND, "");

#if IS_VOPI_ENGINE
	// VOPI: pass m_font_scale so hypertext fonts track the UI size like other text.
	GUIHyperText *e = new GUIHyperText(spec.flabel.c_str(), Environment,
			data->current_parent, spec.fid, rect, m_client, m_tsrc, m_font_scale);
#else
	GUIHyperText *e = new GUIHyperText(spec.flabel.c_str(), Environment,
			data->current_parent, spec.fid, rect, m_client, m_tsrc);
#endif
	e->drop();

	m_fields.push_back(spec);
}

#if IS_VOPI_ENGINE
void GUIFormSpecMenu::parseClock(parserData *data, const std::string &element)
{
	std::vector<std::string> parts;
	if (!precheckElement("clock", element, 3, 4, parts) || !m_client ||
			m_clock_labels.size() >= 64)
		return;
	if (parts[2] != "12h" && parts[2] != "24h") {
		errorstream << "Invalid clock format: expected 12h or 24h" << std::endl;
		return;
	}
	bool twelve_hour = parts[2] == "12h";
	std::string label = parts[0] + ";" + parts[1] + ";" +
			formatDayCycleTime(m_client->getEnv().getTimeOfDayF(), twelve_hour);
#if IS_VOPI_ENGINE
	if (parts.size() == 4)
		label += ";" + parts[3];
#endif
	size_t before = m_fields.size();
	parseLabel(data, label);
	if (m_fields.size() == before + 1) {
		auto *text = getElementFromId(m_fields.back().fid, true);
		if (text)
			m_clock_labels.emplace_back(text, twelve_hour);
	}
}

#endif
void GUIFormSpecMenu::parseLabel(parserData* data, const std::string &element)
{
	std::vector<std::string> parts;
#if IS_VOPI_ENGINE
	// For IS_VOPI_ENGINE: support multiple formats:
	// - label[x,y;text]
	// - label[x,y;text;alignment]
	// - label[x,y;w,h;text]
	// - label[x,y;w,h;text;alignment]
	//
	// The alignment ("left" default, "center", "right") is an anchor: it
	// selects what x means — the left edge, the middle or the right edge of
	// the label (of the text when no size is given, of the w,h rect
	// otherwise). Unlike every other formspec element, x is therefore not
	// always the top-left corner. This is deliberate: it lets a label be
	// pinned to a layout point regardless of its width, identically in both
	// formats, and existing formspecs rely on it.
	if (!precheckElement("label", element, 2, 4, parts))
#else
	// Original logic: [pos], [size], [text] or [pos], [text]
	if (!precheckElement("label", element, 2, data->real_coordinates ? 3 : 2, parts))
#endif
		return;

	std::vector<std::string> v_pos = split(parts[0], ',');
	MY_CHECKPOS("label", 0);

	bool has_size = false;
#if IS_VOPI_ENGINE
	// parts[1] is a size iff it splits into exactly two tokens on UNESCAPED
	// commas. A plain find(',') also matches escaped commas ("\,") inside
	// label text, which misreads the text as a size and rejects the whole
	// element — label text legitimately contains commas (user input,
	// translations), escaped via formspec_escape.
	if (parts.size() >= 3 && split(parts[1], ',').size() == 2) {
		has_size = true;
	}
#else
	// Original logic
	has_size = parts.size() >= 3;
#endif
	v2s32 geom;
	if (has_size) {
		std::vector<std::string> v_geom = split(parts[1], ',');
		MY_CHECKGEOM("label", 1);
		geom = getRealCoordinateGeometry(v_geom);
	}

	if(!data->explicit_size)
		warningstream << "invalid use of label without a size[] element" << std::endl;

#if IS_VOPI_ENGINE
	// Determine alignment based on has_size
	std::string align = "left";
	if (has_size) {
		// Format: label[x,y;w,h;text] or label[x,y;w,h;text;alignment]
		if (parts.size() > 3) {
			align = parts[3];
		}
	} else {
		// Format: label[x,y;text] or label[x,y;text;alignment]
		if (parts.size() > 2) {
			align = parts[2];
		}
	}
#endif

	auto style = getDefaultStyleForElement("label", "");
	gui::IGUIFont *font = getScaledStyleFont(style);

#if IS_VOPI_ENGINE
	auto add_label = [&](core::rect<s32> rect, const EnrichedString &text,
			EGUI_ALIGNMENT align_h, EGUI_ALIGNMENT align_v, bool word_wrap,
			bool auto_center_multiline = false) {
		FieldSpec spec(
			"",
			L"",
			L"",
			258 + m_fields.size(),
			4
		);
		gui::StaticText *e = new gui::StaticText(text, false, Environment,
				data->current_parent, spec.fid, rect, false);
		e->setTextAlignment(align_h, align_v);
		e->setWordWrap(word_wrap);
		// VOPI_ENGINE: Enable auto-centering for multi-line word-wrapped labels
		if (auto_center_multiline && word_wrap) {
			e->setAutoCenterMultiline(true);
		}

		e->setNotClipped(style.getBool(StyleSpec::NOCLIP, false));
		e->setOverrideColor(style.getColor(StyleSpec::TEXTCOLOR, video::SColor(0xFFFFFFFF)));
		e->setOverrideFont(font);
		e->drop();

		m_fields.push_back(spec);

		// labels should let events through
		e->grab();
		m_clickthrough_elements.push_back(e);
	};
#else
	auto add_label = [&](core::rect<s32> rect, const EnrichedString &text,
			EGUI_ALIGNMENT align_h, EGUI_ALIGNMENT align_v, bool word_wrap) {
		FieldSpec spec(
			"",
			L"",
			L"",
			258 + m_fields.size(),
			4
		);
		gui::IGUIStaticText *e = gui::StaticText::add(Environment,
				text, rect, false, false, data->current_parent,
				spec.fid);
		e->setTextAlignment(align_h, align_v);
		e->setWordWrap(word_wrap);

		e->setNotClipped(style.getBool(StyleSpec::NOCLIP, false));
		e->setOverrideColor(style.getColor(StyleSpec::TEXTCOLOR, video::SColor(0xFFFFFFFF)));
		e->setOverrideFont(font);

		m_fields.push_back(spec);

		// labels should let events through
		e->grab();
		m_clickthrough_elements.push_back(e);
	};
#endif

	// Text position depends on whether size is specified
	std::string text_param = parts[has_size ? 2 : 1];
	EnrichedString str(unescape_string(utf8_to_wide(text_param)));

	if (geom == v2s32()) {
		size_t str_pos = 0;

		for (size_t i = 0; str_pos < str.size(); ++i) {
			EnrichedString line = str.getNextLine(&str_pos);

#if IS_VOPI_ENGINE
			s32 text_width = font->getDimension(line.c_str()).Width;
			gui::EGUI_ALIGNMENT e_align = gui::EGUIA_UPPERLEFT;
			if (align == "center") {
				e_align = gui::EGUIA_CENTER;
			} else if (align == "right") {
				e_align = gui::EGUIA_LOWERRIGHT;
			}
#endif

			core::rect<s32> rect;

			if (data->real_coordinates) {
				// Lines are spaced at the distance of 1/2 imgsize.
				// This alows lines that line up with the new elements
				// easily without sacrificing good line distance.  If
				// it was one whole imgsize, it would have too much
				// spacing.
				v2s32 pos = getRealCoordinateBasePos(v_pos);

				// Labels are positioned by their center, not their top.
#if IS_VOPI_ENGINE
				pos.Y += (((float) imgsize.Y) / -2) + (((float) imgsize.Y) * i / 3.5f);
#else
				pos.Y += (((float) imgsize.Y) / -2) + (((float) imgsize.Y) * i / 2);
#endif

#if IS_VOPI_ENGINE
				// Modify the X coordinate based on alignment
				if (align == "center") {
					pos.X -= text_width / 2;
				} else if (align == "right") {
					pos.X -= text_width;
				}
#endif

				rect = core::rect<s32>(
					pos.X, pos.Y,
#if IS_VOPI_ENGINE
					pos.X + text_width,
#else
					pos.X + font->getDimension(line.c_str()).Width,
#endif
					pos.Y + imgsize.Y);

			} else {
				// Lines are spaced at the nominal distance of
				// 2/5 inventory slot, even if the font doesn't
				// quite match that.  This provides consistent
				// form layout, at the expense of sometimes
				// having sub-optimal spacing for the font.
				// We multiply by 2 and then divide by 5, rather
				// than multiply by 0.4, to get exact results
				// in the integer cases: 0.4 is not exactly
				// representable in binary floating point.

				v2s32 pos = getElementBasePos(nullptr);
				pos.X += stof(v_pos[0]) * spacing.X;
				pos.Y += (stof(v_pos[1]) + 7.0f / 30.0f) * spacing.Y;

				pos.Y += ((float) i) * spacing.Y * 2.0 / 5.0;

#if IS_VOPI_ENGINE
				// Modify the X coordinate based on alignment
				if (align == "center") {
					pos.X -= text_width / 2;
				} else if (align == "right") {
					pos.X -= text_width;
				}
#endif

				rect = core::rect<s32>(
					pos.X, pos.Y - m_btn_height,
#if IS_VOPI_ENGINE
					pos.X + text_width,
#else
					pos.X + font->getDimension(line.c_str()).Width,
#endif
					pos.Y + m_btn_height);
			}

#if IS_VOPI_ENGINE
			add_label(rect, line, e_align, gui::EGUIA_CENTER, false);
#else
			add_label(rect, line, gui::EGUIA_UPPERLEFT, gui::EGUIA_CENTER, false);
#endif
		}
	} else {
		v2s32 pos = getRealCoordinateBasePos(v_pos);

#if IS_VOPI_ENGINE
		gui::EGUI_ALIGNMENT e_align = gui::EGUIA_UPPERLEFT;
		core::rect<s32> rect;

		if (align == "center") {
			e_align = gui::EGUIA_CENTER;
			// Center alignment: rect centered around pos.X
			rect = core::rect<s32>(
				pos.X - geom.X / 2, pos.Y,
				pos.X + geom.X / 2, pos.Y + geom.Y);
		} else if (align == "right") {
			e_align = gui::EGUIA_LOWERRIGHT;
			// Right alignment: pos.X is the right edge, text goes left
			rect = core::rect<s32>(
				pos.X - geom.X, pos.Y,
				pos.X, pos.Y + geom.Y);
		} else {
			// Left alignment (default): pos.X is the left edge, text goes right
			rect = core::rect<s32>(
				pos.X, pos.Y,
				pos.X + geom.X, pos.Y + geom.Y);
		}

		// VOPI_ENGINE: Use UPPERLEFT vertical alignment, but enable auto-centering
		// for multi-line labels (centers only when 2+ lines, with reduced spacing)
		add_label(rect, str, e_align, gui::EGUIA_UPPERLEFT, true, true);
#else
		core::rect<s32> rect(
				pos.X, pos.Y,
				pos.X + geom.X,
				pos.Y + geom.Y);
		add_label(rect, str, gui::EGUIA_UPPERLEFT, gui::EGUIA_UPPERLEFT, true);
#endif
	}
}

void GUIFormSpecMenu::parseVertLabel(parserData* data, const std::string &element)
{
	std::vector<std::string> parts;
	if (!precheckElement("vertlabel", element, 2, 2, parts))
		return;

	std::vector<std::string> v_pos = split(parts[0], ',');

	// Use EnrichedString so color escapes and formatting are preserved
	EnrichedString etext(unescape_string(utf8_to_wide(parts[1])));

	// Build vertical text (one character per line)
	EnrichedString vlabel;
	const size_t char_count = etext.getString().size();

	for (size_t i = 0; i < char_count; i++) {
		vlabel += etext.substr(i, 1);
		vlabel.addCharNoColor(L'\n');
	}

	MY_CHECKPOS("vertlabel", 1);

	auto style = getDefaultStyleForElement("vertlabel", "", "label");
	gui::IGUIFont *font = getScaledStyleFont(style);

	v2s32 pos;
	core::rect<s32> rect;

	if (data->real_coordinates) {
		pos = getRealCoordinateBasePos(v_pos);

		// Vertlabels are positioned by center, not left.
		pos.X -= imgsize.X / 2;

		rect = core::rect<s32>(pos.X, pos.Y,
			pos.X + imgsize.X,
			pos.Y + font_line_height(font) * char_count);

	} else {
		pos = getElementBasePos(&v_pos);

		// The width of the rect (15 pixels) seems rather
		// arbitrary, but changing it might break something.
		rect = core::rect<s32>(
			pos.X, pos.Y + ((imgsize.Y / 2) - m_btn_height),
			pos.X + 15, pos.Y +
				font_line_height(font) * (char_count + 1) +
				((imgsize.Y / 2) - m_btn_height));
	}

	if(!data->explicit_size)
		warningstream << "invalid use of label without a size[] element" << std::endl;

	FieldSpec spec(
		"",
		L"",
		L"",
		258 + m_fields.size()
	);

	gui::IGUIStaticText *e = gui::StaticText::add(Environment, vlabel,
			rect, false, false, data->current_parent, spec.fid);

	e->setTextAlignment(gui::EGUIA_CENTER, gui::EGUIA_CENTER);

	e->setNotClipped(style.getBool(StyleSpec::NOCLIP, false));
	e->setOverrideColor(style.getColor(StyleSpec::TEXTCOLOR, video::SColor(0xFFFFFFFF)));
	e->setOverrideFont(font);

	m_fields.push_back(spec);

	// vertlabels should let events through
	e->grab();
	m_clickthrough_elements.push_back(e);
}

void GUIFormSpecMenu::parseImageButton(parserData* data, const std::string &element)
{
	std::vector<std::string> parts;
	if (!precheckElement("image_button", element, 5, 8, parts))
		return;

	if (parts.size() == 6) {
		// Invalid argument count.
		errorstream << "Invalid image_button element(" << parts.size() << "): '" << element << "'" << std::endl;
		return;
	}

	std::vector<std::string> v_pos = split(parts[0],',');
	std::vector<std::string> v_geom = split(parts[1],',');
	std::string image_name = parts[2];
	std::string name = parts[3];
	std::string label = parts[4];

	MY_CHECKPOS("image_button",0);
	MY_CHECKGEOM("image_button",1);

	std::string pressed_image_name;

	if (parts.size() >= 8) {
		pressed_image_name = parts[7];
	}

	v2s32 pos;
	v2s32 geom;

	if (data->real_coordinates) {
		pos = getRealCoordinateBasePos(v_pos);
		geom = getRealCoordinateGeometry(v_geom);
	} else {
		pos = getElementBasePos(&v_pos);
		geom.X = (stof(v_geom[0]) * spacing.X) - (spacing.X - imgsize.X);
		geom.Y = (stof(v_geom[1]) * spacing.Y) - (spacing.Y - imgsize.Y);
	}

	core::rect<s32> rect = core::rect<s32>(pos.X, pos.Y, pos.X+geom.X,
		pos.Y+geom.Y);

	if (!data->explicit_size)
		warningstream << "invalid use of image_button without a size[] element" << std::endl;

	image_name = unescape_string(image_name);
	pressed_image_name = unescape_string(pressed_image_name);

	std::wstring wlabel = utf8_to_wide(unescape_string(label));

	FieldSpec spec(
		name,
		wlabel,
		utf8_to_wide(image_name),
		258 + m_fields.size()
	);
	spec.ftype = f_Button;

	if (data->type == "image_button_exit")
		spec.is_exit = true;

	GUIButtonImage *e = GUIButtonImage::addButton(Environment, rect, m_tsrc,
			data->current_parent, spec.fid, spec.flabel.c_str());

	if (spec.fname == m_focused_element) {
		Environment->setFocus(e);
	}

	auto style = getStyleForElement("image_button", spec.fname);

	spec.sound = style[StyleSpec::STATE_DEFAULT].get(StyleSpec::Property::SOUND, "");

	// Override style properties with values specified directly in the element
	if (!image_name.empty())
		style[StyleSpec::STATE_DEFAULT].set(StyleSpec::FGIMG, image_name);

	if (!pressed_image_name.empty())
		style[StyleSpec::STATE_PRESSED].set(StyleSpec::FGIMG, pressed_image_name);

	if (parts.size() >= 7) {
		style[StyleSpec::STATE_DEFAULT].set(StyleSpec::NOCLIP, parts[5]);
		style[StyleSpec::STATE_DEFAULT].set(StyleSpec::BORDER, parts[6]);
	}

	e->setFontScale(m_font_scale);
	e->setStyles(style);
	e->setScaleImage(true);

	m_fields.push_back(spec);
}

void GUIFormSpecMenu::parseTabHeader(parserData* data, const std::string &element)
{
	std::vector<std::string> parts;
	if (!precheckElement("tabheader", element, 4, 7, parts))
		return;

	// Length 7: Additional "height" parameter after "pos". Only valid with real_coordinates.
	// Note: New arguments for the "height" syntax cannot be added without breaking older clients.
	if (parts.size() == 5 || (parts.size() == 7 && !data->real_coordinates)) {
		errorstream << "Invalid tabheader element(" << parts.size() << "): '"
			<< element << "'" << std::endl;
		return;
	}

	std::vector<std::string> v_pos = split(parts[0],',');

	// If we're using real coordinates, add an extra field for height.
	// Width is not here because tabs are the width of the text, and
	// there's no reason to change that.
	unsigned int i = 0;
	std::vector<std::string> v_geom = {"1", "1"}; // Dummy width and height
	bool auto_width = true;
	if (parts.size() == 7) {
		i++;

		v_geom = split(parts[1], ',');
		if (v_geom.size() == 1)
			v_geom.insert(v_geom.begin(), "1"); // Dummy value
		else
			auto_width = false;
	}

	std::string name = parts[i+1];
	std::vector<std::string> buttons = split(parts[i+2], ',');
	std::string str_index = parts[i+3];
	bool show_background = true;
	bool show_border = true;
	int tab_index = stoi(str_index) - 1;

	MY_CHECKPOS("tabheader", 0);

	if (parts.size() == 6 + i) {
		if (parts[4+i] == "true")
			show_background = false;
		if (parts[5+i] == "false")
			show_border = false;
	}

	FieldSpec spec(
		name,
		L"",
		L"",
		258 + m_fields.size()
	);

	spec.ftype = f_TabHeader;

	v2s32 pos;
	v2s32 geom;

	if (data->real_coordinates) {
		pos = getRealCoordinateBasePos(v_pos);

		geom = getRealCoordinateGeometry(v_geom);
		// Set default height
		if (parts.size() <= 6)
			geom.Y = m_btn_height * 2;
		pos.Y -= geom.Y; // TabHeader base pos is the bottom, not the top.
		if (auto_width)
			geom.X = DesiredRect.getWidth(); // Set automatic width

		MY_CHECKGEOM("tabheader", 1);
	} else {
		v2f32 pos_f = pos_offset * spacing;
		pos_f.X += stof(v_pos[0]) * spacing.X;
		pos_f.Y += stof(v_pos[1]) * spacing.Y - m_btn_height * 2;
		pos = v2s32(pos_f.X, pos_f.Y);

		geom.Y = m_btn_height * 2;
		geom.X = DesiredRect.getWidth();
	}

	core::rect<s32> rect = core::rect<s32>(pos.X, pos.Y, pos.X+geom.X,
			pos.Y+geom.Y);

	gui::IGUITabControl *e = Environment->addTabControl(rect,
			data->current_parent, show_background, show_border, spec.fid);
	e->setAlignment(gui::EGUIA_UPPERLEFT, gui::EGUIA_UPPERLEFT,
			gui::EGUIA_UPPERLEFT, gui::EGUIA_LOWERRIGHT);
	e->setTabHeight(geom.Y);

	auto style = getDefaultStyleForElement("tabheader", name);

	spec.sound = style.get(StyleSpec::Property::SOUND, "");

	e->setNotClipped(style.getBool(StyleSpec::NOCLIP, true));

	for (const std::string &button : buttons) {
		auto tab = e->addTab(unescape_translate(unescape_string(
			utf8_to_wide(button))).c_str(), -1);
		if (style.isNotDefault(StyleSpec::BGCOLOR))
			tab->setBackgroundColor(style.getColor(StyleSpec::BGCOLOR));

		tab->setTextColor(style.getColor(StyleSpec::TEXTCOLOR, video::SColor(0xFFFFFFFF)));
	}

	if ((tab_index >= 0) &&
			(buttons.size() < INT_MAX) &&
			(tab_index < (int) buttons.size()))
		e->setActiveTab(tab_index);

	m_fields.push_back(spec);
	m_tabheader_upper_edge = MYMIN(m_tabheader_upper_edge, rect.UpperLeftCorner.Y);
}

void GUIFormSpecMenu::parseItemImageButton(parserData* data, const std::string &element)
{
	MY_CHECKCLIENT("item_image_button");

	std::vector<std::string> parts;
	if (!precheckElement("item_image_button", element, 5, 5, parts))
		return;

	std::vector<std::string> v_pos = split(parts[0],',');
	std::vector<std::string> v_geom = split(parts[1],',');
	std::string item_name = parts[2];
	std::string name = parts[3];
	std::string label = parts[4];

	label = unescape_string(label);
	item_name = unescape_string(item_name);

	MY_CHECKPOS("item_image_button",0);
	MY_CHECKGEOM("item_image_button",1);

	v2s32 pos;
	v2s32 geom;

	if (data->real_coordinates) {
		pos = getRealCoordinateBasePos(v_pos);
		geom = getRealCoordinateGeometry(v_geom);
	} else {
		pos = getElementBasePos(&v_pos);
		geom.X = (stof(v_geom[0]) * spacing.X) - (spacing.X - imgsize.X);
		geom.Y = (stof(v_geom[1]) * spacing.Y) - (spacing.Y - imgsize.Y);
	}

	core::rect<s32> rect = core::rect<s32>(pos.X, pos.Y, pos.X+geom.X, pos.Y+geom.Y);

	if(!data->explicit_size)
		warningstream << "invalid use of item_image_button without a size[] element" << std::endl;

	IItemDefManager *idef = m_client->idef();
	ItemStack item;
	item.deSerialize(item_name, idef);

	m_tooltips[name] =
		TooltipSpec(utf8_to_wide(item.getDefinition(idef).description),
					m_default_tooltip_bgcolor,
					m_default_tooltip_color);

	// the spec for the button
	FieldSpec spec_btn(
		name,
		utf8_to_wide(label),
		utf8_to_wide(item_name),
		258 + m_fields.size(),
		2
	);

	GUIButtonItemImage *e_btn = GUIButtonItemImage::addButton(Environment,
			rect, m_tsrc, data->current_parent, spec_btn.fid, spec_btn.flabel.c_str(),
			item_name, m_client);

	auto style = getStyleForElement("item_image_button", spec_btn.fname, "image_button");

	spec_btn.sound = style[StyleSpec::STATE_DEFAULT].get(StyleSpec::Property::SOUND, "");

	e_btn->setStyles(style);

	if (spec_btn.fname == m_focused_element) {
		Environment->setFocus(e_btn);
	}

	spec_btn.ftype = f_Button;
	rect += data->basepos-padding;
	m_fields.push_back(spec_btn);
}

void GUIFormSpecMenu::parseBox(parserData* data, const std::string &element)
{
	std::vector<std::string> parts;
	if (!precheckElement("box", element, 3, 3, parts))
		return;

	std::vector<std::string> v_pos = split(parts[0], ',');
	std::vector<std::string> v_geom = split(parts[1], ',');

	MY_CHECKPOS("box", 0);
	MY_CHECKGEOM("box", 1);

	v2s32 pos;
	v2s32 geom;

	if (data->real_coordinates) {
		pos = getRealCoordinateBasePos(v_pos);
		geom = getRealCoordinateGeometry(v_geom);
	} else {
		pos = getElementBasePos(&v_pos);
		geom.X = stof(v_geom[0]) * spacing.X;
		geom.Y = stof(v_geom[1]) * spacing.Y;
	}

	FieldSpec spec(
		"",
		L"",
		L"",
		258 + m_fields.size(),
		-2
	);
	spec.ftype = f_Box;

	auto style = getDefaultStyleForElement("box", spec.fname);

	video::SColor tmp_color;
	std::array<video::SColor, 4> colors;
	std::array<video::SColor, 4> bordercolors = {0x0, 0x0, 0x0, 0x0};
	std::array<s32, 4> borderwidths = {0, 0, 0, 0};

	if (parseColorString(parts[2], tmp_color, true, 0x8C)) {
		colors = {tmp_color, tmp_color, tmp_color, tmp_color};
	} else {
		colors = style.getColorArray(StyleSpec::COLORS, {0x0, 0x0, 0x0, 0x0});
		bordercolors = style.getColorArray(StyleSpec::BORDERCOLORS,
			{0x0, 0x0, 0x0, 0x0});
		borderwidths = style.getIntArray(StyleSpec::BORDERWIDTHS, {0, 0, 0, 0});
	}

	core::rect<s32> rect(pos, pos + geom);

	GUIBox *e = new GUIBox(Environment, data->current_parent, spec.fid, rect,
		colors, bordercolors, borderwidths);
	e->setNotClipped(style.getBool(StyleSpec::NOCLIP, m_formspec_version < 3));
	e->drop();

	m_fields.push_back(spec);
}

void GUIFormSpecMenu::parseBackgroundColor(parserData* data, const std::string &element)
{
	std::vector<std::string> parts;
	if (!precheckElement("bgcolor", element, 1, 3, parts))
		return;

	const u32 parameter_count = parts.size();

	if (parameter_count > 2 && m_formspec_version < 3) {
		errorstream << "Invalid bgcolor element(" << parameter_count << "): '"
				<< element << "'" << std::endl;
		return;
	}

	// bgcolor
	if (parameter_count >= 1 && !parts[0].empty())
		parseColorString(parts[0], m_bgcolor, false);

	// fullscreen
	if (parameter_count >= 2) {
		if (parts[1] == "both") {
			m_bgnonfullscreen = true;
			m_bgfullscreen = true;
		} else if (parts[1] == "neither") {
			m_bgnonfullscreen = false;
			m_bgfullscreen = false;
		} else if (!parts[1].empty() || m_formspec_version < 3) {
			m_bgfullscreen = is_yes(parts[1]);
			m_bgnonfullscreen = !m_bgfullscreen;
		}
	}

	// fbgcolor
	if (parameter_count >= 3 && !parts[2].empty())
		parseColorString(parts[2], m_fullscreen_bgcolor, false);
}

void GUIFormSpecMenu::parseListColors(parserData* data, const std::string &element)
{
	std::vector<std::string> parts;
	// Legacy Note: If clients older than 5.5.0-dev are supplied with additional arguments,
	// the tooltip colors will be ignored.
	if (!precheckElement("listcolors", element, 2, 5, parts))
		return;

	if (parts.size() == 4) {
		// Invalid argument combination
		errorstream << "Invalid listcolors element(" << parts.size() << "): '"
				<< element << "'" << std::endl;
		return;
	}

	parseColorString(parts[0], data->inventorylist_options.slotbg_n, false);
	parseColorString(parts[1], data->inventorylist_options.slotbg_h, false);

	if (parts.size() >= 3) {
		if (parseColorString(parts[2], data->inventorylist_options.slotbordercolor,
				false)) {
			data->inventorylist_options.slotborder = true;
		}
	}
	if (parts.size() >= 5) {
		video::SColor tmp_color;

		if (parseColorString(parts[3], tmp_color, false))
			m_default_tooltip_bgcolor = tmp_color;
		if (parseColorString(parts[4], tmp_color, false))
			m_default_tooltip_color = tmp_color;
	}

	// update all already parsed inventorylists
	for (GUIInventoryList *e : m_inventorylists) {
		e->setSlotBGColors(data->inventorylist_options.slotbg_n,
				data->inventorylist_options.slotbg_h);
		e->setSlotBorders(data->inventorylist_options.slotborder,
				data->inventorylist_options.slotbordercolor);
	}
}

void GUIFormSpecMenu::parseTooltip(parserData* data, const std::string &element)
{
	std::vector<std::string> parts;
	if (!precheckElement("tooltip", element, 2, 5, parts))
		return;

	// Get mode and check size
	bool rect_mode = parts[0].find(',') != std::string::npos;
	size_t base_size = rect_mode ? 3 : 2;
	if (parts.size() != base_size && parts.size() != base_size + 2) {
		errorstream << "Invalid tooltip element(" << parts.size() << "): '"
				<< element << "'"  << std::endl;
		return;
	}

	// Read colors
	video::SColor bgcolor = m_default_tooltip_bgcolor;
	video::SColor color   = m_default_tooltip_color;
	if (parts.size() == base_size + 2 &&
			(!parseColorString(parts[base_size], bgcolor, false) ||
				!parseColorString(parts[base_size + 1], color, false))) {
		errorstream << "Invalid color in tooltip element(" << parts.size()
				<< "): '" << element << "'"  << std::endl;
		return;
	}

	// Make tooltip spec
	std::string text = unescape_string(parts[rect_mode ? 2 : 1]);
	TooltipSpec spec(utf8_to_wide(text), bgcolor, color);

	// Add tooltip
	if (rect_mode) {
		std::vector<std::string> v_pos  = split(parts[0], ',');
		std::vector<std::string> v_geom = split(parts[1], ',');

		MY_CHECKPOS("tooltip", 0);
		MY_CHECKGEOM("tooltip", 1);

		v2s32 pos;
		v2s32 geom;

		if (data->real_coordinates) {
			pos = getRealCoordinateBasePos(v_pos);
			geom = getRealCoordinateGeometry(v_geom);
		} else {
			pos = getElementBasePos(&v_pos);
			geom.X = stof(v_geom[0]) * spacing.X;
			geom.Y = stof(v_geom[1]) * spacing.Y;
		}

		FieldSpec fieldspec(
			"",
			L"",
			L"",
			258 + m_fields.size()
		);

		core::rect<s32> rect(pos, pos + geom);

		gui::IGUIElement *e = new gui::IGUIElement(EGUIET_ELEMENT, Environment,
				data->current_parent, fieldspec.fid, rect);

		// the element the rect tooltip is bound to should not block mouse-clicks
		e->setVisible(false);

		m_fields.push_back(fieldspec);
		m_tooltip_rects.emplace_back(e, spec);

	} else {
		m_tooltips[parts[0]] = spec;
	}
}

bool GUIFormSpecMenu::parseVersionDirect(const std::string &data)
{
	//some prechecks
	if (data.empty())
		return false;

	std::vector<std::string> parts = split(data,'[');

	if (parts.size() < 2) {
		return false;
	}

	if (trim(parts[0]) != "formspec_version") {
		return false;
	}

	if (is_number(parts[1])) {
		m_formspec_version = mystoi(parts[1]);
		return true;
	}

	return false;
}

bool GUIFormSpecMenu::parseSizeDirect(parserData* data, const std::string &element)
{
	if (element.empty())
		return false;

	std::vector<std::string> parts = split(element,'[');

	if (parts.size() < 2)
		return false;

	auto type = trim(parts[0]);
	std::string description(trim(parts[1]));

	if (type != "size" && type != "invsize")
		return false;

	if (type == "invsize")
		warningstream << "Deprecated formspec element \"invsize\" is used" << std::endl;

	parseSize(data, description);

	return true;
}

bool GUIFormSpecMenu::parsePositionDirect(parserData *data, const std::string &element)
{
	if (element.empty())
		return false;

	std::vector<std::string> parts = split(element, '[');

	if (parts.size() != 2)
		return false;

	auto type = trim(parts[0]);
	std::string description(trim(parts[1]));

	if (type != "position")
		return false;

	parsePosition(data, description);

	return true;
}

void GUIFormSpecMenu::parsePosition(parserData *data, const std::string &element)
{
	std::vector<std::string> parts = split(element, ';');

	if (parts.size() == 1 ||
			(parts.size() > 1 && m_formspec_version > FORMSPEC_API_VERSION)) {
		std::vector<std::string> v_geom = split(parts[0], ',');

		MY_CHECKGEOM("position", 0);

		data->offset.X = stof(v_geom[0]);
		data->offset.Y = stof(v_geom[1]);
		return;
	}

	errorstream << "Invalid position element (" << parts.size() << "): '" << element << "'" << std::endl;
}

bool GUIFormSpecMenu::parseAnchorDirect(parserData *data, const std::string &element)
{
	if (element.empty())
		return false;

	std::vector<std::string> parts = split(element, '[');

	if (parts.size() != 2)
		return false;

	auto type = trim(parts[0]);
	std::string description(trim(parts[1]));

	if (type != "anchor")
		return false;

	parseAnchor(data, description);

	return true;
}

void GUIFormSpecMenu::parseAnchor(parserData *data, const std::string &element)
{
	std::vector<std::string> parts = split(element, ';');

	if (parts.size() == 1 ||
			(parts.size() > 1 && m_formspec_version > FORMSPEC_API_VERSION)) {
		std::vector<std::string> v_geom = split(parts[0], ',');

		MY_CHECKGEOM("anchor", 0);

		data->anchor.X = stof(v_geom[0]);
		data->anchor.Y = stof(v_geom[1]);
		return;
	}

	errorstream << "Invalid anchor element (" << parts.size() << "): '" << element
			<< "'" << std::endl;
}

bool GUIFormSpecMenu::parsePaddingDirect(parserData *data, const std::string &element)
{
	if (element.empty())
		return false;

	std::vector<std::string> parts = split(element, '[');

	if (parts.size() != 2)
		return false;

	auto type = trim(parts[0]);
	std::string description(trim(parts[1]));

	if (type != "padding")
		return false;

	parsePadding(data, description);

	return true;
}

void GUIFormSpecMenu::parsePadding(parserData *data, const std::string &element)
{
	std::vector<std::string> parts = split(element, ';');

	if (parts.size() == 1 ||
			(parts.size() > 1 && m_formspec_version > FORMSPEC_API_VERSION)) {
		std::vector<std::string> v_geom = split(parts[0], ',');

		MY_CHECKGEOM("padding", 0);

		data->padding.X = stof(v_geom[0]);
		data->padding.Y = stof(v_geom[1]);
		return;
	}

	errorstream << "Invalid padding element (" << parts.size() << "): '" << element
			<< "'" << std::endl;
}

void GUIFormSpecMenu::parseStyle(parserData *data, const std::string &element)
{
	if (data->type != "style" && data->type != "style_type") {
		errorstream << "Invalid style element type: '" << data->type << "'" << std::endl;
		return;
	}

	bool style_type = (data->type == "style_type");

	std::vector<std::string> parts = split(element, ';');

	if (parts.size() < 2) {
		errorstream << "Invalid style element (" << parts.size() << "): '" << element
					<< "'" << std::endl;
		return;
	}

	StyleSpec spec;

	// Parse properties
	for (size_t i = 1; i < parts.size(); i++) {
		size_t equal_pos = parts[i].find('=');
		if (equal_pos == std::string::npos) {
			errorstream << "Invalid style element (Property missing value): '" << element
						<< "'" << std::endl;
			return;
		}

		std::string propname = trim(parts[i].substr(0, equal_pos));
		std::string value    = trim(unescape_string(parts[i].substr(equal_pos + 1)));

		std::transform(propname.begin(), propname.end(), propname.begin(), ::tolower);

		StyleSpec::Property prop = StyleSpec::GetPropertyByName(propname);
		if (prop == StyleSpec::NONE) {
			if (property_warned.find(propname) != property_warned.end()) {
				warningstream << "Invalid style element (Unknown property " << propname << "): '"
						<< element
						<< "'" << std::endl;
				property_warned.insert(propname);
			}
			continue;
		}

		spec.set(prop, value);
	}

	std::vector<std::string> selectors = split(parts[0], ',');
	for (size_t sel = 0; sel < selectors.size(); sel++) {
		std::string selector(trim(selectors[sel]));

		// Copy the style properties to a new StyleSpec
		// This allows a separate state mask per-selector
		StyleSpec selector_spec = spec;

		// Parse state information, if it exists
		bool state_valid = true;
		size_t state_pos = selector.find(':');
		if (state_pos != std::string::npos) {
			std::string state_str = selector.substr(state_pos + 1);
			selector = selector.substr(0, state_pos);

			if (state_str.empty()) {
				errorstream << "Invalid style element (Invalid state): '" << element
					<< "'" << std::endl;
				state_valid = false;
			} else {
				std::vector<std::string> states = split(state_str, '+');
				for (std::string &state : states) {
					StyleSpec::State converted = StyleSpec::getStateByName(state);
					if (converted == StyleSpec::STATE_INVALID) {
						infostream << "Unknown style state " << state <<
							" in element '" << element << "'" << std::endl;
						state_valid = false;
						break;
					}

					selector_spec.addState(converted);
				}
			}
		}

		if (!state_valid) {
			// Skip this selector
			continue;
		}

		if (style_type) {
			theme_by_type[selector].push_back(selector_spec);
		} else {
			theme_by_name[selector].push_back(selector_spec);
		}

		// Backwards-compatibility for existing _hovered/_pressed properties
		if (selector_spec.hasProperty(StyleSpec::BGCOLOR_HOVERED)
				|| selector_spec.hasProperty(StyleSpec::BGIMG_HOVERED)
				|| selector_spec.hasProperty(StyleSpec::FGIMG_HOVERED)) {
			StyleSpec hover_spec;
			hover_spec.addState(StyleSpec::STATE_HOVERED);

			if (selector_spec.hasProperty(StyleSpec::BGCOLOR_HOVERED)) {
				hover_spec.set(StyleSpec::BGCOLOR, selector_spec.get(StyleSpec::BGCOLOR_HOVERED, ""));
			}
			if (selector_spec.hasProperty(StyleSpec::BGIMG_HOVERED)) {
				hover_spec.set(StyleSpec::BGIMG, selector_spec.get(StyleSpec::BGIMG_HOVERED, ""));
			}
			if (selector_spec.hasProperty(StyleSpec::FGIMG_HOVERED)) {
				hover_spec.set(StyleSpec::FGIMG, selector_spec.get(StyleSpec::FGIMG_HOVERED, ""));
			}

			if (style_type) {
				theme_by_type[selector].push_back(hover_spec);
			} else {
				theme_by_name[selector].push_back(hover_spec);
			}
		}
		if (selector_spec.hasProperty(StyleSpec::BGCOLOR_PRESSED)
				|| selector_spec.hasProperty(StyleSpec::BGIMG_PRESSED)
				|| selector_spec.hasProperty(StyleSpec::FGIMG_PRESSED)) {
			StyleSpec press_spec;
			press_spec.addState(StyleSpec::STATE_PRESSED);

			if (selector_spec.hasProperty(StyleSpec::BGCOLOR_PRESSED)) {
				press_spec.set(StyleSpec::BGCOLOR, selector_spec.get(StyleSpec::BGCOLOR_PRESSED, ""));
			}
			if (selector_spec.hasProperty(StyleSpec::BGIMG_PRESSED)) {
				press_spec.set(StyleSpec::BGIMG, selector_spec.get(StyleSpec::BGIMG_PRESSED, ""));
			}
			if (selector_spec.hasProperty(StyleSpec::FGIMG_PRESSED)) {
				press_spec.set(StyleSpec::FGIMG, selector_spec.get(StyleSpec::FGIMG_PRESSED, ""));
			}

			if (style_type) {
				theme_by_type[selector].push_back(press_spec);
			} else {
				theme_by_name[selector].push_back(press_spec);
			}
		}
	}

	return;
}

void GUIFormSpecMenu::parseSetFocus(parserData*, const std::string &element)
{
	std::vector<std::string> parts;
	if (!precheckElement("set_focus", element, 1, 2, parts))
		return;

	if (m_is_form_regenerated)
		return; // Never focus on resizing

	bool force_focus = parts.size() >= 2 && is_yes(parts[1]);
	if (force_focus || m_text_dst->m_formname != m_last_formname)
		setFocus(parts[0]);
}

void GUIFormSpecMenu::parseMap(parserData *data, const std::string &element)
{
	MY_CHECKCLIENT("map");

	// Points are passed as additional ';'-separated fields (like table[] cells),
	// because ';' is the formspec field separator — encoding markers inside a
	// single field with ';' would be miscounted as extra element fields. So the
	// field count is variable (pos;geom;name; then zero or more point fields)
	// and we don't cap the maximum.
	std::vector<std::string> parts;
	if (!precheckElement("map", element, 3, 10000, parts))
		return;

	std::vector<std::string> v_pos = split(parts[0], ',');
	std::vector<std::string> v_geom = split(parts[1], ',');
	std::string name = parts[2];

	MY_CHECKPOS("map", 0);
	MY_CHECKGEOM("map", 1);

	v2s32 pos;
	v2s32 geom;

	if (data->real_coordinates) {
		pos = getRealCoordinateBasePos(v_pos);
		geom = getRealCoordinateGeometry(v_geom);
	} else {
		pos = getElementBasePos(&v_pos);
		geom.X = stof(v_geom[0]) * (float)imgsize.X;
		geom.Y = stof(v_geom[1]) * (float)imgsize.Y;
	}

	if (!data->explicit_size)
		warningstream << "Invalid use of map without a size[] element" << std::endl;

	FieldSpec spec(
		name,
		L"",
		L"",
		258 + m_fields.size()
	);

	core::rect<s32> rect(pos, pos + geom);

	GUIMapElement *e = new GUIMapElement(Environment, data->current_parent,
			spec.fid, rect, m_client);

	// Field 4 (optional): zoom, as the number of world nodes spanned across the
	// map window. Empty => default. The element clamps it to its valid range.
	if (parts.size() > 3 && !parts[3].empty())
		e->setViewNodes(stoi(parts[3]));

	// Field 5 (optional): focus center "wx,wy,wz" — the map centers here instead
	// of following the player. Empty => follow the player.
	if (parts.size() > 4 && !parts[4].empty()) {
		std::vector<std::string> fc = split(parts[4], ',');
		if (fc.size() >= 3)
			// Clamp to the map limit before setFocus: the focus is later
			// narrowed float->s16 (floatToInt in GUIMapElement::draw), and an
			// out-of-range value from an untrusted formspec would be UB.
			e->setFocus(v3f(
				core::clamp(stof(fc[0]), -31000.f, 31000.f),
				core::clamp(stof(fc[1]), -31000.f, 31000.f),
				core::clamp(stof(fc[2]), -31000.f, 31000.f)));
	}

	// Field 6 (optional): player-marker icon texture. Empty => default dot.
	if (parts.size() > 5 && !parts[5].empty())
		e->setPlayerIcon(unescape_string(parts[5]));

	// Field 7 (optional): icon size in coordinate units, shared by POI icons and
	// the player icon. Empty/0 => the element's responsive default. Convert to
	// pixels via this element's own px-per-unit (works in both coordinate modes,
	// since geom is already in pixels).
	if (parts.size() > 6 && !parts[6].empty()) {
		const f32 units = stof(v_geom[0]);
		const f32 px_per_unit = units > 0.001f ? (f32)geom.X / units : (f32)imgsize.Y;
		// Clamp to [0, element width] so a stray/garbage size can't overflow the
		// s32 cast (and the 2*icon_half rect maths) into a degenerate rect.
		const f32 size_px = core::clamp(stof(parts[6]) * px_per_unit, 0.0f, (f32)geom.X);
		e->setIconSize((s32)size_px);
	}

	// Fields 8+ (optional): markers, each its own ';'-separated field of the
	// form "wx,wy,wz,#RRGGBB[,icon]". The optional 5th sub-field names an icon
	// texture drawn instead of the colour square (the colour is the fallback).
	if (parts.size() > 7) {
		std::vector<GUIMapElement::MapPoint> points;
		for (size_t i = 7; i < parts.size(); i++) {
			if (parts[i].empty())
				continue;
			std::vector<std::string> f = split(parts[i], ',');
			if (f.size() < 4)
				continue;
			GUIMapElement::MapPoint mp;
			// Clamp to the map limit like the focus field: the coords are
			// later narrowed float->s32 in GUIMapElement::draw, and an
			// out-of-range (or NaN, via core::clamp->finite bound) value from
			// an untrusted formspec would be UB.
			mp.world_pos = v3f(
					core::clamp(stof(f[0]), -31000.f, 31000.f),
					core::clamp(stof(f[1]), -31000.f, 31000.f),
					core::clamp(stof(f[2]), -31000.f, 31000.f));
			if (!parseColorString(f[3], mp.color, false))
				mp.color = video::SColor(255, 255, 0, 0);
			if (f.size() >= 5)
				mp.icon = unescape_string(f[4]);
			points.push_back(mp);
		}
		e->setPoints(std::move(points));
	}

	auto style = getDefaultStyleForElement("map", spec.fname);
	e->setNotClipped(style.getBool(StyleSpec::NOCLIP, false));

	e->drop();

	m_fields.push_back(spec);
}

#if IS_VOPI_ENGINE
namespace {
// VOPI extension — the `fit` field of model[], selecting how GUIScene frames
// the mesh. Syntax: `<mode>` or `<mode>:<fill>`.
//
// An empty (or absent) field leaves the GUIScene default alone, which is the
// pre-existing axis-aligned framing — that is what keeps every formspec that
// predates this parameter, notably the player skin preview, pixel-identical.
void apply_model_fit(GUIScene *e, const std::string &fit)
{
	if (fit.empty())
		return;

	std::string mode = fit;
	f32 fill = 1.0f;

	const size_t sep = fit.find(':');
	if (sep != std::string::npos) {
		mode = fit.substr(0, sep);
		// stof() yields 0 for anything unparseable and never returns a
		// non-finite value, so a single positivity test covers both.
		fill = stof(fit.substr(sep + 1));
		if (fill <= 0.f) {
			warningstream << "Invalid model element: fit fill '"
				<< fit.substr(sep + 1) << "' is not a positive number, "
				<< "using 1.0" << std::endl;
			fill = 1.0f;
		}
	}

	if (mode == "aabb")
		e->setFitMode(GUIScene::FitMode::AABB, fill);
	else if (mode == "silhouette")
		e->setFitMode(GUIScene::FitMode::SILHOUETTE, fill);
	else
		warningstream << "Invalid model element: unknown fit mode '"
			<< mode << "'" << std::endl;
}
} // namespace
#endif

void GUIFormSpecMenu::parseModel(parserData *data, const std::string &element)
{
	MY_CHECKCLIENT("model");

	// VOPI Engine adds one optional field (`fit`) past upstream's 10.
#if IS_VOPI_ENGINE
	const size_t args_max = 11;
#else
	const size_t args_max = 10;
#endif

	std::vector<std::string> parts;
	if (!precheckElement("model", element, 5, args_max, parts))
		return;

	// Avoid length checks by resizing
	if (parts.size() < args_max)
		parts.resize(args_max);

	std::vector<std::string> v_pos = split(parts[0], ',');
	std::vector<std::string> v_geom = split(parts[1], ',');
	std::string name = unescape_string(parts[2]);
	std::string meshstr = unescape_string(parts[3]);
	std::vector<std::string> textures = split(parts[4], ',');
	std::vector<std::string> vec_rot = split(parts[5], ',');
	bool inf_rotation = is_yes(parts[6]);
	bool mousectrl = is_yes(parts[7]) || parts[7].empty(); // default true
	std::vector<std::string> frame_loop = split(parts[8], ',');
	std::string speed = unescape_string(parts[9]);

	MY_CHECKPOS("model", 0);
	MY_CHECKGEOM("model", 1);

	v2s32 pos;
	v2s32 geom;

	if (data->real_coordinates) {
		pos = getRealCoordinateBasePos(v_pos);
		geom = getRealCoordinateGeometry(v_geom);
	} else {
		pos = getElementBasePos(&v_pos);
		geom.X = stof(v_geom[0]) * (float)imgsize.X;
		geom.Y = stof(v_geom[1]) * (float)imgsize.Y;
	}

	if (!data->explicit_size)
		warningstream << "invalid use of model without a size[] element" << std::endl;

	scene::IAnimatedMesh *mesh = m_client->getMesh(meshstr);

	if (!mesh) {
		errorstream << "Invalid model element: Unable to load mesh:"
				<< std::endl << "\t" << meshstr << std::endl;
		return;
	}

	FieldSpec spec(
		name,
		L"",
		L"",
		258 + m_fields.size()
	);

	core::rect<s32> rect(pos, pos + geom);

	GUIScene *e = new GUIScene(Environment, m_client->getSceneManager(),
			data->current_parent, rect, spec.fid);

	auto meshnode = e->setMesh(mesh);
	// m_client->getMesh() returned with refcount +1; setMesh() (via
	// addAnimatedMeshSceneNode) grabbed it again. Drop our caller-side
	// reference here so when the scene node is destroyed the mesh
	// refcount reaches zero and the underlying data is freed. Without
	// this every formspec refresh leaks one IAnimatedMesh — matches
	// the content_cao.cpp pattern for in-world meshes.
	mesh->drop();

	for (u32 i = 0; i < meshnode->getMaterialCount(); ++i) {
		const auto texture_idx = mesh->getTextureSlot(i);
		if (texture_idx >= textures.size())
			warningstream << "Invalid model element: Not enough textures" << std::endl;
		else
			e->setTexture(i, m_tsrc->getTexture(unescape_string(textures[texture_idx])));
	}

	if (vec_rot.size() >= 2)
		e->setRotation(v2f(stof(vec_rot[0]), stof(vec_rot[1])));

	e->enableContinuousRotation(inf_rotation);
	e->enableMouseControl(mousectrl);

	f32 frame_loop_begin = 0;
	// This will be clamped to the animation duration.
	f32 frame_loop_end = std::numeric_limits<f32>::infinity();

	if (frame_loop.size() == 2) {
	    frame_loop_begin = stof(frame_loop[0]);
	    frame_loop_end = stof(frame_loop[1]);
	}

	e->setFrameLoop(frame_loop_begin, frame_loop_end);
	e->setAnimationSpeed(stof(speed));

#if IS_VOPI_ENGINE
	apply_model_fit(e, unescape_string(parts[10]));
#endif

	auto style = getStyleForElement("model", spec.fname);
	e->setStyles(style);
	e->drop();

#if IS_VOPI_ENGINE
	// Index by name so later model_overlay[] entries can find this scene.
	// Skip empty names (model[] with no name can't be referenced anyway,
	// and "" key would conflict with other unnamed model[] elements).
	// Warn on duplicate names — overlays placed between two model[name=X]
	// entries would attach to the first, those after to the second; that
	// non-obvious ordering dependency is worth surfacing instead of
	// silently swallowing.
	if (!name.empty()) {
		if (m_scene_models.find(name) != m_scene_models.end()) {
			warningstream << "Duplicate model[] name '" << name
				<< "' — second declaration will shadow the first for "
				<< "model_overlay[] resolution" << std::endl;
		}
		m_scene_models[name] = e;
	}
#endif

	m_fields.push_back(spec);
}

#if IS_VOPI_ENGINE
// VOPI extension — `model_overlay[model_name;mesh;textures;bone;pos;rot;scale]`
//
// Attaches a secondary mesh to a previously-declared model[] by name. Position,
// rotation, scale are in bone-local coordinates — identical semantics to
// entity:set_attach() in-world, so the same _appearance numbers used for the
// world-side wearable can drive the preview without translation.
//
// Must appear AFTER the model[] it references; if the parent isn't found in
// m_scene_models the call logs and skips. Multiple overlays per model[] are
// supported — each call appends to the GUIScene's attachment list.
void GUIFormSpecMenu::parseModelOverlay(parserData *data, const std::string &element)
{
	MY_CHECKCLIENT("model_overlay");

	std::vector<std::string> parts;
	if (!precheckElement("model_overlay", element, 7, 7, parts))
		return;

	std::string model_name = unescape_string(parts[0]);
	std::string meshstr    = unescape_string(parts[1]);
	std::vector<std::string> textures = split(parts[2], ',');
	std::string bone_name  = unescape_string(parts[3]);
	std::vector<std::string> v_pos = split(parts[4], ',');
	std::vector<std::string> v_rot = split(parts[5], ',');
	std::vector<std::string> v_scl = split(parts[6], ',');

	auto it = m_scene_models.find(model_name);
	if (it == m_scene_models.end()) {
		errorstream << "Invalid model_overlay element: parent model '"
			<< model_name << "' not found (model_overlay[] must appear "
			<< "after its model[] in the formspec)" << std::endl;
		return;
	}
	GUIScene *scene = it->second;
	if (!scene)
		return;

	scene::IAnimatedMesh *mesh = m_client->getMesh(meshstr);
	if (!mesh) {
		errorstream << "Invalid model_overlay element: unable to load mesh:"
			<< std::endl << "\t" << meshstr << std::endl;
		return;
	}

	// Resolve texture file names to ITexture pointers. Order matches the
	// declaration; null entries are accepted (caller may pad with blank
	// strings to skip particular material slots, though typical usage
	// passes exactly one texture per slot).
	std::vector<video::ITexture *> resolved_textures;
	resolved_textures.reserve(textures.size());
	for (const auto &tex : textures) {
		std::string tex_name = unescape_string(tex);
		resolved_textures.push_back(tex_name.empty()
			? nullptr : m_tsrc->getTexture(tex_name));
	}

	// pos/rot/scale come straight from the (server-supplied) formspec
	// string. stof is mystof — it never throws, but parses "nan"/"inf"/
	// "1e9999" into non-finite floats and leaves large values unbounded.
	// Non-finite or huge transforms propagate through
	// getTransformedBoundingBox() into calcOptimalDistance() and push the
	// preview camera to infinity, blanking the model for the formspec's
	// lifetime. Sanitize every component: reject non-finite, clamp
	// magnitude; scale falls back to 1 (identity), pos/rot to 0.
	auto sane = [](f32 v, f32 lo, f32 hi, f32 fallback) {
		return std::isfinite(v) ? rangelim(v, lo, hi) : fallback;
	};

	v3f position(0.f, 0.f, 0.f);
	if (v_pos.size() >= 3) {
		position.X = sane(stof(v_pos[0]), -1000.f, 1000.f, 0.f);
		position.Y = sane(stof(v_pos[1]), -1000.f, 1000.f, 0.f);
		position.Z = sane(stof(v_pos[2]), -1000.f, 1000.f, 0.f);
	}

	v3f rotation(0.f, 0.f, 0.f);
	if (v_rot.size() >= 3) {
		rotation.X = sane(stof(v_rot[0]), -360.f, 360.f, 0.f);
		rotation.Y = sane(stof(v_rot[1]), -360.f, 360.f, 0.f);
		rotation.Z = sane(stof(v_rot[2]), -360.f, 360.f, 0.f);
	}

	v3f scale(1.f, 1.f, 1.f);
	if (v_scl.size() >= 3) {
		scale.X = sane(stof(v_scl[0]), 0.001f, 100.f, 1.f);
		scale.Y = sane(stof(v_scl[1]), 0.001f, 100.f, 1.f);
		scale.Z = sane(stof(v_scl[2]), 0.001f, 100.f, 1.f);
	}

	scene->addAttachment(mesh, resolved_textures, bone_name,
		position, rotation, scale);

	// Same mesh refcount discipline as parseModel — getMesh() handed us a
	// +1 ref, addAttachment's internal addAnimatedMeshSceneNode grabbed
	// again, drop our caller-side ref so the mesh frees when the scene
	// node goes away.
	mesh->drop();
}
#endif

void GUIFormSpecMenu::parseAllowClose(parserData *data, const std::string &element)
{
	m_allowclose = is_yes(element);
}

void GUIFormSpecMenu::removeAll()
{
#if IS_VOPI_ENGINE
	m_clock_labels.clear();
	m_clock_minute = -1;
#endif
#if IS_VOPI_ENGINE
	// GUIScene pointers in m_scene_models are about to be invalidated by
	// removeAllChildren(). Clear the index first so no stale lookup can
	// race with the cleanup (parseModelOverlay races aren't realistic
	// today, but cheap defensive hygiene).
	m_scene_models.clear();
#endif

#if IS_VOPI_ENGINE && (defined(__ANDROID__) || defined(__IOS__))
	// Drop any in-flight touch drag-to-scroll tracking before the scroll
	// containers it may reference are torn down. Lives here (not in
	// regenerateGui) so the destructor path is covered too.
	resetTouchScroll();
	// Same hygiene for the gesture-target index of read-only textareas: the
	// elements are about to be destroyed by removeAllChildren().
	m_scroll_textareas.clear();
#endif

	// Remove children
	removeAllChildren();
	removeTooltip();

#if IS_VOPI_ENGINE && (defined(__ANDROID__) || defined(__IOS__))
	removeItemSelectBackground();
#endif

	for (auto &table_it : m_tables)
		table_it.second->drop();
	for (auto &inventorylist_it : m_inventorylists)
		inventorylist_it->drop();
	for (auto &checkbox_it : m_checkboxes)
		checkbox_it.second->drop();
	for (auto &scrollbar_it : m_scrollbars)
		scrollbar_it.second->drop();
	for (auto &tooltip_rect_it : m_tooltip_rects)
		tooltip_rect_it.first->drop();
	for (auto &clickthrough_it : m_clickthrough_elements)
		clickthrough_it->drop();
	for (auto &scroll_container_it : m_scroll_containers)
		scroll_container_it.second->drop();
}

const std::unordered_map<std::string, std::function<void(GUIFormSpecMenu*, GUIFormSpecMenu::parserData *data,
	const std::string &description)>> GUIFormSpecMenu::element_parsers = {
		{"container",              &GUIFormSpecMenu::parseContainer},
		{"container_end",          &GUIFormSpecMenu::parseContainerEnd},
		{"list",                   &GUIFormSpecMenu::parseList},
		{"listring",               &GUIFormSpecMenu::parseListRing},
		{"checkbox",               &GUIFormSpecMenu::parseCheckbox},
		{"image",                  &GUIFormSpecMenu::parseImage},
		{"animated_image",         &GUIFormSpecMenu::parseAnimatedImage},
		{"item_image",             &GUIFormSpecMenu::parseItemImage},
		{"button",                 &GUIFormSpecMenu::parseButton},
		{"button_exit",            &GUIFormSpecMenu::parseButton},
		{"button_url",             &GUIFormSpecMenu::parseButton},
		{"button_url_exit",        &GUIFormSpecMenu::parseButton},
		{"button_key",             &GUIFormSpecMenu::parseButton},
		{"background",             &GUIFormSpecMenu::parseBackground},
		{"background9",            &GUIFormSpecMenu::parseBackground},
		{"tableoptions",           &GUIFormSpecMenu::parseTableOptions},
		{"tablecolumns",           &GUIFormSpecMenu::parseTableColumns},
		{"table",                  &GUIFormSpecMenu::parseTable},
		{"textlist",               &GUIFormSpecMenu::parseTextList},
		{"dropdown",               &GUIFormSpecMenu::parseDropDown},
		{"field_enter_after_edit", &GUIFormSpecMenu::parseFieldEnterAfterEdit},
		{"field_close_on_enter",   &GUIFormSpecMenu::parseFieldCloseOnEnter},
		{"pwdfield",               &GUIFormSpecMenu::parsePwdField},
		{"field",                  &GUIFormSpecMenu::parseField},
		{"textarea",               &GUIFormSpecMenu::parseField},
		{"hypertext",              &GUIFormSpecMenu::parseHyperText},
		{"label",                  &GUIFormSpecMenu::parseLabel},
#if IS_VOPI_ENGINE
		{"clock",                  &GUIFormSpecMenu::parseClock},
#endif
		{"vertlabel",              &GUIFormSpecMenu::parseVertLabel},
		{"item_image_button",      &GUIFormSpecMenu::parseItemImageButton},
		{"image_button",           &GUIFormSpecMenu::parseImageButton},
		{"image_button_exit",      &GUIFormSpecMenu::parseImageButton},
		{"tabheader",              &GUIFormSpecMenu::parseTabHeader},
		{"box",                    &GUIFormSpecMenu::parseBox},
		{"bgcolor",                &GUIFormSpecMenu::parseBackgroundColor},
		{"listcolors",             &GUIFormSpecMenu::parseListColors},
		{"tooltip",                &GUIFormSpecMenu::parseTooltip},
		{"scrollbar",              &GUIFormSpecMenu::parseScrollBar},
		{"real_coordinates",       &GUIFormSpecMenu::parseRealCoordinates},
		{"style",                  &GUIFormSpecMenu::parseStyle},
		{"style_type",             &GUIFormSpecMenu::parseStyle},
		{"scrollbaroptions",       &GUIFormSpecMenu::parseScrollBarOptions},
		{"scroll_container",       &GUIFormSpecMenu::parseScrollContainer},
		{"scroll_container_end",   &GUIFormSpecMenu::parseScrollContainerEnd},
		{"set_focus",              &GUIFormSpecMenu::parseSetFocus},
		{"model",                  &GUIFormSpecMenu::parseModel},
		{"map",                    &GUIFormSpecMenu::parseMap},
#if IS_VOPI_ENGINE
		{"model_overlay",          &GUIFormSpecMenu::parseModelOverlay},
#endif
		{"allow_close",            &GUIFormSpecMenu::parseAllowClose},
};


void GUIFormSpecMenu::parseElement(parserData* data, const std::string &element)
{
	//some prechecks
	if (element.empty())
		return;

	if (parseVersionDirect(element))
		return;

	size_t pos = element.find('[');
	if (pos == std::string::npos)
		return;

	std::string type = trim(element.substr(0, pos));
	std::string description = element.substr(pos+1);

	// They remain here due to bool flags, for now
	data->type = type;

	auto it = element_parsers.find(type);
	if (it != element_parsers.end()) {
		it->second(this, data, description);
		return;
	}


	// Ignore others
	infostream << "Unknown DrawSpec: type=" << type << ", data=\"" << description << "\""
			<< std::endl;
}

void GUIFormSpecMenu::regenerateGui(v2u32 screensize)
{
	// Useless to regenerate without a screensize
	if ((screensize.X <= 0) || (screensize.Y <= 0)) {
		return;
	}

	parserData mydata;

	// Preserve stuff only on same form, not on a new form.
	if (m_text_dst->m_formname == m_last_formname) {
		// Preserve tables/textlists
		for (auto &m_table : m_tables) {
			std::string tablename = m_table.first.fname;
			GUITable *table = m_table.second;
			mydata.table_dyndata[tablename] = table->getDynamicData();
		}

		// Preserve focus
		gui::IGUIElement *focused_element = Environment->getFocus();
		// Check recursively to cover elements inside e.g. scroll containers
		if (focused_element && isMyDescendant(focused_element)) {
			s32 focused_id = focused_element->getID();
			if (focused_id > ID_PROCEED_BTN) {
				for (const GUIFormSpecMenu::FieldSpec &field : m_fields) {
					if (field.fid == focused_id) {
						m_focused_element = field.fname;
						break;
					}
				}
			}
		}
	} else {
		// Don't keep old focus value
		m_focused_element = std::nullopt;
		// Discard active inventory list interaction
		m_held_mouse_button = BET_OTHER;
	}

	if (m_held_mouse_button != BET_OTHER) {
		// Inventory list interaction -> focus "this". See also: `setInitialFocus`
		m_focused_element = std::nullopt;
	}

	removeAll();

	mydata.size = v2s32(100, 100);
	mydata.screensize = screensize;
	mydata.offset = v2f32(0.5f, 0.5f);
	mydata.anchor = v2f32(0.5f, 0.5f);
	mydata.padding = v2f32(0.05f, 0.05f);
	mydata.simple_field_count = 0;

	// Base position of contents of form
	mydata.basepos = getBasePos();

	// the parent for the parsed elements
	mydata.current_parent = this;

	m_inventorylists.clear();
	m_tables.clear();
	m_checkboxes.clear();
	m_scrollbars.clear();
	m_fields.clear();
	m_tooltips.clear();
	m_tooltip_rects.clear();
	m_inventory_rings.clear();
	m_dropdowns.clear();
	m_scroll_containers.clear();
#if IS_VOPI_ENGINE && (defined(__ANDROID__) || defined(__IOS__))
	m_scroll_textareas.clear();
#endif
	theme_by_name.clear();
	theme_by_type.clear();
	m_clickthrough_elements.clear();
	field_enter_after_edit.clear();
	field_close_on_enter.clear();
	m_dropdown_index_event.clear();

	m_allowclose = m_default_allowclose;
	m_bgnonfullscreen = true;
	m_bgfullscreen = false;

	m_formspec_version = 1;
	m_bgcolor = video::SColor(140, 0, 0, 0);
	m_tabheader_upper_edge = 0;

	{
		v3f formspec_bgcolor = g_settings->getV3F("formspec_fullscreen_bg_color").value_or(v3f());
		m_fullscreen_bgcolor = video::SColor(
			(u8) clamp_u8(g_settings->getS32("formspec_fullscreen_bg_opacity")),
			clamp_u8(myround(formspec_bgcolor.X)),
			clamp_u8(myround(formspec_bgcolor.Y)),
			clamp_u8(myround(formspec_bgcolor.Z))
		);
	}

	m_default_tooltip_bgcolor = video::SColor(255,110,130,60);
	m_default_tooltip_color = video::SColor(255,255,255,255);

#if IS_VOPI_ENGINE
	video::IVideoDriver *driver = RenderingEngine::get_video_driver();
	std::string textures_path = porting::path_share + "/textures/base/pack/gui_pop_up/";
#if defined(__ANDROID__) || defined(__IOS__)
	{
		assert(!m_selected_item_bg);
		std::string slot_texture = porting::path_share +
			"/textures/base/pack/gui_common/gui_selected_slot.png";
		m_selected_item_bg = guienv->addImage(
			core::rect<s32>(0, 0, 1, 1), guiroot, -1, nullptr, true);
		m_selected_item_bg->setImage(driver->getTexture(slot_texture.c_str()));
		m_selected_item_bg->setScaleImage(true);
		m_selected_item_bg->setVisible(false);
		m_selected_item_bg->grab();
	}
#endif
#endif

	// Add tooltip
	{
		assert(!m_tooltip_element);

		// Note: parent != this so that the tooltip isn't clipped by the menu rectangle
		m_tooltip_element = gui::StaticText::add(Environment, L"",
			core::rect<s32>(0, 0, 110, 18));
#if IS_VOPI_ENGINE
		m_tooltip_element->enableOverrideColor(false);
#else
		m_tooltip_element->enableOverrideColor(true);
#endif
		m_tooltip_element->setBackgroundColor(m_default_tooltip_bgcolor);
#if IS_VOPI_ENGINE
		m_tooltip_element->setDrawBackground(false);
		m_tooltip_element->setDrawBorder(false);
#else
		m_tooltip_element->setDrawBackground(true);
		m_tooltip_element->setDrawBorder(true);
#endif
		m_tooltip_element->setOverrideColor(m_default_tooltip_color);
		m_tooltip_element->setTextAlignment(gui::EGUIA_CENTER, gui::EGUIA_CENTER);
		m_tooltip_element->setWordWrap(false);
		//we're not parent so no autograb for this one!
		m_tooltip_element->grab();

#if IS_VOPI_ENGINE
		assert(!m_tooltip_bg.isInitialized());
		m_tooltip_bg.init(guienv, driver, guiroot, textures_path, "gui_tooltip_bg", false);
#endif
	}

	std::vector<std::string> elements = split(m_formspec_string,']');
	unsigned int i = 0;

	/* try to read version from first element only */
	if (!elements.empty()) {
		if (parseVersionDirect(elements[0])) {
			i++;
		}
	}

	/* we need size first in order to calculate image scale */
	mydata.explicit_size = false;
	for (; i< elements.size(); i++) {
		if (!parseSizeDirect(&mydata, elements[i])) {
			break;
		}
	}

	/* "position" element is always after "size" element if it used */
	for (; i< elements.size(); i++) {
		if (!parsePositionDirect(&mydata, elements[i])) {
			break;
		}
	}

	/* "anchor" element is always after "position" (or  "size" element) if it used */
	for (; i< elements.size(); i++) {
		if (!parseAnchorDirect(&mydata, elements[i])) {
			break;
		}
	}

	/* "padding" element is always after "anchor" and previous if it is used */
	for (; i < elements.size(); i++) {
		if (!parsePaddingDirect(&mydata, elements[i])) {
			break;
		}
	}

	/* "no_prepend" element is always after "padding" and previous if it used */
	bool enable_prepends = true;
	for (; i < elements.size(); i++) {
		if (elements[i].empty())
			break;

		std::vector<std::string> parts = split(elements[i], '[');
		if (trim(parts[0]) == "no_prepend")
			enable_prepends = false;
		else
			break;
	}

	/* Copy of the "real_coordinates" element for after the form size. */
	mydata.real_coordinates = m_formspec_version >= 2;
	for (; i < elements.size(); i++) {
		std::vector<std::string> parts = split(elements[i], '[');
		auto name = trim(parts[0]);
		if (name != "real_coordinates" || parts.size() != 2)
			break; // Invalid format

		mydata.real_coordinates = is_yes(trim(parts[1]));
	}

	if (mydata.explicit_size) {
		// compute scaling for specified form size
		if (m_lock) {
			v2u32 current_screensize = RenderingEngine::get_video_driver()->getScreenSize();
			v2u32 delta = current_screensize - m_lockscreensize;

			if (current_screensize.Y > m_lockscreensize.Y)
				delta.Y /= 2;
			else
				delta.Y = 0;

			if (current_screensize.X > m_lockscreensize.X)
				delta.X /= 2;
			else
				delta.X = 0;

			offset = v2s32(delta.X,delta.Y);

			mydata.screensize = m_lockscreensize;
		} else {
			offset = v2s32(0,0);
		}

		double use_imgsize = calculateImgsize(mydata);

		// Everything else is scaled in proportion to the
		// inventory image size.  The inventory slot spacing
		// is 5/4 image size horizontally and 15/13 image size
		// vertically.	The padding around the form (incorporating
		// the border of the outer inventory slots) is 3/8
		// image size.	Font height (baseline to baseline)
		// is 2/5 vertical inventory slot spacing, and button
		// half-height is 7/8 of font height.
		imgsize = v2s32(use_imgsize, use_imgsize);
		spacing = v2f32(use_imgsize*5.0/4, use_imgsize*15.0/13);
		padding = v2s32(use_imgsize*3.0/8, use_imgsize*3.0/8);
		m_btn_height = use_imgsize*15.0/13 * 0.35;

#if IS_VOPI_ENGINE
		// VOPI: scale formspec fonts with the formspec's element size (imgsize),
		// NOT the screen. Every element is sized as units * imgsize, so making
		// font_px proportional to imgsize keeps text a constant FRACTION of the
		// UI on every device — design once (e.g. on Mac) and it stays
		// proportional on phones, no per-device calibration. Resizing the game
		// window still rescales text, because imgsize tracks the window
		// (calculateImgsize). Note: font_px itself is NOT constant across devices
		// (it grows with imgsize) — the text/UI RATIO is what stays fixed.
		//
		// FontEngine renders a *N-styled font at N * base * m_font_scale *
		// density * gui_scaling. Solving font_px = VOPI_FONT_IMGSIZE_RATIO *
		// imgsize for the scale gives the line below: base, density and
		// gui_scaling cancel because we divide by them here and FontEngine
		// multiplies by them at render time. (`base` is the FM_Standard size; if
		// mono/bold are configured to a different size, their *N text scales by
		// that size's ratio to standard — a non-issue while the defaults match.)
		{
			const float base = (float)std::max(1u, g_fontengine->getFontSize(FM_Standard));
			const float density = std::max(0.1f, RenderingEngine::getDisplayDensity());
			const float gui_scaling = g_settings->getFloat("gui_scaling", 0.5f, 42.0f);
			m_font_scale = std::max(0.3f,
				(float)(VOPI_FONT_IMGSIZE_RATIO * use_imgsize) / (base * density * gui_scaling));
		}
#else
		m_font_scale = 1.0f;
#endif

		m_font = getScaledDefaultFont();

		if (mydata.real_coordinates) {
			mydata.size = v2s32(
				mydata.invsize.X*imgsize.X,
				mydata.invsize.Y*imgsize.Y
			);
		} else {
			mydata.size = v2s32(
				padding.X*2+spacing.X*(mydata.invsize.X-1.0)+imgsize.X,
				padding.Y*2+spacing.Y*(mydata.invsize.Y-1.0)+imgsize.Y + m_btn_height*2.0/3.0
			);
		}

		DesiredRect = mydata.rect = core::rect<s32>(
				(s32)((f32)mydata.screensize.X * mydata.offset.X) - (s32)(mydata.anchor.X * (f32)mydata.size.X) + offset.X,
				(s32)((f32)mydata.screensize.Y * mydata.offset.Y) - (s32)(mydata.anchor.Y * (f32)mydata.size.Y) + offset.Y,
				(s32)((f32)mydata.screensize.X * mydata.offset.X) + (s32)((1.0 - mydata.anchor.X) * (f32)mydata.size.X) + offset.X,
				(s32)((f32)mydata.screensize.Y * mydata.offset.Y) + (s32)((1.0 - mydata.anchor.Y) * (f32)mydata.size.Y) + offset.Y
		);
	} else {
		// Non-size[] form must consist only of text fields and
		// implicit "Proceed" button.  Use default font, and
		// temporary form size which will be recalculated below.
		m_font = getScaledDefaultFont();
		m_btn_height = font_line_height(m_font) * 0.875;
		DesiredRect = core::rect<s32>(
			(s32)((f32)mydata.screensize.X * mydata.offset.X) - (s32)(mydata.anchor.X * 580.0),
			(s32)((f32)mydata.screensize.Y * mydata.offset.Y) - (s32)(mydata.anchor.Y * 300.0),
			(s32)((f32)mydata.screensize.X * mydata.offset.X) + (s32)((1.0 - mydata.anchor.X) * 580.0),
			(s32)((f32)mydata.screensize.Y * mydata.offset.Y) + (s32)((1.0 - mydata.anchor.Y) * 300.0)
		);
	}
	recalculateAbsolutePosition(false);
	mydata.basepos = getBasePos();
	m_tooltip_element->setOverrideFont(getScaledTooltipFont());

	gui::IGUISkin *skin = Environment->getSkin();
	sanity_check(skin);
	gui::IGUIFont *old_font = skin->getFont();
	skin->setFont(m_font);

	// Add a new element that will hold all the background elements as its children.
	// Because it is the first added element, all backgrounds will be behind all
	// the other elements.
	// (We use an arbitrarily big rect. The actual size is determined later by
	// clipping to `this`.)
	core::rect<s32> background_parent_rect(0, 0, 100000, 100000);
	mydata.background_parent.reset(new gui::IGUIElement(EGUIET_ELEMENT, Environment,
			this, -1, background_parent_rect));

	pos_offset = v2f32();

	// used for formspec versions < 3
	auto legacy_sort_start = std::prev(Children.end()); // last element

	if (enable_prepends) {
		// Backup the coordinates so that prepends can use the coordinates of choice.
		bool rc_backup = mydata.real_coordinates;
		u16 version_backup = m_formspec_version;
		mydata.real_coordinates = false; // Old coordinates by default.

		std::vector<std::string> prepend_elements = split(m_formspec_prepend, ']');
		for (const auto &element : prepend_elements)
			parseElement(&mydata, element);

		// legacy sorting for formspec versions < 3
		if (m_formspec_version >= 3)
			// prepends do not need to be reordered
			legacy_sort_start = std::prev(Children.end()); // last element
		else if (version_backup >= 3)
			// only prepends elements have to be reordered
			legacySortElements(legacy_sort_start);

		m_formspec_version = version_backup;
		mydata.real_coordinates = rc_backup; // Restore coordinates
	}

	for (; i< elements.size(); i++) {
		parseElement(&mydata, elements[i]);
	}

	if (mydata.current_parent != this) {
		errorstream << "Invalid formspec string: scroll_container was never closed!"
			<< std::endl;
	} else if (!container_stack.empty()) {
		errorstream << "Invalid formspec string: container was never closed!"
			<< std::endl;
	}

	// get the scrollbar elements for scroll_containers
	for (const std::pair<std::string, GUIScrollContainer *> &c : m_scroll_containers) {
		for (const std::pair<FieldSpec, GUIScrollBar *> &b : m_scrollbars) {
			if (c.first == b.first.fname) {
				c.second->setScrollBar(b.second);
				b.second->setPos(b.first.aux_f32); // scroll position
				c.second->updateScrolling();
				break;
			}
		}
	}

	// If there are fields without explicit size[], add a "Proceed"
	// button and adjust size to fit all the fields.
	if (mydata.simple_field_count > 0 && !mydata.explicit_size) {
		mydata.rect = core::rect<s32>(
				mydata.screensize.X / 2 - 580 / 2,
				mydata.screensize.Y / 2 - 300 / 2,
				mydata.screensize.X / 2 + 580 / 2,
				mydata.screensize.Y / 2 + 240 / 2 + mydata.simple_field_count * 60
		);

		DesiredRect = mydata.rect;
		recalculateAbsolutePosition(false);
		mydata.basepos = getBasePos();

		{
			v2s32 pos = mydata.basepos;
			pos.Y = (mydata.simple_field_count + 2) * 60;

			v2s32 size = DesiredRect.getSize();
			mydata.rect = core::rect<s32>(
					size.X / 2 - 70,       pos.Y,
					size.X / 2 - 70 + 140, pos.Y + m_btn_height * 2
			);
			GUIButton::addButton(Environment, mydata.rect, m_tsrc, this, ID_PROCEED_BTN,
					wstrgettext("Proceed").c_str());
		}
	}

	// Set initial focus if parser didn't set it
	gui::IGUIElement *focused_element = Environment->getFocus();
	if (!focused_element
			|| !isMyDescendant(focused_element)
			|| focused_element->getType() == gui::EGUIET_TAB_CONTROL)
		setInitialFocus();

	skin->setFont(old_font);

	// legacy sorting
	if (m_formspec_version < 3)
		legacySortElements(legacy_sort_start);

	// Formname and regeneration setting
	if (!m_is_form_regenerated) {
		// Only set previous form name if we purposefully showed a new formspec
		m_last_formname = m_text_dst->m_formname;
		m_is_form_regenerated = true;
	}
}

void GUIFormSpecMenu::legacySortElements(std::list<IGUIElement *>::iterator from)
{
	/*
		Draw order for formspec_version <= 2:
		-3  bgcolor
		-2  background
		-1  box
		0   All other elements
		1   image
		2   item_image, item_image_button
		3   list
		4   label
	*/

	if (from == Children.end())
		from = Children.begin();
	else
		++from;

	auto to = Children.end();
	// 1: Copy into a sortable container
	std::vector<IGUIElement *> elements(from, to);

	// 2: Sort the container
	std::stable_sort(elements.begin(), elements.end(),
			[this] (const IGUIElement *a, const IGUIElement *b) -> bool {
		// TODO: getSpecByID is a linear search. It should made O(1), or cached here.
		const FieldSpec *spec_a = getSpecByID(a->getID());
		const FieldSpec *spec_b = getSpecByID(b->getID());
		// The comparison has to be compatible with strict weak ordering
		if (spec_a && spec_b)
			return spec_a->priority < spec_b->priority;

		if (spec_a && !spec_b)
			return true;

		return false;
	});

	// 3: Re-assign the pointers
	reorderChildren(from, to, elements);
}

#if defined(__ANDROID__) || defined(__IOS__)
void GUIFormSpecMenu::getAndroidUIInput()
{
	porting::AndroidDialogState dialogState = getAndroidUIInputState();
	if (dialogState == porting::DIALOG_SHOWN) {
		return;
	} else if (dialogState == porting::DIALOG_CANCELED) {
		m_jni_field_name.clear();
		return;
	}

	porting::AndroidDialogType dialog_type = porting::getLastInputDialogType();

	std::string fieldname = m_jni_field_name;
	m_jni_field_name.clear();

	for (const FieldSpec &field : m_fields) {
		if (field.fname != fieldname)
			continue; // Iterate until found

		IGUIElement *element = getElementFromId(field.fid, true);

		if (!element)
			return;

		auto element_type = element->getType();
		if (dialog_type == porting::TEXT_INPUT && element_type == gui::EGUIET_EDIT_BOX) {
			gui::IGUIEditBox *editbox = (gui::IGUIEditBox *)element;
			std::string text = porting::getInputDialogMessage();
			editbox->setText(utf8_to_wide(text).c_str());

			bool enter_after_edit = false;
			auto iter = field_enter_after_edit.find(fieldname);
			if (iter != field_enter_after_edit.end()) {
				enter_after_edit = iter->second;
			}
			if (enter_after_edit && editbox->getParent()) {
				SEvent enter;
				enter.EventType = EET_GUI_EVENT;
				enter.GUIEvent.Caller = editbox;
				enter.GUIEvent.Element = nullptr;
				enter.GUIEvent.EventType = gui::EGET_EDITBOX_ENTER;
				editbox->getParent()->OnEvent(enter);
			}
		} else if (dialog_type == porting::SELECTION_INPUT &&
				element_type == gui::EGUIET_COMBO_BOX) {
			auto dropdown = (gui::IGUIComboBox *) element;
			int selected = porting::getInputDialogSelection();
			dropdown->setAndSendSelected(selected);
		}

		return; // Early-return after found
	}
}
#endif

GUIInventoryList::ItemSpec GUIFormSpecMenu::getItemAtPos(v2s32 p) const
{
	for (const GUIInventoryList *e : m_inventorylists) {
		s32 item_index = e->getItemIndexAtPos(p);
#if IS_VOPI_ENGINE
		if (item_index != -1) {
			// We get the slot rectangle by index
			core::rect<s32> slot_rect = e->getSlotRect(item_index);

			return GUIInventoryList::ItemSpec(
				e->getInventoryloc(),
				e->getListname(),
				item_index,
				e->getSlotSize(),
				slot_rect.UpperLeftCorner  // Add a slot position
			);
		}
#else
		if (item_index != -1)
			return GUIInventoryList::ItemSpec(e->getInventoryloc(), e->getListname(),
					item_index, e->getSlotSize());
#endif
	}

#if IS_VOPI_ENGINE
	return GUIInventoryList::ItemSpec(InventoryLocation(), "", -1, {0,0}, {0,0});
#else
	return GUIInventoryList::ItemSpec(InventoryLocation(), "", -1, {0,0});
#endif
}

void GUIFormSpecMenu::drawSelectedItem()
{
	video::IVideoDriver* driver = Environment->getVideoDriver();

	if (!m_selected_item) {
		// reset rotation time
		drawItemStack(driver, m_font, ItemStack(),
				core::rect<s32>(v2s32(0, 0), v2s32(0, 0)), NULL,
				m_client, IT_ROT_DRAGGED);
		return;
	}

	Inventory *inv = m_invmgr->getInventory(m_selected_item->inventoryloc);
	sanity_check(inv);
	InventoryList *list = inv->getList(m_selected_item->listname);
	sanity_check(list);
	ItemStack stack = list->getItem(m_selected_item->i);
	stack.count = m_selected_amount;

	v2s32 slotsize = m_selected_item->slotsize;
	core::rect<s32> imgrect(0, 0, slotsize.X, slotsize.Y);
#if IS_VOPI_ENGINE && (defined(__ANDROID__) || defined(__IOS__))
	core::rect<s32> rect;
	if (!m_selected_dragging) {
		// If you do not drag and drop, draw in the slot position
		rect = core::rect<s32>(
			m_selected_item->position.X,
			m_selected_item->position.Y,
			m_selected_item->position.X + slotsize.X,
			m_selected_item->position.Y + slotsize.Y
		);
		if (m_selected_item_bg) {
			if (m_selected_active) {
				// Cached once at first call (lazy static init). Defaults registered
				// in defaultsettings.cpp with platform-specific values.
				// Sanity-clamped (±4096): they feed plain s32 rect arithmetic.
				static thread_local const s32 bg_padding  = rangelim(g_settings->getS32("selected_slot_bg_padding"), -4096, 4096);
				static thread_local const s32 bg_offset_x = rangelim(g_settings->getS32("selected_slot_bg_offset_x"), -4096, 4096);
				static thread_local const s32 bg_offset_y = rangelim(g_settings->getS32("selected_slot_bg_offset_y"), -4096, 4096);
				core::rect<s32> bg_rect(
					rect.UpperLeftCorner.X - bg_padding + bg_offset_x,
					rect.UpperLeftCorner.Y - bg_padding + bg_offset_y,
					rect.LowerRightCorner.X + bg_padding + bg_offset_x,
					rect.LowerRightCorner.Y + bg_padding + bg_offset_y
				);
				m_selected_item_bg->setRelativePosition(bg_rect);
				m_selected_item_bg->setVisible(true);
			} else {
				m_selected_item_bg->setVisible(false);
			}
			m_selected_item_bg->draw();
		}
	} else {
		// When dragging, follow the cursor
		rect = imgrect + (m_pointer - imgrect.getCenter());
		rect.constrainTo(driver->getViewPort());
	}
#else
	core::rect<s32> rect = imgrect + (m_pointer - imgrect.getCenter());
#endif
	rect.constrainTo(driver->getViewPort());
	drawItemStack(driver, m_font, stack, rect, NULL, m_client, IT_ROT_DRAGGED);
}

void GUIFormSpecMenu::drawMenu()
{
#if IS_VOPI_ENGINE
	// Updating the existing text nodes preserves edits, focus and scroll state.
	if (m_client && !m_clock_labels.empty()) {
		double time = m_client->getEnv().getTimeOfDayF();
		s32 minute = dayCycleMinute(time);
		if (minute != m_clock_minute) {
			m_clock_minute = minute;
			for (const auto &label : m_clock_labels)
				label.first->setText(utf8_to_wide(formatDayCycleTime(time, label.second)).c_str());
		}
	}
#endif
	if (m_form_src) {
		const std::string &newform = m_form_src->getForm();
		if (newform != m_formspec_string) {
			m_formspec_string = newform;
			m_is_form_regenerated = false;
			regenerateGui(m_screensize_old);
		}
	}

	gui::IGUISkin* skin = Environment->getSkin();
	sanity_check(skin != NULL);
	gui::IGUIFont *old_font = skin->getFont();
	skin->setFont(m_font);

	m_hovered_item_tooltips.clear();

	updateSelectedItem();

	// Auto-scroll to center focused element when Tab enables focus tracking
	autoScroll();

	video::IVideoDriver* driver = Environment->getVideoDriver();

	/*
		Draw background color
	*/
	v2u32 screenSize = driver->getScreenSize();
	core::rect<s32> allbg(0, 0, screenSize.X, screenSize.Y);

	if (m_bgfullscreen)
		driver->draw2DRectangle(m_fullscreen_bgcolor, allbg, &allbg);
	if (m_bgnonfullscreen)
		driver->draw2DRectangle(m_bgcolor, AbsoluteRect, &AbsoluteClippingRect);

	/*
		Draw rect_mode tooltip
	*/
	m_tooltip_element->setVisible(false);

#if IS_VOPI_ENGINE
	m_tooltip_bg.setVisible(false);
#endif

	for (const auto &pair : m_tooltip_rects) {
		const core::rect<s32> &rect = pair.first->getAbsoluteClippingRect();
		if (rect.getArea() > 0 && rect.isPointInside(m_pointer)) {
			const std::wstring &text = pair.second.tooltip;
			if (!text.empty()) {
				showTooltip(text, pair.second.color, pair.second.bgcolor);
				break;
			}
		}
	}

	// Some elements are only visible while being drawn
	for (gui::IGUIElement *e : m_clickthrough_elements)
		e->setVisible(true);

	/*
		This is where all the drawing happens.
	*/
	for (auto child : Children)
		if (child->isNotClipped() ||
				AbsoluteClippingRect.isRectCollided(
						child->getAbsolutePosition()))
			child->draw();

	for (gui::IGUIElement *e : m_clickthrough_elements)
		e->setVisible(false);

	// Draw hovered item tooltips
	for (const std::string &tooltip : m_hovered_item_tooltips) {
#if IS_VOPI_ENGINE
		showTooltip(utf8_to_wide(tooltip), m_default_tooltip_color, m_default_tooltip_bgcolor);
#else
		showTooltip(utf8_to_wide(tooltip), m_default_tooltip_color,
				m_default_tooltip_bgcolor);
#endif
	}

	if (m_hovered_item_tooltips.empty()) {
		// reset rotation time
		drawItemStack(driver, m_font, ItemStack(),
			core::rect<s32>(v2s32(0, 0), v2s32(0, 0)),
			NULL, m_client, IT_ROT_HOVERED);
	}

	/*
		Draw fields/buttons tooltips and update the mouse cursor
	*/
	gui::IGUIElement *hovered =
			Environment->getRootGUIElement()->getElementFromPoint(m_pointer);

	gui::ICursorControl *cursor_control = RenderingEngine::get_raw_device()->
			getCursorControl();
	gui::ECURSOR_ICON current_cursor_icon = gui::ECI_NORMAL;
	if (cursor_control)
		current_cursor_icon = cursor_control->getActiveIcon();

	bool hovered_element_found = false;

	if (hovered) {
		if (m_show_debug) {
			core::rect<s32> rect = hovered->getAbsoluteClippingRect();
			driver->draw2DRectangle(0x22FFFF00, rect, &rect);
		}

		// find the formspec-element of the hovered IGUIElement (a parent)
		s32 id;
		for (gui::IGUIElement *hovered_fselem = hovered; hovered_fselem;
				hovered_fselem = hovered_fselem->getParent()) {
			id = hovered_fselem->getID();
			if (id != -1)
				break;
		}

		u64 delta = 0;
		if (id == -1) {
			m_old_tooltip_id = id;
		} else {
			if (id == m_old_tooltip_id) {
				delta = porting::getDeltaMs(m_hovered_time, porting::getTimeMs());
			} else {
				m_hovered_time = porting::getTimeMs();
				m_old_tooltip_id = id;
			}
		}

		// Find and update the current tooltip and cursor icon
		if (id != -1) {
			for (const FieldSpec &field : m_fields) {

				if (field.fid != id)
					continue;

				if (delta >= m_tooltip_show_delay) {
					const std::wstring &text = m_tooltips[field.fname].tooltip;
					if (!text.empty())
						showTooltip(text, m_tooltips[field.fname].color,
							m_tooltips[field.fname].bgcolor);
				}

				if (cursor_control &&
						field.ftype != f_HyperText && // Handled directly in guiHyperText
						current_cursor_icon != field.fcursor_icon)
					cursor_control->setActiveIcon(field.fcursor_icon);

				hovered_element_found = true;

				break;
			}
		}
	}

	if (!hovered_element_found) {
		// no element is hovered
		if (cursor_control && current_cursor_icon != ECI_NORMAL)
			cursor_control->setActiveIcon(ECI_NORMAL);
	}

	// Draw white outline around keyboard-focused form elements.
	const gui::IGUIElement *focused = Environment->getFocus();
	if (focused && m_show_focus && focused->isTabStop() ) {
		core::rect<s32> rect = focused->getAbsoluteClippingRect();
		const video::SColor white(255, 255, 255, 255);
		const s32 border = 2;

		driver->draw2DRectangle(white,
			core::rect<s32>(rect.UpperLeftCorner.X, rect.UpperLeftCorner.Y,
				rect.LowerRightCorner.X, rect.UpperLeftCorner.Y + border), nullptr);
		driver->draw2DRectangle(white,
			core::rect<s32>(rect.UpperLeftCorner.X, rect.LowerRightCorner.Y - border,
				rect.LowerRightCorner.X, rect.LowerRightCorner.Y), nullptr);
		driver->draw2DRectangle(white,
			core::rect<s32>(rect.UpperLeftCorner.X, rect.UpperLeftCorner.Y,
				rect.UpperLeftCorner.X + border, rect.LowerRightCorner.Y), nullptr);
		driver->draw2DRectangle(white,
			core::rect<s32>(rect.LowerRightCorner.X - border, rect.UpperLeftCorner.Y,
				rect.LowerRightCorner.X, rect.LowerRightCorner.Y), nullptr);
	}

	// Draw dragged item stack
	drawSelectedItem();

	// Draw tooltip
	m_tooltip_element->draw();

#if IS_VOPI_ENGINE
	m_tooltip_bg.draw();
#endif

	skin->setFont(old_font);
}


void GUIFormSpecMenu::showTooltip(const std::wstring &text,
	const video::SColor &color, const video::SColor &bgcolor)
{
#if IS_VOPI_ENGINE
	setStaticText(m_tooltip_element, text);
	m_tooltip_element->setOverrideFont(getScaledTooltipFont());
#else
	EnrichedString ntext(text);
	ntext.setDefaultColor(color);
	if (!ntext.hasBackground())
		ntext.setBackground(bgcolor);
#endif

#if IS_VOPI_ENGINE
	// Cached once at first call (lazy static init). Defaults registered in
	// defaultsettings.cpp with platform-specific values.
	// Sanity-clamped (±4096): they feed plain s32 rect arithmetic.
	static thread_local const s32 corner_size    = rangelim(g_settings->getS32("tooltip_corner_size"), -4096, 4096);
	static thread_local const s32 padding_width  = rangelim(g_settings->getS32("tooltip_padding_width"), -4096, 4096);
	static thread_local const s32 padding_height = rangelim(g_settings->getS32("tooltip_padding_height"), -4096, 4096);
	static thread_local const s32 bg_offset_x    = rangelim(g_settings->getS32("tooltip_bg_offset_x"), -4096, 4096);
	static thread_local const s32 bg_offset_y    = rangelim(g_settings->getS32("tooltip_bg_offset_y"), -4096, 4096);
#else
	setStaticText(m_tooltip_element, ntext);
#endif

	// Tooltip size and offset
#if IS_VOPI_ENGINE
	s32 tooltip_width = m_tooltip_element->getTextWidth() + m_btn_height + 15 + padding_width;
	s32 tooltip_height = m_tooltip_element->getTextHeight() + 10 + padding_height;
#else
	s32 tooltip_width = m_tooltip_element->getTextWidth() + m_btn_height;
	s32 tooltip_height = m_tooltip_element->getTextHeight() + 5;
#endif

#if IS_VOPI_ENGINE && (defined(__ANDROID__) || defined(__IOS__))
	s32 tooltip_y = m_screensize_old.Y - ((m_screensize_old.Y - tooltip_height) / 5) - 25;
	s32 tooltip_x = (m_screensize_old.X - tooltip_width) / 2;
#else
	v2u32 screenSize = Environment->getVideoDriver()->getScreenSize();
	int tooltip_offset_x = m_btn_height;
	int tooltip_offset_y = m_btn_height;

	if (RenderingEngine::getLastPointerType() == PointerType::Touch) {
		tooltip_offset_x *= 3;
		tooltip_offset_y  = 0;
		if (m_pointer.X > (s32)screenSize.X / 2)
			tooltip_offset_x = -(tooltip_offset_x + tooltip_width);
	}

	// Calculate and set the tooltip position
	s32 tooltip_x = m_pointer.X + tooltip_offset_x;
	s32 tooltip_y = m_pointer.Y + tooltip_offset_y;
	// Bottom/Left limited positions (if the tooltip is too far out)
	s32 tooltip_x_alt = (s32)screenSize.X - tooltip_width  - m_btn_height;
	s32 tooltip_y_alt = (s32)screenSize.Y - tooltip_height - m_btn_height;

	int collision = (tooltip_x_alt < tooltip_x) + 2 * (tooltip_y_alt < tooltip_y);
	switch (collision) {
	case 1: // x
		tooltip_x = tooltip_x_alt;
		break;
	case 2: // y
		tooltip_y = tooltip_y_alt;
		break;
	case 3: // both
		tooltip_x = tooltip_x_alt;
		tooltip_y = (s32)screenSize.Y - 2 * tooltip_height - m_btn_height;
		break;
	default: // OK
		break;
	}
#endif

	m_tooltip_element->setRelativePosition(
		core::rect<s32>(
			core::position2d<s32>(tooltip_x, tooltip_y),
			core::dimension2d<s32>(tooltip_width, tooltip_height)
		)
	);

	// Display the tooltip
	m_tooltip_element->setVisible(true);
	bringToFront(m_tooltip_element);

#if IS_VOPI_ENGINE
	m_tooltip_bg.setPosition(
		core::rect<s32>(tooltip_x + bg_offset_x, tooltip_y + bg_offset_y,
			tooltip_x + tooltip_width + bg_offset_x,
			tooltip_y + tooltip_height + bg_offset_y),
		corner_size);
	m_tooltip_bg.setVisible(true);
#endif
}

void GUIFormSpecMenu::autoScroll()
{
	gui::IGUIElement *focus = Environment->getFocus();
	if (!m_show_focus || !focus)
		return;

	// Only process if focus changed or this is the first focus
	if (focus == m_last_focused && m_last_focused != nullptr)
		return;

	bool first_focus = (m_last_focused == nullptr);
	m_last_focused = focus;

	// Find the scroll container that contains the focused element
	for (const auto &cont : m_scroll_containers) {
		if (!cont.second->isMyDescendant(focus))
			continue;

		gui::IGUIElement *clipper = cont.second->getParent();
		if (!clipper)
			break;

		// Find scrollbars for this container
		GUIScrollBar *scrollbar_v = nullptr;
		GUIScrollBar *scrollbar_h = nullptr;
		for (const auto &sb : m_scrollbars) {
			if (sb.first.fname == cont.first) {
				if (sb.second->isHorizontal())
					scrollbar_h = sb.second;
				else
					scrollbar_v = sb.second;
			}
		}

		core::rect<s32> clip = clipper->getAbsoluteClippingRect();
		core::rect<s32> elem = focus->getAbsolutePosition();
		f32 scrollfactor = cont.second->getScrollFactor();

		if (scrollfactor == 0)
			break;

		// Handle vertical scrolling
		if (scrollbar_v) {
			bool visible = elem.UpperLeftCorner.Y >= clip.UpperLeftCorner.Y &&
						   elem.LowerRightCorner.Y <= clip.LowerRightCorner.Y;
			if (first_focus || !visible) {
				s32 target_y = elem.UpperLeftCorner.Y;
				if (elem.getHeight() < clip.getHeight())
					target_y = clip.UpperLeftCorner.Y + (clip.getHeight() - elem.getHeight()) / 2;

				s32 new_pos = scrollbar_v->getPos() + (s32)std::round((target_y - elem.UpperLeftCorner.Y) / scrollfactor);
				new_pos = rangelim(new_pos, scrollbar_v->getMin(), scrollbar_v->getMax());

				if (new_pos != scrollbar_v->getPos()) {
					scrollbar_v->setPos(new_pos);
					cont.second->updateScrolling();
				}
			}
		}

		// Handle horizontal scrolling
		if (scrollbar_h) {
			bool visible = elem.UpperLeftCorner.X >= clip.UpperLeftCorner.X &&
						   elem.LowerRightCorner.X <= clip.LowerRightCorner.X;
			if (first_focus || !visible) {
				s32 target_x = elem.UpperLeftCorner.X;
				if (elem.getWidth() < clip.getWidth())
					target_x = clip.UpperLeftCorner.X + (clip.getWidth() - elem.getWidth()) / 2;

				s32 new_pos = scrollbar_h->getPos() + (s32)std::round((target_x - elem.UpperLeftCorner.X) / scrollfactor);
				new_pos = rangelim(new_pos, scrollbar_h->getMin(), scrollbar_h->getMax());

				if (new_pos != scrollbar_h->getPos()) {
					scrollbar_h->setPos(new_pos);
					cont.second->updateScrolling();
				}
			}
		}

		break;
	}
}

void GUIFormSpecMenu::updateSelectedItem()
{
	// Don't update when dragging an item
	if (m_selected_item && (m_selected_dragging || m_left_dragging))
		return;

	verifySelectedItem();

	// If craftresult is not empty and nothing else is selected,
	// try to move it somewhere or select it now
	if (!m_selected_item || m_shift_move_after_craft) {
		for (const GUIInventoryList *e : m_inventorylists) {
			if (e->getListname() != "craftpreview")
				continue;

			Inventory *inv = m_invmgr->getInventory(e->getInventoryloc());
			if (!inv)
				continue;

			InventoryList *list = inv->getList("craftresult");

			if (!list || list->getSize() == 0)
				continue;

			const ItemStack &item = list->getItem(0);
			if (item.empty())
				continue;

			GUIInventoryList::ItemSpec s = GUIInventoryList::ItemSpec();
			s.inventoryloc = e->getInventoryloc();
			s.listname = "craftresult";
			s.i = 0;
			s.slotsize = e->getSlotSize();

			if (m_shift_move_after_craft) {
				// Try to shift-move the crafted item to the next list in the ring after the "craft" list.
				// We don't look for the "craftresult" list because it's a hidden list,
				// and shouldn't be part of the formspec, thus it won't be in the list ring.
				do {
					s16 r = getNextInventoryRing(s.inventoryloc, "craft");
					if (r < 0) // Not found
						break;

					const ListRingSpec &to_ring = m_inventory_rings[r];
					Inventory *inv_to = m_invmgr->getInventory(to_ring.inventoryloc);
					if (!inv_to)
						break;
					InventoryList *list_to = inv_to->getList(to_ring.listname);
					if (!list_to)
						break;

					IMoveAction *a = new IMoveAction();
					a->count = item.count;
					a->from_inv = s.inventoryloc;
					a->from_list = s.listname;
					a->from_i = s.i;
					a->to_inv = to_ring.inventoryloc;
					a->to_list = to_ring.listname;
					a->move_somewhere = true;
					m_invmgr->inventoryAction(a);
				} while (0);

				m_shift_move_after_craft = false;

			} else {
				// Grab selected item from the crafting result list
				m_selected_item = std::make_unique<GUIInventoryList::ItemSpec>(s);
				m_selected_amount = item.count;
				m_selected_dragging = false;
			}
			break;
		}
	}

	// If craftresult is selected, keep the whole stack selected
	if (m_selected_item && m_selected_item->listname == "craftresult")
		m_selected_amount = verifySelectedItem().count;

#if IS_VOPI_ENGINE && (defined(__ANDROID__) || defined(__IOS__))
	//Show tooltip window when m_selected_item != null
	if (m_selected_item && m_selected_active && !m_selected_dragging && m_tooltip_show) {
		u64 current_time = porting::getTimeMs();

		if (current_time - m_tooltip_show_time <= 2000) {
			Inventory *inv = m_invmgr->getInventory(m_selected_item->inventoryloc);
			InventoryList *list = inv ? inv->getList(m_selected_item->listname) : nullptr;
			if (list && (u32)m_selected_item->i < list->getSize())
				addHoveredItemTooltip(list->getItem(m_selected_item->i).getDescription(m_client->idef()));
		} else {
			m_tooltip_show = false;
			m_tooltip_show_time = 0;
		}
	}
#endif
}

ItemStack GUIFormSpecMenu::verifySelectedItem()
{
	// If the selected stack has become empty for some reason, deselect it.
	// If the selected stack has become inaccessible, deselect it.
	// If the selected stack has become smaller, adjust m_selected_amount.
	// Return the selected stack.

	if (m_selected_item) {
		if (m_selected_item->isValid()) {
			Inventory *inv = m_invmgr->getInventory(m_selected_item->inventoryloc);
			if (inv) {
				InventoryList *list = inv->getList(m_selected_item->listname);
				if (list && (u32) m_selected_item->i < list->getSize()) {
					ItemStack stack = list->getItem(m_selected_item->i);
					if (!m_selected_swap.empty()) {
						if (m_selected_swap.name == stack.name &&
								m_selected_swap.count == stack.count)
							m_selected_swap.clear();
					} else {
						m_selected_amount = std::min(m_selected_amount, stack.count);
					}

					if (!stack.empty())
						return stack;
				}
			}
		}

		// selection was not valid
		m_selected_item.reset();
		m_selected_amount = 0;
		m_selected_dragging = false;
	}
	return ItemStack();
}

s16 GUIFormSpecMenu::getNextInventoryRing(
		const InventoryLocation &inventoryloc, const std::string &listname)
{
	u16 rings = m_inventory_rings.size();
	if (rings < 2)
		return -1;
	// Look for the source ring
	s16 index = -1;
	for (u16 i = 0; i < rings; i++) {
		ListRingSpec &lr = m_inventory_rings[i];
		if (lr.inventoryloc == inventoryloc && lr.listname == listname) {
			// Set the index to the next ring
			index = (i + 1) % rings;
			break;
		}
	}
	return index;
}

void GUIFormSpecMenu::acceptInput(FormspecQuitMode quitmode)
{
	if(m_text_dst)
	{
		StringMap fields;

		if (quitmode == quit_mode_accept) {
			fields["quit"] = "true";
		} else if (quitmode == quit_mode_cancel) {
			fields["quit"] = "true";
			m_text_dst->gotText(fields);
			return;
		} else if (quitmode == quit_mode_try) {
			fields["try_quit"] = "true";
		}

		if (current_keys_pending.key_down) {
			fields["key_down"] = "true";
			current_keys_pending.key_down = false;
		}

		if (current_keys_pending.key_up) {
			fields["key_up"] = "true";
			current_keys_pending.key_up = false;
		}

		if (current_keys_pending.key_enter) {
			fields["key_enter"] = "true";
			current_keys_pending.key_enter = false;
		}

		if (!current_field_enter_pending.empty()) {
			fields["key_enter_field"] = current_field_enter_pending;
			current_field_enter_pending.clear();
		}

		for (const GUIFormSpecMenu::FieldSpec &s : m_fields) {
			if (s.send) {
				std::string name = s.fname;
				if (s.ftype == f_Button) {
					fields[name] = wide_to_utf8(s.flabel);
				} else if (s.ftype == f_Table) {
					GUITable *table = getTable(s.fname);
					if (table) {
						fields[name] = table->checkEvent();
					}
				} else if (s.ftype == f_DropDown) {
					IGUIElement *element = getElementFromId(s.fid, true);
					gui::IGUIComboBox *e = NULL;
					if ((element) && (element->getType() == gui::EGUIET_COMBO_BOX)) {
						e = static_cast<gui::IGUIComboBox *>(element);
					} else {
						warningstream << "GUIFormSpecMenu::acceptInput: dropdown "
								<< "field without dropdown element" << std::endl;
						continue;
					}
					s32 selected = e->getSelected();
					if (selected >= 0) {
						if (m_dropdown_index_event.find(s.fname) !=
								m_dropdown_index_event.end()) {
							fields[name] = std::to_string(selected + 1);
						} else {
							std::vector<std::string> *dropdown_values =
								getDropDownValues(s.fname);
							if (dropdown_values && selected < (s32)dropdown_values->size())
								fields[name] = (*dropdown_values)[selected];
						}
					}
				} else if (s.ftype == f_TabHeader) {
					IGUIElement *element = getElementFromId(s.fid, true);
					gui::IGUITabControl *e = nullptr;
					if ((element) && (element->getType() == gui::EGUIET_TAB_CONTROL)) {
						e = static_cast<gui::IGUITabControl *>(element);
					}

					if (e != 0) {
						fields[name] = itos(e->getActiveTab() + 1);
					}
				} else if (s.ftype == f_CheckBox) {
					IGUIElement *element = getElementFromId(s.fid, true);
					gui::IGUICheckBox *e = nullptr;
					if ((element) && (element->getType() == gui::EGUIET_CHECK_BOX)) {
						e = static_cast<gui::IGUICheckBox*>(element);
					}

					if (e != 0) {
						if (e->isChecked())
							fields[name] = "true";
						else
							fields[name] = "false";
					}
				} else if (s.ftype == f_ScrollBar) {
					IGUIElement *element = getElementFromId(s.fid, true);
					GUIScrollBar *e = nullptr;
					if (element && element->getType() == gui::EGUIET_SCROLL_BAR)
						e = static_cast<GUIScrollBar *>(element);

					if (e) {
						if (s.fdefault == L"Changed")
							fields[name] = "CHG:" + itos(e->getPos());
						else
							fields[name] = "VAL:" + itos(e->getPos());
					}
				} else if (s.ftype == f_AnimatedImage) {
					IGUIElement *element = getElementFromId(s.fid, true);
					GUIAnimatedImage *e = nullptr;
					if (element && element->getType() == gui::EGUIET_ELEMENT)
						e = static_cast<GUIAnimatedImage *>(element);

					if (e)
						fields[name] = std::to_string(e->getFrameIndex() + 1);
				} else {
					IGUIElement *e = getElementFromId(s.fid, true);
					if (e)
						fields[name] = wide_to_utf8(e->getText());
				}
			}
		}

		m_text_dst->gotText(fields);
	}
}

bool GUIFormSpecMenu::remapClickOutside(const SEvent &event)
{
	// Don't remap a click outside the formspec to ESC when holding an item.
	if (m_selected_item)
		return false;
	return GUIModalMenu::remapClickOutside(event);
}

#if IS_VOPI_ENGINE && (defined(__ANDROID__) || defined(__IOS__))

namespace {
	// Touch drag-to-scroll fling feel.
	// Smoothing window (ms) for the release-velocity estimate: a finger-move gap
	// at or above this fully trusts the newest sample (so a pause decays a stale
	// velocity), shorter gaps blend for smoothness.
	constexpr f32 TOUCH_SCROLL_VELOCITY_TAU_MS = 50.0f;
	// If the finger was motionless longer than this before lifting, the release
	// is a stop, not a flick (no fling).
	constexpr u64 TOUCH_SCROLL_FLING_STALE_MS = 70;
}

void GUIFormSpecMenu::resetTouchScroll()
{
	m_touch_scroll_phase = TouchScrollPhase::Inactive;
	m_touch_scroll_target = nullptr;
	m_touch_scroll_velocity = 0.0f;
	m_touch_scroll_caught_fling = false;
}

ITouchScrollTarget *GUIFormSpecMenu::findScrollableAt(v2s32 p) const
{
	// Read-only textareas are leaves: they can sit inside a scroll container
	// but never contain one, so a scrollable textarea under the finger always
	// wins over any container match. Test containment before isScrollable():
	// the latter measures the wrapped text, and only the box actually under
	// the finger should pay that.
	for (GUIEditBoxWithScrollBar *ta : m_scroll_textareas) {
		if (ta && ta->isTrulyVisible() &&
				ta->getAbsoluteClippingRect().isPointInside(p) &&
				ta->isScrollable())
			return ta;
	}

	// Pick the innermost (deepest) scrollable container whose viewport contains
	// the point. m_scroll_containers preserves declaration order and nested
	// containers are declared after their parent, so reverse iteration yields
	// the innermost match first.
	for (auto it = m_scroll_containers.rbegin(); it != m_scroll_containers.rend(); ++it) {
		GUIScrollContainer *c = it->second;
		if (c && c->isVisible() && c->isScrollable() &&
				c->getAbsoluteClippingRect().isPointInside(p))
			return c;
	}
	return nullptr;
}

bool GUIFormSpecMenu::handleTouchScroll(const SEvent &event)
{
	if (event.EventType != EET_TOUCH_INPUT_EVENT)
		return false;

	// Only single-finger gestures pan. Anything else (e.g. the two-finger
	// right-click) cancels tracking and is left to the normal pipeline.
	if (event.TouchInput.touchedCount != 1) {
		// A second finger while still Pending must not swallow the withheld
		// press: replay it so the child under the first finger still reacts
		// (unless the press was only catching an in-flight fling). The trailing
		// events flow through the normal pipeline once tracking is reset.
		if (m_touch_scroll_phase == TouchScrollPhase::Pending &&
				!m_touch_scroll_caught_fling)
			GUIModalMenu::preprocessEvent(m_touch_scroll_press);
		resetTouchScroll();
		return false;
	}

	const v2s32 pointer(event.TouchInput.X, event.TouchInput.Y);
	const size_t id = event.TouchInput.ID;

	switch (event.TouchInput.Event) {
	case ETIE_PRESSED_DOWN: {
		// If the finger landed on a scrollbar, leave it to the scrollbar's own
		// thumb/track handling instead of panning.
		for (const auto &sb : m_scrollbars) {
			if (sb.second && sb.second->isVisible() &&
					sb.second->getAbsoluteClippingRect().isPointInside(pointer)) {
				resetTouchScroll();
				return false;
			}
		}
		// Same for the built-in scrollbar of a read-only textarea.
		for (GUIEditBoxWithScrollBar *ta : m_scroll_textareas) {
			if (ta && ta->isTrulyVisible() && ta->isPointOverScrollbar(pointer)) {
				resetTouchScroll();
				return false;
			}
		}
		ITouchScrollTarget *target = findScrollableAt(pointer);
		if (!target) {
			resetTouchScroll();
			return false; // not over a scrollable container: normal handling
		}
		// Catching an in-flight fling: stop it now. The tap that catches the
		// momentum must not also activate a child (see ETIE_LEFT_UP).
		const bool caught = target->isFlinging();
		if (caught)
			target->stopFling();
		// Withhold the press; classify it on the following move/up events.
		m_touch_scroll_phase = TouchScrollPhase::Pending;
		m_touch_scroll_target = target;
		m_touch_scroll_id = id;
		m_touch_scroll_down_pos = pointer;
		m_touch_scroll_down_ms = porting::getTimeMs();
		m_touch_scroll_caught_fling = caught;
		m_touch_scroll_press = event;
		return true; // consume: do not forward to children yet
	}

	case ETIE_MOVED: {
		if (!isTrackingTouch(id))
			return false;

		// A finger that already went across the scroll axis (see below) is
		// neither a tap nor a pan: keep swallowing its motion.
		if (m_touch_scroll_phase == TouchScrollPhase::Cancelled)
			return true;

		if (m_touch_scroll_phase == TouchScrollPhase::Pending) {
			// Classify: did the finger travel past the threshold along the
			// scroll axis? The threshold scales with the formspec unit size.
			const v2s32 moved = pointer - m_touch_scroll_down_pos;
			s32 travel = m_touch_scroll_target->axisDelta(moved);
			if (travel < 0)
				travel = -travel;
			const s32 threshold = std::max<s32>(8, std::min(imgsize.X, imgsize.Y) / 4);
			if (travel < threshold) {
				// Scroll axis still ambiguous. But if the finger has instead
				// swiped across it past the same threshold, this is a
				// perpendicular swipe (e.g. horizontal drag over a full-width
				// row in a vertical list): cancel without ever replaying the
				// press, so it cannot activate the widget underneath on release.
				s32 cross = m_touch_scroll_target->crossAxisDelta(moved);
				if (cross < 0)
					cross = -cross;
				if (cross >= threshold) {
					m_touch_scroll_phase = TouchScrollPhase::Cancelled;
					return true;
				}
				return true; // still ambiguous: keep withholding
			}

			// Promote to panning. Anchor the origin here so the motion is
			// smooth (the threshold pixels are not counted as scroll).
			m_touch_scroll_phase = TouchScrollPhase::Scrolling;
			m_touch_scroll_origin_pos = pointer;
			m_touch_scroll_origin_scrollpos = m_touch_scroll_target->getScrollPos();
			// Seed the release-velocity estimate from the press->threshold
			// motion, so a short fast flick (few post-promote samples) still
			// launches at its real speed instead of from zero.
			const u64 now = porting::getTimeMs();
			const u64 dt0 = now - m_touch_scroll_down_ms;
			m_touch_scroll_velocity = dt0 > 0
					? (f32)m_touch_scroll_target->axisDelta(
							pointer - m_touch_scroll_down_pos) / (f32)dt0
					: 0.0f;
			m_touch_scroll_last_pos = pointer;
			m_touch_scroll_last_ms = now;
		} else {
			// Refine the smoothed finger speed (axis px/ms). The blend weight
			// grows with the time gap, so a pause-then-move decays a stale
			// velocity toward the slow motion instead of preserving it.
			const u64 now = porting::getTimeMs();
			const u64 dt = now - m_touch_scroll_last_ms;
			if (dt > 0) {
				const f32 inst = (f32)m_touch_scroll_target->axisDelta(
						pointer - m_touch_scroll_last_pos) / (f32)dt;
				const f32 w = std::min(1.0f,
						(f32)dt / TOUCH_SCROLL_VELOCITY_TAU_MS);
				m_touch_scroll_velocity += (inst - m_touch_scroll_velocity) * w;
				m_touch_scroll_last_pos = pointer;
				m_touch_scroll_last_ms = now;
			}
		}

		m_touch_scroll_target->scrollByPixels(m_touch_scroll_origin_scrollpos,
				pointer - m_touch_scroll_origin_pos);
		return true;
	}

	case ETIE_LEFT_UP: {
		if (!isTrackingTouch(id))
			return false;

		// A cancelled perpendicular swipe ends here: swallow the release without
		// replaying the press (no activation) and without launching a fling.
		if (m_touch_scroll_phase == TouchScrollPhase::Cancelled) {
			resetTouchScroll();
			return true;
		}

		const bool was_tap = m_touch_scroll_phase == TouchScrollPhase::Pending;
		const bool caught = m_touch_scroll_caught_fling;
		const SEvent press = m_touch_scroll_press;
		ITouchScrollTarget *target = m_touch_scroll_target;
		const f32 velocity = m_touch_scroll_velocity;
		// Ignore stale velocity if the finger paused before lifting.
		const bool moving = (porting::getTimeMs() - m_touch_scroll_last_ms)
				<= TOUCH_SCROLL_FLING_STALE_MS;
		resetTouchScroll();

		if (was_tap) {
			// A tap that caught an in-flight fling only stops it; it must not
			// activate the child underneath.
			if (caught)
				return true;
			// Plain tap: replay the withheld press so the child reacts, then
			// let this release flow through the normal pipeline.
			GUIModalMenu::preprocessEvent(press);
			return false;
		}
		// End of a pan: launch inertial scrolling if the finger was still moving.
		if (target && moving)
			target->startFling(velocity);
		return true; // end of a pan: swallow the release
	}

	default:
		return false;
	}
}

#endif

bool GUIFormSpecMenu::preprocessEvent(const SEvent& event)
{
#if IS_VOPI_ENGINE && (defined(__ANDROID__) || defined(__IOS__))
	// VOPI: touch drag-to-scroll. Must run before the base handler so the
	// finger-down can be withheld from child widgets until the gesture is
	// classified as a tap or a pan.
	if (handleTouchScroll(event))
		return true;
#endif

	// This must be done first so that GUIModalMenu can set m_pointer_type
	// correctly.
	if (GUIModalMenu::preprocessEvent(event))
		return true;

	// Handle keyboard and touch input to show/hide focus outline
	switch (event.EventType) {
	case EET_KEY_INPUT_EVENT:
		if (event.KeyInput.PressedDown && event.KeyInput.Key == KEY_TAB &&
				!event.KeyInput.Control) {
			m_show_focus = true;
			m_last_focused = nullptr;
		}
		break;
	case EET_MOUSE_INPUT_EVENT:
		switch (event.MouseInput.Event) {
		case EMIE_LMOUSE_PRESSED_DOWN:
		case EMIE_RMOUSE_PRESSED_DOWN:
		case EMIE_MMOUSE_PRESSED_DOWN:
			m_show_focus = false;
			m_last_focused = nullptr;
			break;
		default:
			break;
		}
		break;
	case EET_TOUCH_INPUT_EVENT:
		if (event.TouchInput.Event == ETIE_PRESSED_DOWN) {
			m_show_focus = false;
			m_last_focused = nullptr;
		}
		break;
	default:
		break;
	}

	// The IGUITabControl renders visually using the skin's selected
	// font, which we override for the duration of form drawing,
	// but computes tab hotspots based on how it would have rendered
	// using the font that is selected at the time of button release.
	// To make these two consistent, temporarily override the skin's
	// font while the IGUITabControl is processing the event.
	if (event.EventType == EET_MOUSE_INPUT_EVENT &&
			event.MouseInput.Event == EMIE_LMOUSE_LEFT_UP) {
		s32 x = event.MouseInput.X;
		s32 y = event.MouseInput.Y;
		gui::IGUIElement *hovered =
			Environment->getRootGUIElement()->getElementFromPoint(
				core::position2d<s32>(x, y));
		if (hovered && isMyDescendant(hovered) &&
				hovered->getType() == gui::EGUIET_TAB_CONTROL) {
			gui::IGUISkin* skin = Environment->getSkin();
			sanity_check(skin != NULL);
			gui::IGUIFont *old_font = skin->getFont();
			skin->setFont(m_font);
			bool retval = hovered->OnEvent(event);
			skin->setFont(old_font);
			// This is expected to be set to BET_OTHER with mouse UP event
			m_held_mouse_button = BET_OTHER;
			return retval;
		}
	}

	// Fix Esc/Return key being eaten by checkboxen and tables
	if (event.EventType == EET_KEY_INPUT_EVENT) {
			KeyPress kp(event.KeyInput);
		if (kp == EscapeKey
				|| keySettingHasMatch("keymap_inventory", kp)
				|| event.KeyInput.Key==KEY_RETURN) {
			gui::IGUIElement *focused = Environment->getFocus();
			if (focused && isMyDescendant(focused) &&
					(focused->getType() == gui::EGUIET_LIST_BOX ||
					focused->getType() == gui::EGUIET_CHECK_BOX) &&
					(focused->getParent()->getType() != gui::EGUIET_COMBO_BOX ||
					event.KeyInput.Key != KEY_RETURN)) {
				OnEvent(event);
				return true;
			}
		}
	}
	// Mouse wheel and move events: send to hovered element instead of focused
	if (event.EventType == EET_MOUSE_INPUT_EVENT &&
			(event.MouseInput.Event == EMIE_MOUSE_WHEEL ||
			(event.MouseInput.Event == EMIE_MOUSE_MOVED &&
			event.MouseInput.ButtonStates == 0))) {
		s32 x = event.MouseInput.X;
		s32 y = event.MouseInput.Y;
		gui::IGUIElement *hovered =
			Environment->getRootGUIElement()->getElementFromPoint(
				core::position2d<s32>(x, y));
		if (hovered && isMyDescendant(hovered)) {
			hovered->OnEvent(event);
			return event.MouseInput.Event == EMIE_MOUSE_WHEEL;
		}
	}

	if (event.EventType == EET_JOYSTICK_INPUT_EVENT) {
		if (event.JoystickEvent.Joystick != m_joystick->getJoystickId())
			return false;

		bool handled = m_joystick->handleEvent(event.JoystickEvent);
		if (handled) {
			if (m_joystick->wasKeyDown(KeyType::ESC)) {
				tryClose();
			} else if (m_joystick->wasKeyDown(KeyType::JUMP)) {
				trySubmitClose();
			}
		}
		return handled;
	}

	return false;
}

void GUIFormSpecMenu::tryClose()
{
	if (m_allowclose) {
		doPause = false;
		acceptInput(quit_mode_cancel);
		quitMenu();
	} else {
		acceptInput(quit_mode_try);
	}
}

void GUIFormSpecMenu::trySubmitClose()
{
	if (m_allowclose) {
		acceptInput(quit_mode_accept);
		quitMenu();
	} else {
		acceptInput(quit_mode_try);
	}
}

#if IS_VOPI_ENGINE && (defined(__ANDROID__) || defined(__IOS__))
void GUIFormSpecMenu::clearSelection()
{
	m_selected_swap.clear();
	m_selected_item.reset();
	m_selected_amount = 0;
	m_selected_dragging = false;
	m_selected_active = false;
	m_tooltip_show = false;
	m_tooltip_show_time = 0;
}
bool isPointerFarFromSlot(const v2s32& pointer_pos, const v2s32& slot_pos, const v2s32& slot_size)
{
	// Create a slot rectangle
	core::rect<s32> slot_rect(
		slot_pos.X,
		slot_pos.Y,
		slot_pos.X + slot_size.X,
		slot_pos.Y + slot_size.Y
	);

	// Add a small area around the slot
	slot_rect.UpperLeftCorner -= v2s32(5, 5);
	slot_rect.LowerRightCorner += v2s32(5, 5);

	// Check if the pointer has gone beyond this area
	return !slot_rect.isPointInside(pointer_pos);
}
#endif

bool GUIFormSpecMenu::OnEvent(const SEvent& event)
{
	if (event.EventType==EET_KEY_INPUT_EVENT) {
		KeyPress kp(event.KeyInput);
		// Ctrl (+ Shift) + Tab: Select the (previous or) next tab of a TabControl instance.
		bool shift = event.KeyInput.Shift;
		bool ctrl = event.KeyInput.Control;
		if (event.KeyInput.PressedDown && (event.KeyInput.Key == KEY_TAB && ctrl)) {
			// Try to find a tab control among our elements
			for (const FieldSpec &s : m_fields) {
				if (s.ftype != f_TabHeader)
					continue;

				IGUIElement *element = getElementFromId(s.fid, true);
				if (!element || element->getType() != gui::EGUIET_TAB_CONTROL)
					continue;

				gui::IGUITabControl *tabs = static_cast<gui::IGUITabControl *>(element);
				s32 num_tabs = tabs->getTabCount();
				if (num_tabs <= 1)
					continue;

				s32 active = tabs->getActiveTab();
				// Shift: Previous tab, No shift: Next tab
				active = (active + (shift ? -1 : 1) + num_tabs) % num_tabs;
				tabs->setActiveTab(active);
				return true; // handled
			}
		}
		if (event.KeyInput.PressedDown && (
				(kp == EscapeKey) ||
				((m_client != NULL) && (keySettingHasMatch("keymap_inventory", kp))))) {
			tryClose();
			return true;
		}

		if (event.KeyInput.PressedDown &&
				(keySettingHasMatch("keymap_screenshot", kp))) {
			if (m_client) {
				m_client->makeScreenshot();
			} else if (m_text_dst) { // in main menu
				m_text_dst->requestScreenshot();
			}
		}

		if (event.KeyInput.PressedDown && keySettingHasMatch("keymap_toggle_debug", kp)) {
			if (!m_client || m_client->checkPrivilege("debug"))
				m_show_debug = !m_show_debug;
		}

		if (event.KeyInput.PressedDown &&
			(event.KeyInput.Key==KEY_RETURN ||
			 event.KeyInput.Key==KEY_UP ||
			 event.KeyInput.Key==KEY_DOWN)
			) {
			switch (event.KeyInput.Key) {
				case KEY_RETURN:
					current_keys_pending.key_enter = true;
					break;
				case KEY_UP:
					current_keys_pending.key_up = true;
					break;
				case KEY_DOWN:
					current_keys_pending.key_down = true;
					break;
				break;
				default:
					//can't happen at all!
					FATAL_ERROR("Reached a source line that can't ever been reached");
					break;
			}
			if (current_keys_pending.key_enter) {
				trySubmitClose();
			} else {
				acceptInput();
			}
			return true;
		}

	}

#if IS_VOPI_ENGINE && (defined(__ANDROID__) || defined(__IOS__))
	if (event.EventType == EET_MOUSE_INPUT_EVENT &&
			event.MouseInput.Event == EMIE_MOUSE_MOVED &&
			m_selected_item && !m_selected_dragging && !m_selected_active) {
		if (isPointerFarFromSlot(m_pointer, m_selected_item->position, m_selected_item->slotsize)) {
			m_selected_dragging = true;
			m_tooltip_show = false;
			m_tooltip_show_time = 0;
		}
	}
#endif

	/* Mouse event other than movement, or crossing the border of inventory
	   field while holding left, right, or middle mouse button
	   or touch event (for touch screen devices)
	 */
	if ((event.EventType == EET_MOUSE_INPUT_EVENT &&
			(event.MouseInput.Event != EMIE_MOUSE_MOVED ||
				((event.MouseInput.isLeftPressed() ||
					event.MouseInput.isRightPressed() ||
					event.MouseInput.isMiddlePressed()) &&
				getItemAtPos(m_pointer).i != getItemAtPos(m_old_pointer).i))) ||
			event.EventType == EET_TOUCH_INPUT_EVENT) {

		// Get selected item and hovered/clicked item (s)

		m_old_tooltip_id = -1;
		updateSelectedItem();
		GUIInventoryList::ItemSpec s = getItemAtPos(m_pointer);

		Inventory *inv_selected = NULL;
		InventoryList *list_selected = NULL;
		Inventory *inv_s = NULL;
		InventoryList *list_s = NULL;

		if (m_selected_item) {
			inv_selected = m_invmgr->getInventory(m_selected_item->inventoryloc);
			sanity_check(inv_selected);
			list_selected = inv_selected->getList(m_selected_item->listname);
			sanity_check(list_selected);
		}

		u32 s_count = 0;

		if (s.isValid())
		do { // breakable
			inv_s = m_invmgr->getInventory(s.inventoryloc);

			if (!inv_s) {
				errorstream << "InventoryMenu: The selected inventory location "
						<< "\"" << s.inventoryloc.dump() << "\" doesn't exist"
						<< std::endl;
				s.i = -1;  // make it invalid again
				break;
			}

			list_s = inv_s->getList(s.listname);
			if (list_s == NULL) {
				verbosestream << "InventoryMenu: The selected inventory list \""
						<< s.listname << "\" does not exist" << std::endl;
				s.i = -1;  // make it invalid again
				break;
			}

			if ((u32)s.i >= list_s->getSize()) {
				infostream << "InventoryMenu: The selected inventory list \""
						<< s.listname << "\" is too small (i=" << s.i << ", size="
						<< list_s->getSize() << ")" << std::endl;
				s.i = -1;  // make it invalid again
				break;
			}

			s_count = list_s->getItem(s.i).count;
		} while(0);

		// True if the hovered slot is the selected slot
		bool identical = m_selected_item && s.isValid() && (*m_selected_item == s);

		// True if the hovered slot is empty
		bool empty = s.isValid() && list_s->getItem(s.i).empty();

		// True if the hovered item would stack with the selected item
		bool matching = false;
		if (m_selected_item && s.isValid()) {
			ItemStack a = list_selected->getItem(m_selected_item->i);
			ItemStack b = list_s->getItem(s.i);
			matching = a.stacksWith(b);
		}

		ButtonEventType button = BET_OTHER;
		ButtonEventType updown = BET_OTHER;
		bool mouse_shift = false;
		if (event.EventType == EET_MOUSE_INPUT_EVENT) {
			mouse_shift = event.MouseInput.Shift;
			switch (event.MouseInput.Event) {
			case EMIE_LMOUSE_PRESSED_DOWN:
				button = BET_LEFT; updown = BET_DOWN;
				break;
			case EMIE_RMOUSE_PRESSED_DOWN:
				button = BET_RIGHT; updown = BET_DOWN;
				break;
			case EMIE_MMOUSE_PRESSED_DOWN:
				button = BET_MIDDLE; updown = BET_DOWN;
				break;
			case EMIE_MOUSE_WHEEL:
				button = (event.MouseInput.Wheel > 0) ?
					BET_WHEEL_UP : BET_WHEEL_DOWN;
				updown = BET_DOWN;
				break;
			case EMIE_LMOUSE_LEFT_UP:
				button = BET_LEFT; updown = BET_UP;
				break;
			case EMIE_RMOUSE_LEFT_UP:
				button = BET_RIGHT; updown = BET_UP;
				break;
			case EMIE_MMOUSE_LEFT_UP:
				button = BET_MIDDLE; updown = BET_UP;
				break;
			case EMIE_MOUSE_MOVED:
				updown = BET_MOVE;
				break;
			default:
				break;
			}
		}

		// The second touch (see GUIModalMenu::preprocessEvent() function)
		ButtonEventType touch = BET_OTHER;
		if (event.EventType == EET_TOUCH_INPUT_EVENT) {
			if (event.TouchInput.Event == ETIE_LEFT_UP)
				touch = BET_RIGHT;
		}

		// Set this number to a positive value to generate a move action
		// from m_selected_item to s.
		u32 move_amount = 0;

		// Set this number to a positive value to generate a move action
		// from s to the next inventory ring.
		u32 shift_move_amount = 0;

		// Set this number to a positive value to generate a move action
		// from s to m_selected_item.
		u32 pickup_amount = 0;

		// Set this number to a positive value to generate a drop action
		// from m_selected_item.
		u32 drop_amount = 0;

		// Set this number to a positive value to generate a craft action at s.
		u32 craft_amount = 0;

		switch (updown) {
		case BET_DOWN: {
			// Some mouse button has been pressed

			if (m_held_mouse_button != BET_OTHER)
				break;

			if (button == BET_LEFT || button == BET_RIGHT || button == BET_MIDDLE)
				m_held_mouse_button = button;

#if IS_VOPI_ENGINE && (defined(__ANDROID__) || defined(__IOS__))
			if (!s.isValid()) {
				if (m_selected_item) {
					if (!getAbsoluteClippingRect().isPointInside(m_pointer)) {
						// Clicked outside of the window: drop
						drop_amount = m_selected_amount;
						m_selected_active = false;
						m_tooltip_show = false;
						m_tooltip_show_time = 0;
					} else {
						clearSelection();
					}
				}
				break;
			}
			if (s.listname == "craftpreview") {
				if (m_selected_item) {
					clearSelection();

					break;
				}
				// Craft preview has been clicked: craft
				craft_amount = 1;

				// Holding shift moves the crafted item to the inventory
				m_shift_move_after_craft = false;
			} else if (!m_selected_item && !empty) {
				// Non-empty stack has been clicked: select it
				m_selected_item = std::make_unique<GUIInventoryList::ItemSpec>(s);
				m_selected_amount = s_count;
			} else if (m_selected_item) {
				// Clicked a slot: move
				if (s.listname == "craft") {
					move_amount = 1;
				} else {
					move_amount = m_selected_amount;
					m_selected_active = false;
					m_tooltip_show = false;
					m_tooltip_show_time = 0;
				}

				if (identical) {
					// Change the selected amount instead of moving
					if (move_amount >= m_selected_amount)
						m_selected_amount = 0;
					else
						m_selected_amount -= move_amount;

					move_amount = 0;
					pickup_amount = 0;
				}
			}
#else
			if (!s.isValid()) {
				if (m_selected_item && !getAbsoluteClippingRect().isPointInside(m_pointer)) {
					// Clicked outside of the window: drop
					if (button == BET_RIGHT || button == BET_WHEEL_UP)
						drop_amount = 1;
					else if (button == BET_MIDDLE)
						drop_amount = MYMIN(m_selected_amount, 10);
					else if (button == BET_LEFT)
						drop_amount = m_selected_amount;
				}
				break;
			}
			if (s.listname == "craftpreview") {
				// Craft preview has been clicked: craft
				if (button == BET_MIDDLE)
					craft_amount = 10;
				else if (mouse_shift && button == BET_LEFT)
					craft_amount = list_s->getItem(s.i).getStackMax(m_client->idef());
				else
					craft_amount = 1;

				// Holding shift moves the crafted item to the inventory
				m_shift_move_after_craft = mouse_shift;

			} else if (!m_selected_item && button != BET_WHEEL_UP && !empty) {
				// Non-empty stack has been clicked: select or shift-move it
				u32 count = 0;
				if (button == BET_RIGHT)
					count = (s_count + 1) / 2;
				else if (button == BET_MIDDLE)
					count = MYMIN(s_count, 10);
				else if (button == BET_WHEEL_DOWN)
					count = 1;
				else if (button == BET_LEFT)
					count = s_count;

				if (mouse_shift) {
					// Shift pressed: move item, right click moves 1
					shift_move_amount = button == BET_RIGHT ? 1 : count;
				} else {
					// No shift: select item
					m_selected_item = std::make_unique<GUIInventoryList::ItemSpec>(s);
					m_selected_amount = count;
					m_selected_dragging = button != BET_WHEEL_DOWN;
				}

			} else if (m_selected_item) {
				// Clicked a slot: move
				if (button == BET_RIGHT || button == BET_WHEEL_UP)
					move_amount = 1;
				else if (button == BET_WHEEL_DOWN)
					pickup_amount = MYMIN(s_count, 1);
				else if (button == BET_MIDDLE)
					move_amount = MYMIN(m_selected_amount, 10);
				else if (button == BET_LEFT)
					move_amount = m_selected_amount;

				if (mouse_shift && !identical && matching) {
					// Shift-move all items the same as the selected item to the next list
					move_amount = 0;

					// Try to find somewhere to move the items to
					s16 r = getNextInventoryRing(s.inventoryloc, s.listname);
					if (r < 0) // Not found
						break;

					const ListRingSpec &to_ring = m_inventory_rings[r];
					Inventory *inv_to = m_invmgr->getInventory(to_ring.inventoryloc);
					if (!inv_to)
						break;
					InventoryList *list_to = inv_to->getList(to_ring.listname);
					if (!list_to)
						break;

					ItemStack slct = list_selected->getItem(m_selected_item->i);

					for (s32 i = 0; i < (s32)list_s->getSize(); i++) {
						// Skip the selected slot
						if (i == m_selected_item->i)
							continue;
						ItemStack item = list_s->getItem(i);

						if (slct.stacksWith(item)) {
							IMoveAction *a = new IMoveAction();
							a->count = item.count;
							a->from_inv = s.inventoryloc;
							a->from_list = s.listname;
							a->from_i = i;
							a->to_inv = to_ring.inventoryloc;
							a->to_list = to_ring.listname;
							a->move_somewhere = true;
							m_invmgr->inventoryAction(a);
						}
					}
				} else if (button == BET_LEFT && (empty || matching)) {
					// We don't know if the user is left-dragging, just moving
					// the item, or doing a pickup-all via doubleclick, so assume
					// that they are left-dragging, and wait for the next event
					// before moving the item, or doing a pickup-all
					m_left_dragging = true;
					m_client->inhibit_inventory_revert = true;
					m_left_drag_stack = list_selected->getItem(m_selected_item->i);
					m_left_drag_amount = m_selected_amount;
					m_left_drag_stacks.emplace_back(s, list_s->getItem(s.i));
					move_amount = 0;

				} else if (identical) {
					// Change the selected amount instead of moving
					if (button == BET_WHEEL_DOWN) {
						if (m_selected_amount < s_count)
							++m_selected_amount;
					} else if (button == BET_WHEEL_UP) {
						if (m_selected_amount > 0)
							--m_selected_amount;
					} else {
						if (move_amount >= m_selected_amount)
							m_selected_amount = 0;
						else
							m_selected_amount -= move_amount;
					}
					move_amount = 0;
					pickup_amount = 0;
				}
			}
#endif
			break;
		}
		case BET_UP: {
			// Some mouse button has been released

			if (m_held_mouse_button != BET_OTHER && m_held_mouse_button != button)
				break;
			m_held_mouse_button = BET_OTHER;

#if IS_VOPI_ENGINE && (defined(__ANDROID__) || defined(__IOS__))
			if (m_selected_dragging && m_selected_item) {
				if (s.isValid() && s.listname != "craftpreview" && s.listname != "craftresult") {
					if (!identical) {
						// Dragged to different slot: move all selected
						move_amount = m_selected_amount;
					} else {
						m_selected_amount = 0;

						m_selected_active = false;
						m_tooltip_show = false;
						m_tooltip_show_time = 0;
					}
				} else if (!getAbsoluteClippingRect().isPointInside(m_pointer)) {
					// Dragged outside of window: drop all selected
					drop_amount = m_selected_amount;
				} else {
					m_selected_amount = 0;
				}
			}
			if(!m_selected_dragging && m_selected_item) {
				if (s.isValid()) {
					if (s.listname != "craftpreview" && s.listname != "craftresult" && s.listname != "craft") {
						if (identical) {
							m_selected_active = true;
							if (!m_tooltip_show) {
								m_tooltip_show = true;
								m_tooltip_show_time = porting::getTimeMs();
							}
						} else {
							if (m_selected_active) {
								move_amount = m_selected_amount;
								m_selected_active = false;
								m_tooltip_show = false;
								m_tooltip_show_time = 0;
							} else {
								m_selected_amount = 0;
							}
						}
					} else {
						if (s.listname != "craft") {
							m_selected_amount = 0;
							m_selected_active = false;
							m_tooltip_show = false;
							m_tooltip_show_time = 0;
						} else {
							m_selected_active = true;
							if (!m_tooltip_show) {
								m_tooltip_show = true;
								m_tooltip_show_time = porting::getTimeMs();
							}
						}
					}
				} else {
					m_selected_amount = 0;
					m_selected_active = false;
					m_tooltip_show = false;
					m_tooltip_show_time = 0;
				}
			}
			m_selected_dragging = false;
#else

			if (m_selected_dragging && m_selected_item) {
				if (s.isValid() && !identical && (empty || matching)) {
					// Dragged to different slot: move all selected
					move_amount = m_selected_amount;

				} else if (!getAbsoluteClippingRect().isPointInside(m_pointer)) {
					// Dragged outside of window: drop all selected
					drop_amount = m_selected_amount;
				}
			}

			m_selected_dragging = false;

			if (m_left_dragging && button == BET_LEFT) {
				m_left_dragging = false;
				m_client->inhibit_inventory_revert = false;

				if (m_left_drag_stacks.size() > 1) {
					// Finalize the left-dragging
					for (auto &ds : m_left_drag_stacks) {
						if (ds.first == *m_selected_item) {
							// This entry is needed to properly calculate the stack sizes.
							// The stack already exists, hence no further action needed here.
							continue;
						}

						// Check how many items we should move to this slot,
						// it may be less than the full split
						Inventory *inv_to = m_invmgr->getInventory(ds.first.inventoryloc);
						InventoryList *list_to = inv_to->getList(ds.first.listname);
						ItemStack stack_to = list_to->getItem(ds.first.i);
						u16 amount = stack_to.count - ds.second.count;

						IMoveAction *a = new IMoveAction();
						a->count = amount;
						a->from_inv = m_selected_item->inventoryloc;
						a->from_list = m_selected_item->listname;
						a->from_i = m_selected_item->i;
						a->to_inv = ds.first.inventoryloc;
						a->to_list = ds.first.listname;
						a->to_i = ds.first.i;
						m_invmgr->inventoryAction(a);
					}

				} else if (identical) {
					// Put the selected item back where it came from
					m_selected_amount = 0;
				} else if (s.isValid()) {
					// Move the selected item
					move_amount = m_selected_amount;
				}

				m_left_drag_stacks.clear();
			}
#endif
			break;
		}
		case BET_MOVE: {
			// Mouse button is down and mouse pointer entered a new inventory field

			if (!s.isValid() || s.listname == "craftpreview")
				break;

#if IS_VOPI_ENGINE && (defined(__ANDROID__) || defined(__IOS__))
			if (m_selected_item && m_selected_dragging && matching && !identical) {
				// Pickup items of the same type while dragging
				pickup_amount = s_count;
			}
#else
			if (!m_selected_item && mouse_shift) {
				// Shift-move items while dragging
				if (m_held_mouse_button == BET_RIGHT)
					shift_move_amount = 1;
				else if (m_held_mouse_button == BET_MIDDLE)
					shift_move_amount = MYMIN(s_count, 10);
				else if (m_held_mouse_button == BET_LEFT)
					shift_move_amount = s_count;

			} else if (m_selected_item) {
				if (m_held_mouse_button != BET_LEFT) {
					// Move items if the destination slot is empty
					// or contains the same item type as what is going to be moved
					if (!m_selected_dragging && (empty || matching)) {
						if (m_held_mouse_button == BET_RIGHT)
							move_amount = 1;
						else if (m_held_mouse_button == BET_MIDDLE)
							move_amount = MYMIN(m_selected_amount, 10);
					}

				} else if (m_left_dragging && (empty || matching) &&
						m_left_drag_amount > m_left_drag_stacks.size()) {
					// Add the slot to the left-drag list if it doesn't exist
					bool found = false;
					for (auto &ds : m_left_drag_stacks) {
						if (s == ds.first) {
							found = true;
							break;
						}
					}
					if (!found) {
						m_left_drag_stacks.emplace_back(s, list_s->getItem(s.i));
					}

				} else if (m_selected_dragging && matching && !identical) {
					// Pickup items of the same type while dragging
					pickup_amount = s_count;
				}

			} else if (m_held_mouse_button == BET_LEFT) {
				// Start picking up items
				m_selected_item = std::make_unique<GUIInventoryList::ItemSpec>(s);
				m_selected_amount = s_count;
				m_selected_dragging = true;
			}
#endif
			break;
		}
		case BET_OTHER: {
#if !IS_VOPI_ENGINE && !defined(__ANDROID__) && !defined(__IOS__)
			// Some other mouse event has occured
			// Currently only left-double-click should trigger this
			if (!s.isValid() || event.EventType != EET_MOUSE_INPUT_EVENT ||
					event.MouseInput.Event != EMIE_LMOUSE_DOUBLE_CLICK)
				break;

			// Only do the pickup all thing when putting down an item.
			// Doubleclick events are triggered after press-down events, so if
			// m_left_dragging is true here, the user just put down an itemstack,
			// but didn't yet release the button to make it happen.
			if (!m_left_dragging)
				break;

			// Both the selected item and the hovered item need to be checked
			// because we don't know exactly when the double-click happened
			ItemStack slct;
			if (!m_selected_item && !empty)
				slct = list_s->getItem(s.i);
			else if (m_selected_item && (identical || empty))
				slct = list_selected->getItem(m_selected_item->i);

			// Pickup all of the item from the list
			if (slct.count > 0) {
				for (s32 i = 0; i < (s32)list_s->getSize(); i++) {
					// Skip the selected slot
					if (i == s.i)
						continue;

					ItemStack item = list_s->getItem(i);

					if (slct.stacksWith(item)) {
						// Found a match, check if we can pick it up
						bool full = false;
						u16 amount = item.count;
						ItemStack leftover = slct.addItem(item, m_client->idef());
						if (!leftover.empty()) {
							amount -= leftover.count;
							full = true;
						}

						if (amount > 0) {
							if (m_left_dragging) {
								// Abort left-dragging
								m_left_dragging = false;
								m_client->inhibit_inventory_revert = false;
								m_left_drag_stacks.clear();
							}
							IMoveAction *a = new IMoveAction();
							a->count = amount;
							a->from_inv = s.inventoryloc;
							a->from_list = s.listname;
							a->from_i = i;
							a->to_inv = s.inventoryloc;
							a->to_list = s.listname;
							a->to_i = s.i;
							m_invmgr->inventoryAction(a);

							if (m_selected_item)
								m_selected_amount += amount;
						}

						if (full) // Stack is full, stop
							break;
					}
				}
			}
#endif
			break;
		}
		default:
			break;
		}

		if (touch == BET_RIGHT && m_selected_item && !m_left_dragging) {
			if (!s.isValid()) {
				// Not a valid slot
				if (!getAbsoluteClippingRect().isPointInside(m_pointer))
					// Is outside the menu
					drop_amount = 1;
			} else {
				// Over a valid slot
				move_amount = 1;
				if (identical) {
					// Change the selected amount instead of moving
					if (move_amount >= m_selected_amount)
						m_selected_amount = 0;
					else
						m_selected_amount -= move_amount;
					move_amount = 0;
				}
			}
		}

		// Update left-dragged slots
		if (m_left_dragging && m_left_drag_stacks.size() > 1) {
			// The split amount will always at least one, because the number
			// of slots will never be greater than the selected amount
			u16 split_amount = m_left_drag_amount / m_left_drag_stacks.size();
			u16 split_remaining = m_left_drag_amount % m_left_drag_stacks.size();

			ItemStack stack_from = m_left_drag_stack;
			m_selected_amount = m_left_drag_amount;

			for (auto &ds : m_left_drag_stacks) {
				Inventory *inv_to = m_invmgr->getInventory(ds.first.inventoryloc);
				InventoryList *list_to = inv_to->getList(ds.first.listname);

				if (ds.first == *m_selected_item) {
					// Adding to the source stack, just change the selected amount
					m_selected_amount -= split_amount + split_remaining;

				} else {
					// Reset the stack to its original state
					list_to->changeItem(ds.first.i, ds.second);

					// Add the new split to the stack
					ItemStack add_stack = stack_from;
					add_stack.count = split_amount;
					ItemStack leftover = list_to->addItem(ds.first.i, add_stack);

					// Remove the split items from the source stack
					u16 moved = split_amount - leftover.count;
					m_selected_amount -= moved;
					stack_from.count -= moved;
				}
			}
			// Save the adjusted source stack
			list_selected->changeItem(m_selected_item->i, stack_from);
		}

		bool absorb_event = false;

		// Possibly send inventory action to server
		if (move_amount > 0) {
			// Send IAction::Move

			assert(m_selected_item && m_selected_item->isValid());
			assert(s.isValid());
			assert(list_selected && list_s);

			ItemStack stack_from = list_selected->getItem(m_selected_item->i);
			ItemStack stack_to = list_s->getItem(s.i);

			// Check how many items can be moved
			move_amount = stack_from.count = MYMIN(move_amount, stack_from.count);
			ItemStack leftover = stack_to.addItem(stack_from, m_client->idef());

			// If source stack cannot be added to destination stack at all,
			// they are swapped
			if (leftover.count == stack_from.count && leftover.name == stack_from.name) {
				if (m_selected_swap.empty()) {
					m_selected_amount = stack_to.count;
					m_selected_dragging = false;

					// WARNING: BLACK MAGIC, BUT IN A REDUCED SET
					// Skip next validation checks due async inventory calls
					m_selected_swap = stack_to;
				} else {
					move_amount = 0;
				}
			}
			// Source stack goes fully into destination stack
			else if (leftover.empty()) {
				m_selected_amount -= move_amount;
			}
			// Source stack goes partly into destination stack
			else {
				move_amount -= leftover.count;
				m_selected_amount -= move_amount;
			}

			if (move_amount > 0) {
				infostream << "Handing IAction::Move to manager" << std::endl;
				IMoveAction *a = new IMoveAction();
				a->count = move_amount;
				a->from_inv = m_selected_item->inventoryloc;
				a->from_list = m_selected_item->listname;
				a->from_i = m_selected_item->i;
				a->to_inv = s.inventoryloc;
				a->to_list = s.listname;
				a->to_i = s.i;
				m_invmgr->inventoryAction(a);
			}
		} else if (pickup_amount > 0) {
			// Send IAction::Move

			assert(m_selected_item && m_selected_item->isValid());
			assert(s.isValid());
			assert(list_selected && list_s);

			ItemStack stack_from = list_s->getItem(s.i);
			ItemStack stack_to = list_selected->getItem(m_selected_item->i);

			// Only move if the items are exactly the same,
			// we shouldn't attempt to pickup different items
			if (matching) {
#if IS_VOPI_ENGINE
				// VOPI: Don't allow pickup from detached inventories (like phone)
				// This prevents exploit where player holds item and clicks on same
				// item in phone tab to get free items
				if (s.inventoryloc.type == InventoryLocation::DETACHED) {
					pickup_amount = 0;
				} else {
#endif
					// Check how many items can be moved
					pickup_amount = stack_from.count = MYMIN(pickup_amount, stack_from.count);
					ItemStack leftover = stack_to.addItem(stack_from, m_client->idef());
					pickup_amount -= leftover.count;
#if IS_VOPI_ENGINE
				}
#endif
			} else {
				pickup_amount = 0;
			}

			if (pickup_amount > 0) {
				m_selected_amount += pickup_amount;

				infostream << "Handing IAction::Move to manager" << std::endl;
				IMoveAction *a = new IMoveAction();
				a->count = pickup_amount;
				a->from_inv = s.inventoryloc;
				a->from_list = s.listname;
				a->from_i = s.i;
				a->to_inv = m_selected_item->inventoryloc;
				a->to_list = m_selected_item->listname;
				a->to_i = m_selected_item->i;
				m_invmgr->inventoryAction(a);
			}
		} else if (shift_move_amount > 0) {
			// Try to shift-move the item
			do {
				s16 r = getNextInventoryRing(s.inventoryloc, s.listname);
				if (r < 0) // Not found
					break;

				const ListRingSpec &to_ring = m_inventory_rings[r];
				InventoryList *list_from = list_s;
				if (!s.isValid())
					break;
				Inventory *inv_to = m_invmgr->getInventory(to_ring.inventoryloc);
				if (!inv_to)
					break;
				InventoryList *list_to = inv_to->getList(to_ring.listname);
				if (!list_to)
					break;

				// Check how many items can be moved
				ItemStack stack_from = list_from->getItem(s.i);
				shift_move_amount = MYMIN(shift_move_amount, stack_from.count);
				if (shift_move_amount == 0)
					break;

				infostream << "Handing IAction::Move to manager" << std::endl;
				IMoveAction *a = new IMoveAction();
				a->count = shift_move_amount;
				a->from_inv = s.inventoryloc;
				a->from_list = s.listname;
				a->from_i = s.i;
				a->to_inv = to_ring.inventoryloc;
				a->to_list = to_ring.listname;
				a->move_somewhere = true;
				m_invmgr->inventoryAction(a);
			} while (0);

		} else if (drop_amount > 0) {
			// Send IAction::Drop

			assert(m_selected_item && m_selected_item->isValid());
			assert(list_selected);
			ItemStack stack_from = list_selected->getItem(m_selected_item->i);

			// Check how many items can be dropped
			drop_amount = stack_from.count = MYMIN(drop_amount, stack_from.count);
			assert(drop_amount > 0 && drop_amount <= m_selected_amount);
			m_selected_amount -= drop_amount;

			infostream << "Handing IAction::Drop to manager" << std::endl;
			IDropAction *a = new IDropAction();
			a->count = drop_amount;
			a->from_inv = m_selected_item->inventoryloc;
			a->from_list = m_selected_item->listname;
			a->from_i = m_selected_item->i;
			m_invmgr->inventoryAction(a);

			// Formspecs usually close when you click outside them, we absorb
			// the event to prevent that. See GUIModalMenu::remapClickOutside.
			absorb_event = true;

		} else if (craft_amount > 0) {
			assert(s.isValid());
#if IS_VOPI_ENGINE && (defined(__ANDROID__) || defined(__IOS__))
			// First, look for an empty slot
			bool found_slot = false;
			GUIInventoryList::ItemSpec target_slot;
			// Get the item from craftpreview for comparison
			Inventory *craft_inv = m_invmgr->getInventory(s.inventoryloc);
			if (!craft_inv)
			   return false;
			InventoryList *craft_list = craft_inv->getList("craftpreview");
			if (!craft_list || craft_list->getSize() == 0)
			   return false;

			ItemStack craft_item = craft_list->getItem(0);
			if (craft_item.empty())
			   return false;
			// Receive itemdef from the client
			IItemDefManager *idef = m_client->idef();
			// Search for a slot
			for (const GUIInventoryList *e : m_inventorylists) {
			   if (e->getListname() != "main" ||
				   e->getInventoryloc().type != InventoryLocation::CURRENT_PLAYER)
				   continue;

			   Inventory *inv = m_invmgr->getInventory(e->getInventoryloc());
			   if (!inv)
				   continue;

			   InventoryList *list = inv->getList(e->getListname());
			   if (!list)
				   continue;

			   // First, look for slots with the same subject
			   for (s32 i = 0; i < list->getSize(); i++) {
				   ItemStack stack = list->getItem(i);
				   if (!stack.empty() && stack.name == craft_item.name) {
					   // Check if there is enough space for the new item
					   s32 free_space = stack.getStackMax(idef) - stack.count;
					   if (free_space >= craft_item.count) {
						   core::rect<s32> rect = e->getSlotRect(i);

						   target_slot = GUIInventoryList::ItemSpec(
							   e->getInventoryloc(),
							   e->getListname(),
							   i,
							   e->getSlotSize(),
							   rect.UpperLeftCorner
						   );
						   found_slot = true;
						   break;
					   }
				   }
			   }

			   // If you do not find a suitable slot with the same item, look for an empty one
			   if (!found_slot) {
				   for (s32 i = 0; i < list->getSize(); i++) {
					   ItemStack stack = list->getItem(i);
					   if (stack.empty()) {
						   core::rect<s32> rect = e->getSlotRect(i);

						   target_slot = GUIInventoryList::ItemSpec(
							   e->getInventoryloc(),
							   e->getListname(),
							   i,
							   e->getSlotSize(),
							   rect.UpperLeftCorner
						   );
						   found_slot = true;
						   break;
					   }
				   }
			   }

			   if (found_slot)
				   break;
			}

			if (!found_slot) {
			   craft_amount = 0;
			   return false;
			}
			// If a slot is found, we craft it
			ICraftAction *a = new ICraftAction();
			a->count = craft_amount;
			a->craft_inv = s.inventoryloc;
			m_invmgr->inventoryAction(a);
			Inventory *result_inv = m_invmgr->getInventory(s.inventoryloc);
			if (result_inv) {
			   InventoryList *result_list = result_inv->getList("craftresult");
			   if (result_list && result_list->getSize() > 0) {
				   u32 result_count = result_list->getItem(0).count;

				   IMoveAction *move = new IMoveAction();
				   move->count = result_count;
				   move->from_inv = s.inventoryloc;
				   move->from_list = "craftresult";
				   move->from_i = 0;
				   move->to_inv = target_slot.inventoryloc;
				   move->to_list = target_slot.listname;
				   move->to_i = target_slot.i;
				   m_invmgr->inventoryAction(move);
			   }
			}

			craft_amount = 0;
#else

			// If there are no items selected or the selected item
			// belongs to craftresult list, proceed with crafting
			if (!m_selected_item ||
					!m_selected_item->isValid() || m_selected_item->listname == "craftresult") {

				assert(inv_s);

				// Send IACTION_CRAFT
				infostream << "Handing IACTION_CRAFT to manager" << std::endl;
				ICraftAction *a = new ICraftAction();
				a->count = craft_amount;
				a->craft_inv = s.inventoryloc;
				m_invmgr->inventoryAction(a);
			}
#endif
		}

		// If m_selected_amount has been decreased to zero,
		// and we are not left-dragging, deselect
		if (m_selected_amount == 0 && !m_left_dragging) {
			m_selected_swap.clear();
			m_selected_item.reset();
			m_selected_amount = 0;
			m_selected_dragging = false;
		}
		m_old_pointer = m_pointer;

		if (absorb_event)
			return true;
	}

	if (event.EventType == EET_GUI_EVENT) {
		const s32 caller_id = event.GUIEvent.Caller->getID();
		bool close_on_enter;

		switch (event.GUIEvent.EventType) {
		case gui::EGET_TAB_CHANGED:
			if (!isVisible())
				break;

#if IS_VOPI_ENGINE && (defined(__ANDROID__) || defined(__IOS__))
			// reset selection
			clearSelection();
#endif

			// find the element that was clicked
			for (GUIFormSpecMenu::FieldSpec &s : m_fields) {
				if (s.ftype == f_TabHeader &&
						s.fid == caller_id) {
					if (!s.sound.empty() && m_sound_manager)
						m_sound_manager->playSound(0, SoundSpec(s.sound, 1.0f));
					s.send = true;
					acceptInput();
					s.send = false;
					return true;
				}
			}
			break;

		case gui::EGET_ELEMENT_FOCUS_LOST:
			if (!isVisible())
				break;

			if (!canTakeFocus(event.GUIEvent.Element)) {
				infostream<<"GUIFormSpecMenu: Not allowing focus change."
						<<std::endl;
				// Returning true disables focus change
				return true;
			}
			break;

		case gui::EGET_BUTTON_CLICKED:
		case gui::EGET_CHECKBOX_CHANGED:
		case gui::EGET_COMBO_BOX_CHANGED:
		case gui::EGET_SCROLL_BAR_CHANGED:
#if IS_VOPI_ENGINE && (defined(__ANDROID__) || defined(__IOS__))
			// reset selection
			clearSelection();
#endif
			if (caller_id == ID_PROCEED_BTN) {
				trySubmitClose();
				return true;
			}

			// find the element that was clicked
			for (GUIFormSpecMenu::FieldSpec &s : m_fields) {
				// if its a button, set the send field so
				// lua knows which button was pressed

				if (caller_id != s.fid)
					continue;

				if (s.ftype == f_Button || s.ftype == f_CheckBox) {
					if (!s.sound.empty() && m_sound_manager)
						m_sound_manager->playSound(0, SoundSpec(s.sound, 1.0f));

					s.send = true;

					if (!s.url.empty()) {
						if (m_client) {
							// in game
							g_gamecallback->showOpenURLDialog(s.url);
						} else {
							// main menu
							porting::open_url(s.url);
						}
					}

					if (s.is_exit) {
						acceptInput(quit_mode_accept);
						quitMenu();
						return true;
					}

					acceptInput(quit_mode_no);
					s.send = false;
					return true;

				} else if (s.ftype == f_DropDown) {
					// only send the changed dropdown
					for (GUIFormSpecMenu::FieldSpec &s2 : m_fields) {
						if (s2.ftype == f_DropDown) {
							s2.send = false;
						}
					}
					if (!s.sound.empty() && m_sound_manager)
						m_sound_manager->playSound(0, SoundSpec(s.sound, 1.0f));
					s.send = true;
					acceptInput(quit_mode_no);

					// revert configuration to make sure dropdowns are sent on
					// regular button click
					for (GUIFormSpecMenu::FieldSpec &s2 : m_fields) {
						if (s2.ftype == f_DropDown) {
							s2.send = true;
						}
					}
					return true;
				} else if (s.ftype == f_ScrollBar) {
					s.fdefault = L"Changed";
					acceptInput(quit_mode_no);
					s.fdefault.clear();
				} else if (s.ftype == f_Unknown || s.ftype == f_HyperText) {
					if (!s.sound.empty() && m_sound_manager)
						m_sound_manager->playSound(0, SoundSpec(s.sound, 1.0f));
					s.send = true;
					acceptInput();
					s.send = false;
				}
			}

			if (event.GUIEvent.EventType == gui::EGET_SCROLL_BAR_CHANGED) {
				// move scroll_containers
				for (const std::pair<std::string, GUIScrollContainer *> &c : m_scroll_containers)
					c.second->onScrollEvent(event.GUIEvent.Caller);
			}
			break;

		case gui::EGET_EDITBOX_ENTER:
			if (caller_id <= ID_PROCEED_BTN)
				break;

			close_on_enter = true;
			for (GUIFormSpecMenu::FieldSpec &s : m_fields) {
				if (s.ftype == f_Unknown &&
						s.fid == caller_id) {
					current_field_enter_pending = s.fname;
					auto it = field_close_on_enter.find(s.fname);
					if (it != field_close_on_enter.end())
						close_on_enter = (*it).second;

					break;
				}
			}

			current_keys_pending.key_enter = true;

			if (close_on_enter)
				trySubmitClose();
			else
				acceptInput();
			return true;

#if IS_VOPI_ENGINE && (defined(__ANDROID__) || defined(__IOS__))
		case gui::EGET_TABLE_CHANGED:
		case gui::EGET_EDITBOX_CHANGED:
			// reset selection
			clearSelection();
#else
		case gui::EGET_TABLE_CHANGED:
#endif
			if (caller_id <= ID_PROCEED_BTN)
				break;

			// find the element that was clicked
			for (GUIFormSpecMenu::FieldSpec &s : m_fields) {
				// if it's a table, set the send field
				// so lua knows which table was changed
				if (s.ftype == f_Table && s.fid == caller_id) {
					s.send = true;
					acceptInput();
					s.send = false;
				}
			}
			return true;

		default:
			break;
		}
	}

	if (m_second_touch)
		return true; // Stop propagating the event

	return Parent ? Parent->OnEvent(event) : false;
}

/**
 * get name of element by element id
 * @param id of element
 * @return name string or empty string
 */
std::string GUIFormSpecMenu::getNameByID(s32 id)
{
	for (FieldSpec &spec : m_fields) {
		if (spec.fid == id)
			return spec.fname;
	}
	return "";
}


const GUIFormSpecMenu::FieldSpec *GUIFormSpecMenu::getSpecByID(s32 id)
{
	for (FieldSpec &spec : m_fields) {
		if (spec.fid == id)
			return &spec;
	}
	return nullptr;
}

/**
 * get label of element by id
 * @param id of element
 * @return label string or empty string
 */
std::wstring GUIFormSpecMenu::getLabelByID(s32 id)
{
	for (FieldSpec &spec : m_fields) {
		if (spec.fid == id)
			return spec.flabel;
	}
	return L"";
}

StyleSpec GUIFormSpecMenu::getDefaultStyleForElement(const std::string &type,
		const std::string &name, const std::string &parent_type) {
	return getStyleForElement(type, name, parent_type)[StyleSpec::STATE_DEFAULT];
}

std::array<StyleSpec, StyleSpec::NUM_STATES> GUIFormSpecMenu::getStyleForElement(
	const std::string &type, const std::string &name, const std::string &parent_type)
{
	std::array<StyleSpec, StyleSpec::NUM_STATES> ret;

	auto it = theme_by_type.find("*");
	if (it != theme_by_type.end()) {
		for (const StyleSpec &spec : it->second)
			ret[(u32)spec.getState()] |= spec;
	}

	it = theme_by_name.find("*");
	if (it != theme_by_name.end()) {
		for (const StyleSpec &spec : it->second)
			ret[(u32)spec.getState()] |= spec;
	}

	if (!parent_type.empty()) {
		it = theme_by_type.find(parent_type);
		if (it != theme_by_type.end()) {
			for (const StyleSpec &spec : it->second)
				ret[(u32)spec.getState()] |= spec;
		}
	}

	it = theme_by_type.find(type);
	if (it != theme_by_type.end()) {
		for (const StyleSpec &spec : it->second)
			ret[(u32)spec.getState()] |= spec;
	}

	it = theme_by_name.find(name);
	if (it != theme_by_name.end()) {
		for (const StyleSpec &spec : it->second)
			ret[(u32)spec.getState()] |= spec;
	}

	return ret;
}

double GUIFormSpecMenu::getFixedImgsize(double screen_dpi, double gui_scaling)
{
	// In fixed-size mode, inventory image size
	// is 0.53 inch multiplied by the gui_scaling
	// config parameter.  This magic size is chosen
	// to make the main menu (15.5 inventory images
	// wide, including border) just fit into the
	// default window (800 pixels wide) at 96 DPI
	// and default scaling (1.00).
	return 0.5555 * screen_dpi * gui_scaling;
}

double GUIFormSpecMenu::getImgsize(v2u32 avail_screensize, double screen_dpi, double gui_scaling)
{
	double fixed_imgsize = getFixedImgsize(screen_dpi, gui_scaling);

	s32 min_screen_dim = std::min(avail_screensize.X, avail_screensize.Y);
	double prefer_imgsize = min_screen_dim / 15 * gui_scaling;
	// Use the available space more effectively on small windows/screens.
	// This is especially important for mobile platforms.
	prefer_imgsize = std::max(prefer_imgsize, fixed_imgsize);
	return prefer_imgsize;
}

double GUIFormSpecMenu::calculateImgsize(const parserData &data)
{
	// must stay in sync with ClientDynamicInfo::calculateMaxFSSize

	const double screen_dpi = RenderingEngine::getDisplayDensity() * 96;
	const double gui_scaling = g_settings->getFloat("gui_scaling", 0.5f, 42.0f);

	// Fixed-size mode
	if (m_lock)
		return getFixedImgsize(screen_dpi, gui_scaling);

	// Variables for the maximum imgsize that can fit in the screen.
	double fitx_imgsize;
	double fity_imgsize;

	v2f padded_screensize(
		data.screensize.X * (1.0f - data.padding.X * 2.0f),
		data.screensize.Y * (1.0f - data.padding.Y * 2.0f)
	);

	if (data.real_coordinates) {
		fitx_imgsize = padded_screensize.X / data.invsize.X;
		fity_imgsize = padded_screensize.Y / data.invsize.Y;
	} else {
		// The maximum imgsize in the old coordinate system also needs to
		// factor in padding and spacing along with 0.1 inventory slot spare
		// and help text space, hence the magic numbers.
		fitx_imgsize = padded_screensize.X /
				((5.0 / 4.0) * (0.5 + data.invsize.X));
		fity_imgsize = padded_screensize.Y /
				((15.0 / 13.0) * (0.85 + data.invsize.Y));
	}

	double prefer_imgsize = getImgsize(v2u32::from(padded_screensize),
			screen_dpi, gui_scaling);

#if IS_VOPI_ENGINE && !defined(__ANDROID__) && !defined(__IOS__)
	// Desktop VOPI: scale prefer_imgsize with window so fullscreen formspecs
	// (10.24x5.12) fill the window, while popups (4.8x2.4) stay proportional.
	// Reference height 6.0 units: formspecs taller than this fill the window,
	// shorter ones are proportionally smaller (popup behavior).
	double window_scaled_prefer = padded_screensize.Y / 6.0;
	double desktop_prefer = std::max(window_scaled_prefer, prefer_imgsize);
	return std::min(desktop_prefer, std::min(fitx_imgsize, fity_imgsize));
#else
	// Try to use the preferred imgsize, but if that's bigger than the maximum
	// size, use the maximum size.
	return std::min(prefer_imgsize, std::min(fitx_imgsize, fity_imgsize));
#endif
}
