// Copyright 2008 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.
#ifndef MOBILE_DEVICE

#pragma once

#include "Common/CommonTypes.h"
#include "Common/File/Path.h"

class AVIDump {
private:
	static bool CreateAVI();
	static void CloseFile();
	static void CheckResolution(int width, int height);

public:
	static bool Start(int w, int h);
	static void AddFrame();
	// Interleaved stereo s16 at 44100 Hz. Ignored unless an MP4 recording with audio is running.
	static void AddAudioSamples(const s16 *samples, int numFrames);
	// True while recording an MP4 that has an audio track, so the separate WAV dump isn't needed.
	static bool IsRecordingAudio();
	static void Stop();
	static Path LastFilename();
};
#endif
