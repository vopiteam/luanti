// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 VOPI Team

#include "tweenSpec.h"

#include "util/string.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>

namespace {

bool parseUint(std::string_view s, u64 max, u64 &out)
{
	s = trim(s);
	if (s.empty())
		return false;
	u64 value = 0;
	for (char c : s) {
		if (c < '0' || c > '9')
			return false;
		const u64 digit = c - '0';
		if (value > (max - digit) / 10)
			return false;
		value = value * 10 + digit;
	}
	out = value;
	return true;
}

bool parseFloat(std::string_view s, f32 &out)
{
	const std::string str(trim(s));
	if (str.empty())
		return false;
	char *end = nullptr;
	const double value = std::strtod(str.c_str(), &end);
	if (end != str.c_str() + str.size() || !std::isfinite(value))
		return false;
	out = (f32)value;
	return std::isfinite(out);
}

bool parseBool(std::string_view s, bool &out)
{
	const std::string value = lowercase(trim(s));
	if (value == "true" || value == "yes" || value == "1") {
		out = true;
		return true;
	}
	if (value == "false" || value == "no" || value == "0") {
		out = false;
		return true;
	}
	return false;
}

// "x,y", or "v" for (v, v) when `allow_single`
bool parsePair(std::string_view s, bool allow_single, v2f &out)
{
	const size_t comma = s.find(',');
	if (comma == std::string_view::npos) {
		f32 v;
		if (!allow_single || !parseFloat(s, v))
			return false;
		out = v2f(v, v);
		return true;
	}
	f32 x, y;
	if (!parseFloat(s.substr(0, comma), x) || !parseFloat(s.substr(comma + 1), y))
		return false;
	out = v2f(x, y);
	return true;
}

bool parseEasing(std::string_view s, TweenEasing &out)
{
	s = trim(s);
	if (s == "linear")
		out = TweenEasing::Linear;
	else if (s == "in")
		out = TweenEasing::In;
	else if (s == "out")
		out = TweenEasing::Out;
	else if (s == "in_out")
		out = TweenEasing::InOut;
	else if (s == "out_back")
		out = TweenEasing::OutBack;
	else
		return false;
	return true;
}

v2f clampPair(v2f v, f32 min, f32 max)
{
	return v2f(std::clamp(v.X, min, max), std::clamp(v.Y, min, max));
}

f32 ease(TweenEasing easing, f32 u)
{
	switch (easing) {
	case TweenEasing::In:
		return u * u * u;
	case TweenEasing::Out: {
		const f32 v = 1.0f - u;
		return 1.0f - v * v * v;
	}
	case TweenEasing::InOut: {
		if (u < 0.5f)
			return 4.0f * u * u * u;
		const f32 v = 2.0f - 2.0f * u;
		return 1.0f - v * v * v / 2.0f;
	}
	case TweenEasing::OutBack: {
		const f32 c1 = 1.70158f;
		const f32 c3 = c1 + 1.0f;
		const f32 v = u - 1.0f;
		return 1.0f + c3 * v * v * v + c1 * v * v;
	}
	case TweenEasing::Linear:
		break;
	}
	return u;
}

void applyKey(const TweenKey &key, TweenState &state)
{
	state.offset = key.offset;
	state.scale = key.scale;
	state.alpha = key.alpha;
}

} // namespace

