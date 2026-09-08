// SPDX-License-Identifier: MIT
// Copyright (C) 2026-present PortareOS (https://github.com/portare-ch)

#pragma once
#ifndef ES_APP_VOLUME_CONTROL_H
#define ES_APP_VOLUME_CONTROL_H

#include <memory>

/*!
Singleton pattern. Call getInstance() to get an object.

Volume goes to pipewire and nowhere else: no alsa mixer, no pulse, and no
other platform. See VolumeControl.cpp for how the sink is found.
*/
class VolumeControl
{
	int internalVolume;

	static std::weak_ptr<VolumeControl> sInstance;

	VolumeControl();
	VolumeControl(const VolumeControl & right);
	VolumeControl & operator=(const VolumeControl & right);

public:
	static std::shared_ptr<VolumeControl> & getInstance();

	void init();
	void deinit();

	bool isAvailable();

	int getVolume() const;
	void setVolume(int volume);

	~VolumeControl();
};

#endif // ES_APP_VOLUME_CONTROL_H
