#include "guiAnimatedImage.h"

#include "client/guiscalingfilter.h"
#include "porting.h"
#if IS_VOPI_ENGINE
#include "settings.h"
#include <IVideoDriver.h>
#endif
#include <ITexture.h>

GUIAnimatedImage::GUIAnimatedImage(gui::IGUIEnvironment *env, gui::IGUIElement *parent,
	s32 id, const core::rect<s32> &rectangle) :
	gui::IGUIElement(gui::EGUIET_ELEMENT, env, parent, id, rectangle)
{
}

void GUIAnimatedImage::draw()
{
	if (m_texture == nullptr)
		return;

	video::IVideoDriver *driver = Environment->getVideoDriver();

	core::dimension2d<u32> size = m_texture->getOriginalSize();

#if IS_VOPI_ENGINE
	if (m_frame_idx >= m_frame_count)
		m_frame_idx = m_frame_count - 1;

	// VOPI: Frames may be packed as a 2D grid (m_columns > 1), filled
	// left-to-right then top-to-bottom. m_columns == 1 is the classic
	// single-column vertical strip (upstream behaviour). A grid keeps long
	// animations within the GPU's max texture size: the atlas grows
	// ~sqrt(frames) instead of linearly in height. An over-tall strip is
	// downscaled by the driver, which knocks the per-frame offsets off their
	// pixel rows and makes the animation jitter.
	s32 columns = std::max(m_columns, 1);
	// 64-bit ceil: m_frame_count and m_columns are each clamped >= 1 but
	// unbounded above (both are server-controlled), so the numerator
	// (m_frame_count + columns - 1) would overflow s32. The quotient fits in
	// s32 (rows <= m_frame_count).
	s32 rows = (s32)(((s64)m_frame_count + columns - 1) / columns); // ceil

	// Defensive clamps: keep each source cell >= 1px and never sample past the
	// atlas. This replaces the upstream guard `m_frame_count > size.Height`
	// (which assumed a single column); clamping rows/columns keeps frame_w and
	// frame_h >= 1, and the idx clamp below keeps the sampled cell in bounds
	// even if frame_count exceeds what the atlas can physically hold.
	if (columns > (s32)size.Width)
		columns = std::max<s32>((s32)size.Width, 1);
	if (rows > (s32)size.Height)
		rows = std::max<s32>((s32)size.Height, 1);

	const u32 frame_w = size.Width / columns;
	const u32 frame_h = size.Height / rows;

	const s32 idx = std::min(m_frame_idx, columns * rows - 1);
	const s32 col = idx % columns;
	const s32 row = idx / columns;

	core::rect<s32> rect(
		core::position2d<s32>((s32)(col * frame_w), (s32)(row * frame_h)),
		core::dimension2d<u32>(frame_w, frame_h));
#else
	if ((u32)m_frame_count > size.Height)
		m_frame_count = size.Height;
	if (m_frame_idx >= m_frame_count)
		m_frame_idx = m_frame_count - 1;

	size.Height /= m_frame_count;

	core::rect<s32> rect(core::position2d<s32>(0, size.Height * m_frame_idx), size);
#endif
	core::rect<s32> *cliprect = NoClip ? nullptr : &AbsoluteClippingRect;

	if (m_middle.getArea() == 0) {
		const video::SColor color(255, 255, 255, 255);
		const video::SColor colors[] = {color, color, color, color};
#if IS_VOPI_ENGINE
		// The GUI scaling filter samples the frame linearly from the atlas on
		// the GPU. draw2DImageFilterScaled() would scale each frame in
		// software into a texture of its own and keep it, with a copy of the
		// atlas, until guiScalingCacheClear(): every frame at every size drawn.
		// An inverted or empty rect draws nothing, with the filter or without,
		// as draw2DImageFilterScaled() skips it.
		if (AbsoluteRect.getWidth() > 0 && AbsoluteRect.getHeight() > 0) {
			if (g_settings->getBool("gui_scaling_filter"))
				draw2DImageLinear(driver, m_texture, AbsoluteRect, rect, cliprect,
					colors, true);
			else
				driver->draw2DImage(m_texture, AbsoluteRect, rect, cliprect,
					colors, true);
		}
#else
		draw2DImageFilterScaled(driver, m_texture, AbsoluteRect, rect, cliprect,
			colors, true);
#endif
	} else {
		draw2DImage9Slice(driver, m_texture, AbsoluteRect, rect, m_middle, cliprect);
	}

	// Step the animation
	if (m_frame_count > 1 && m_frame_duration > 0) {
		// Determine the delta time to step
		u64 new_global_time = porting::getTimeMs();
		if (m_global_time > 0)
			m_frame_time += new_global_time - m_global_time;

		m_global_time = new_global_time;

		// Advance by the number of elapsed frames
		m_frame_idx += (u32)(m_frame_time / m_frame_duration);
		if (m_loop) {
			// Loop back to the start
			m_frame_idx %= m_frame_count;
		} else if (m_frame_idx >= m_frame_count) {
			// One-shot: hold on the last frame instead of wrapping
			m_frame_idx = m_frame_count - 1;
		}

		// If 1 or more frames have elapsed, reset the frame time counter with
		// the remainder
		m_frame_time %= m_frame_duration;
	}
}
