// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 VOPI Team

#pragma once

#include "irrlichttypes.h"
#include "irr_v2d.h"
#include <string>
#include <string_view>
#include <vector>

// Timing curve of the keyframe segment that ends at a key.
enum class TweenEasing : u8
{
	Linear,
	In,      // cubic, slow start
	Out,     // cubic, slow end
	InOut,   // cubic, slow start and end
	OutBack, // overshoots the end slightly, then settles
};

struct TweenKey
{
	u32 time_ms = 0;
	// Offset of the element's centre from its rest position, in element units
	v2f offset;
	// Scale around the element's centre
	v2f scale{1.0f, 1.0f};
	f32 alpha = 1.0f;
	TweenEasing easing = TweenEasing::Linear;
	// Quadratic Bezier control point of the segment, in element units
	bool curved = false;
	v2f control;
};

struct TweenState
{
	v2f offset;
	v2f scale{1.0f, 1.0f};
	f32 alpha = 1.0f;
	// Index of the sprite frame to show
	u32 frame = 0;
};

// The animation of a tween: a keyframe timeline that moves, scales and fades
// an element, and an optional sprite animation from a frame atlas. Shared by
// every element that animates this way, so it knows nothing about drawing.
class TweenSpec
{
public:
	static constexpr size_t MAX_KEYS = 32;
	static constexpr u32 MAX_FRAMES = 1024;
	static constexpr f32 MAX_OFFSET = 100000.0f;
	static constexpr f32 MAX_SCALE = 1000.0f;

	enum class OptionResult
	{
		Ok,
		Unknown, // not an option of the animation; the caller may know it
		Invalid, // ignored, `error` says why
	};

	// Applies one `name=value` option: frames, columns, frame_ms,
	// frame_loop, key or loop.
	OptionResult setOption(std::string_view name, std::string_view value,
			std::string &error);

	// State at `elapsed_ms` after the animation started.
	TweenState evaluate(u64 elapsed_ms) const;

	// Sprite atlas: `frames` cells packed left to right, then top to bottom,
	// in `columns` columns.
	u32 frames = 1;
	u32 columns = 1;
	// Duration of one sprite frame; 0 shows the first frame only.
	u32 frame_ms = 0;
	// When false, the sprite animation holds on its last frame.
	bool frame_loop = true;
	// When true, the timeline restarts after its last key.
	bool loop = false;
	// Ascending by time_ms.
	std::vector<TweenKey> keys;

private:
	bool parseKey(std::string_view value, std::string &error);
};
