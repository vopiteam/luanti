// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2022 DS
// Copyright (C) 2013 celeron55, Perttu Ahola <celeron55@gmail.com>
// Copyright (C) 2011 Sebastian 'Bahamada' Rühl
// Copyright (C) 2011 Cyriaque 'Cisoun' Skrapits <cysoun@gmail.com>
// Copyright (C) 2011 Giuseppe Bilotta <giuseppe.bilotta@gmail.com>

#pragma once

#include <atomic>
#include <memory>
#include "al_helpers.h"

namespace sound {

/**
 * Class for the openal device and context
 */
class SoundManagerSingleton
{
public:
	struct AlcDeviceDeleter {
		void operator()(ALCdevice *p)
		{
			alcCloseDevice(p);
		}
	};

	struct AlcContextDeleter {
		void operator()(ALCcontext *p)
		{
			alcMakeContextCurrent(nullptr);
			alcDestroyContext(p);
		}
	};

	using unique_ptr_alcdevice = std::unique_ptr<ALCdevice, AlcDeviceDeleter>;
	using unique_ptr_alccontext = std::unique_ptr<ALCcontext, AlcContextDeleter>;

	unique_ptr_alcdevice  m_device;
	unique_ptr_alccontext m_context;

public:
	bool init();

	~SoundManagerSingleton();

#if IS_VOPI_ENGINE && (defined(__ANDROID__) || defined(__IOS__))
	/**
	 * Stops or restarts the output device (ALC_SOFT_pause_device), for the
	 * app in the background on Android and for an interruption of the
	 * audio session on iOS, whose output does not restart by itself. Main
	 * thread; the same state again is a no-op.
	 */
	void setOutputPaused(bool paused);

	/** Whether the output is paused. Any thread */
	bool isOutputPaused() const { return m_output_paused.load(std::memory_order_relaxed); }

private:
	std::atomic<bool> m_output_paused{false};
	// ALC_SOFT_pause_device's and ALC_SOFT_reopen_device's functions, null
	// without the extension
	LPALCDEVICEPAUSESOFT m_device_pause = nullptr;
	LPALCDEVICERESUMESOFT m_device_resume = nullptr;
	LPALCREOPENDEVICESOFT m_device_reopen = nullptr;
#endif
};

} // namespace sound
