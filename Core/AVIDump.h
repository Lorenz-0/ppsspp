// Copyright 2008 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.
#ifndef MOBILE_DEVICE

#pragma once

#include "Common/CommonTypes.h"
#include "Common/File/Path.h"

class AVIDump {
private:
	static bool CreateMP4();
	static void CloseFile();
	static void CheckResolution(int width, int height);

public:
	static bool Start(int w, int h);
	static void AddFrame();
	static void AddAudio(const s16 *samples, int numSamples, u64 timeUs);
	static void Stop();
	static Path LastFilename();
};
#endif