bool TweenSpec::parseKey(std::string_view value, std::string &error)
{
	if (keys.size() >= MAX_KEYS) {
		error = "more than " + std::to_string(MAX_KEYS) + " keys";
		return false;
	}

	std::vector<std::string_view> fields;
	size_t start = 0;
	while (true) {
		const size_t colon = value.find(':', start);
		fields.push_back(value.substr(start, colon - start));
		if (colon == std::string_view::npos)
			break;
		start = colon + 1;
	}
	if (fields.size() > 6) {
		error = "a key has at most 6 fields";
		return false;
	}

	// Empty fields keep the previous key's value
	TweenKey key = keys.empty() ? TweenKey() : keys.back();
	key.easing = TweenEasing::Linear;
	key.curved = false;
	key.control = v2f();

	u64 time_ms;
	if (!parseUint(fields[0], std::numeric_limits<u32>::max(), time_ms)) {
		error = "invalid key time";
		return false;
	}
	if (!keys.empty() && time_ms < keys.back().time_ms) {
		error = "key times must not decrease";
		return false;
	}
	key.time_ms = (u32)time_ms;

	auto present = [&](size_t i) {
		return fields.size() > i && !trim(fields[i]).empty();
	};

	if (present(1)) {
		v2f offset;
		if (!parsePair(fields[1], false, offset)) {
			error = "invalid key offset";
			return false;
		}
		key.offset = clampPair(offset, -MAX_OFFSET, MAX_OFFSET);
	}
	if (present(2)) {
		v2f scale;
		if (!parsePair(fields[2], true, scale)) {
			error = "invalid key scale";
			return false;
		}
		key.scale = clampPair(scale, 0.0f, MAX_SCALE);
	}
	if (present(3)) {
		f32 alpha;
		if (!parseFloat(fields[3], alpha)) {
			error = "invalid key alpha";
			return false;
		}
		key.alpha = std::clamp(alpha, 0.0f, 1.0f);
	}
	if (present(4) && !parseEasing(fields[4], key.easing)) {
		error = "unknown key easing";
		return false;
	}
	if (present(5)) {
		v2f control;
		if (!parsePair(fields[5], false, control)) {
			error = "invalid key control point";
			return false;
		}
		key.control = clampPair(control, -MAX_OFFSET, MAX_OFFSET);
		key.curved = true;
	}

	keys.push_back(key);
	return true;
}

TweenSpec::OptionResult TweenSpec::setOption(std::string_view name,
		std::string_view value, std::string &error)
{
	name = trim(name);
	u64 number;
	bool flag;

	if (name == "frames" || name == "columns") {
		if (!parseUint(value, MAX_FRAMES, number) || number < 1) {
			error = std::string(name) + " must be from 1 to " + std::to_string(MAX_FRAMES);
			return OptionResult::Invalid;
		}
		(name == "frames" ? frames : columns) = (u32)number;
	} else if (name == "frame_ms") {
		if (!parseUint(value, std::numeric_limits<u32>::max(), number)) {
			error = "invalid frame_ms";
			return OptionResult::Invalid;
		}
		frame_ms = (u32)number;
	} else if (name == "frame_loop" || name == "loop") {
		if (!parseBool(value, flag)) {
			error = "invalid " + std::string(name);
			return OptionResult::Invalid;
		}
		(name == "loop" ? loop : frame_loop) = flag;
	} else if (name == "key") {
		if (!parseKey(value, error))
			return OptionResult::Invalid;
	} else {
		return OptionResult::Unknown;
	}
	return OptionResult::Ok;
}

TweenState TweenSpec::evaluate(u64 elapsed_ms) const
{
	TweenState state;

	if (frames > 1 && frame_ms > 0) {
		const u64 frame = elapsed_ms / frame_ms;
		state.frame = frame_loop ? (u32)(frame % frames)
				: (u32)std::min<u64>(frame, frames - 1);
	}

	if (keys.empty())
		return state;

	u64 t = elapsed_ms;
	const u32 period = keys.back().time_ms;
	if (loop && period > 0)
		t %= period;

	// Before the first key and after the last one, their values hold
	if (t <= keys.front().time_ms) {
		applyKey(keys.front(), state);
		return state;
	}
	if (t >= keys.back().time_ms) {
		applyKey(keys.back(), state);
		return state;
	}

	size_t i = 1;
	while (t >= keys[i].time_ms)
		i++;
	const TweenKey &from = keys[i - 1];
	const TweenKey &to = keys[i];

	// from.time_ms <= t < to.time_ms, so the segment is not empty
	const f32 u = (f32)(t - from.time_ms) / (f32)(to.time_ms - from.time_ms);
	const f32 e = ease(to.easing, u);

	if (to.curved) {
		const f32 v = 1.0f - e;
		state.offset = from.offset * (v * v) + to.control * (2.0f * v * e) +
				to.offset * (e * e);
	} else {
		state.offset = from.offset + (to.offset - from.offset) * e;
	}
	const v2f scale = from.scale + (to.scale - from.scale) * e;
	state.scale = v2f(std::max(scale.X, 0.0f), std::max(scale.Y, 0.0f));
	state.alpha = std::clamp(from.alpha + (to.alpha - from.alpha) * e, 0.0f, 1.0f);
	return state;
}
