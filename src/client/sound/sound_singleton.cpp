// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2022 DS
// Copyright (C) 2013 celeron55, Perttu Ahola <celeron55@gmail.com>
// Copyright (C) 2011 Sebastian 'Bahamada' Rühl
// Copyright (C) 2011 Cyriaque 'Cisoun' Skrapits <cysoun@gmail.com>
// Copyright (C) 2011 Giuseppe Bilotta <giuseppe.bilotta@gmail.com>

#include "sound_singleton.h"

namespace sound {

bool SoundManagerSingleton::init()
{
	if (!(m_device = unique_ptr_alcdevice(alcOpenDevice(nullptr)))) {
		errorstream << "Audio: Global Initialization: Failed to open device" << std::endl;
		return false;
	}

	if (!(m_context = unique_ptr_alccontext(alcCreateContext(m_device.get(), nullptr)))) {
		errorstream << "Audio: Global Initialization: Failed to create context" << std::endl;
		return false;
	}

	if (!alcMakeContextCurrent(m_context.get())) {
		errorstream << "Audio: Global Initialization: Failed to make current context" << std::endl;
		return false;
	}

	alDistanceModel(AL_INVERSE_DISTANCE_CLAMPED);

	// Speed of sound in nodes per second
	// FIXME: This value assumes 1 node sidelength = 1 meter, and "normal" air.
	//        Ideally this should be mod-controlled.
	alSpeedOfSound(343.3f);

	// doppler effect turned off for now, for best backwards compatibility
	alDopplerFactor(0.0f);

	if (alGetError() != AL_NO_ERROR) {
		errorstream << "Audio: Global Initialization: OpenAL Error " << alGetError() << std::endl;
		return false;
	}

#if IS_VOPI_ENGINE && defined(__ANDROID__)
	if (alcIsExtensionPresent(m_device.get(), "ALC_SOFT_pause_device")) {
		m_device_pause = reinterpret_cast<LPALCDEVICEPAUSESOFT>(
				alcGetProcAddress(m_device.get(), "alcDevicePauseSOFT"));
		m_device_resume = reinterpret_cast<LPALCDEVICERESUMESOFT>(
				alcGetProcAddress(m_device.get(), "alcDeviceResumeSOFT"));
	}
	if (alcIsExtensionPresent(m_device.get(), "ALC_SOFT_reopen_device")) {
		m_device_reopen = reinterpret_cast<LPALCREOPENDEVICESOFT>(
				alcGetProcAddress(m_device.get(), "alcReopenDeviceSOFT"));
	}
#endif

	infostream << "Audio: Global Initialized: OpenAL " << alGetString(AL_VERSION)
		<< ", using " << alcGetString(m_device.get(), ALC_DEVICE_SPECIFIER)
		<< std::endl;

	return true;
}

SoundManagerSingleton::~SoundManagerSingleton()
{
	infostream << "Audio: Global Deinitialized." << std::endl;
}

#if IS_VOPI_ENGINE && defined(__ANDROID__)
void SoundManagerSingleton::setOutputPaused(bool paused)
{
	if (paused == isOutputPaused() || !m_device)
		return;
	if (!m_device_pause || !m_device_resume) {
		static bool warned = false;
		if (!warned)
			warningstream << "Audio: no ALC_SOFT_pause_device, the output keeps running" << std::endl;
		warned = true;
		return;
	}

	// Stopping the device stops the backend's stream and its mixer thread;
	// the sources keep their state and play on when it restarts
	ALCdevice *device = m_device.get();
	m_output_paused.store(paused, std::memory_order_relaxed);
	if (paused) {
		m_device_pause(device);
	} else {
		m_device_resume(device);
		// A restart that failed (the audio route changed meanwhile, ...)
		// leaves the device disconnected and the game silent: reopened on
		// the default output, with the context and the sources kept
		ALCint connected = 1;
		if (alcIsExtensionPresent(device, "ALC_EXT_disconnect"))
			alcGetIntegerv(device, ALC_CONNECTED, 1, &connected);
		if (!connected) {
			const bool reopened = m_device_reopen && m_device_reopen(device, nullptr, nullptr);
			warningstream << "Audio: the output did not resume, the device is disconnected"
					<< (reopened ? "; reopened" : "; reopening failed") << std::endl;
		}
	}
	infostream << "Audio: output " << (paused ? "paused" : "resumed") << std::endl;
}
#endif

} // namespace sound
