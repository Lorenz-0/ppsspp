// Copyright 2009 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#ifndef MOBILE_DEVICE

#if defined(__FreeBSD__)
#define __STDC_CONSTANT_MACROS 1
#endif

#include <string>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <atomic>
#include <mutex>
#include <vector>

#ifdef USE_FFMPEG

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mathematics.h>
#include <libavutil/samplefmt.h>
#include <libswscale/swscale.h>
}

#endif

#include "Common/Data/Convert/ColorConv.h"
#include "Common/File/FileUtil.h"
#include "Common/File/Path.h"

#include "Core/Config.h"
#include "Core/AVIDump.h"
#include "Core/System.h"
#include "Core/Screenshot.h"

#include "GPU/GPUCommon.h"

#include "Core/ELF/ParamSFO.h"
#include "Core/HLE/sceKernelTime.h"
#include "StringUtils.h"

#ifdef USE_FFMPEG

#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(55, 28, 1)
#define av_frame_alloc avcodec_alloc_frame
#define av_frame_free avcodec_free_frame
#endif

#include "FFMPEGCompat.h"

static AVFormatContext *s_format_context = nullptr;
static AVCodecContext *s_codec_context = nullptr;
static AVStream *s_stream = nullptr;
static AVFrame *s_src_frame = nullptr;
static AVFrame *s_scaled_frame = nullptr;
static SwsContext *s_sws_context = nullptr;

// MP4 only: AAC audio muxed into the same file.
static AVStream *s_audio_stream = nullptr;
static AVCodecContext *s_audio_context = nullptr;
static AVFrame *s_audio_frame = nullptr;
static std::vector<float> s_audio_fifo[2];
static s64 s_video_pts = 0;
static s64 s_audio_pts = 0;

#endif

// Video frames come from the flip, audio blocks from the mixer, and both write to the same muxer.
// Recursive because a resolution change calls Stop and Start from inside AddFrame.
static std::recursive_mutex s_dumpMutex;
static std::atomic<bool> s_audioActive;
static bool s_is_mp4 = false;

static int s_bytes_per_pixel;
static int s_width;
static int s_height;
static bool s_start_dumping = false;
static int s_current_width;
static int s_current_height;
static int s_file_index = 0;
static GPUDebugBuffer buf;
static Path g_filename;

static void InitAVCodec() {
	static bool first_run = true;
	if (first_run) {
#ifdef USE_FFMPEG
#if LIBAVFORMAT_VERSION_INT < AV_VERSION_INT(58, 12, 100)
		av_register_all();
#endif
#endif
		first_run = false;
	}
}

bool AVIDump::Start(int w, int h)
{
	std::lock_guard<std::recursive_mutex> guard(s_dumpMutex);
	s_width = w;
	s_height = h;
	s_current_width = w;
	s_current_height = h;

	InitAVCodec();
	bool success = CreateAVI();
	if (!success)
		CloseFile();
	return success;
}

#ifdef USE_FFMPEG

static bool CreateAudioStream() {
	AVCodec *audioCodec = avcodec_find_encoder(AV_CODEC_ID_AAC);
	s_audio_stream = avformat_new_stream(s_format_context, audioCodec);
	if (!s_audio_stream)
		return false;

#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(57, 48, 101)
	s_audio_context = s_audio_stream->codec;
#else
	s_audio_context = avcodec_alloc_context3(audioCodec);
#endif
	s_audio_context->codec_id = AV_CODEC_ID_AAC;
	s_audio_context->codec_type = AVMEDIA_TYPE_AUDIO;
	s_audio_context->sample_fmt = AV_SAMPLE_FMT_FLTP;
	s_audio_context->sample_rate = 44100;
	s_audio_context->bit_rate = 192000;
	s_audio_context->time_base.num = 1;
	s_audio_context->time_base.den = 44100;
	s_audio_context->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(59, 24, 100)
	av_channel_layout_default(&s_audio_context->ch_layout, 2);
#else
	s_audio_context->channel_layout = AV_CH_LAYOUT_STEREO;
	s_audio_context->channels = 2;
#endif
	if (s_format_context->oformat->flags & AVFMT_GLOBALHEADER)
		s_audio_context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

	if (avcodec_open2(s_audio_context, audioCodec, nullptr) < 0)
		return false;
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(57, 48, 101)
	if (avcodec_parameters_from_context(s_audio_stream->codecpar, s_audio_context) < 0)
		return false;
#endif

	s_audio_frame = av_frame_alloc();
	s_audio_frame->format = AV_SAMPLE_FMT_FLTP;
	s_audio_frame->nb_samples = s_audio_context->frame_size;
	s_audio_frame->sample_rate = s_audio_context->sample_rate;
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(59, 24, 100)
	av_channel_layout_copy(&s_audio_frame->ch_layout, &s_audio_context->ch_layout);
#else
	s_audio_frame->channel_layout = AV_CH_LAYOUT_STEREO;
#endif
	if (av_frame_get_buffer(s_audio_frame, 0) < 0)
		return false;

	s_audio_pts = 0;
	s_audio_fifo[0].clear();
	s_audio_fifo[1].clear();
	return true;
}

#endif

bool AVIDump::CreateAVI() {
#ifdef USE_FFMPEG
	AVCodec *codec = nullptr;

	s_is_mp4 = g_Config.bDumpMP4;
	if (s_is_mp4 && !av_guess_format("mp4", nullptr, nullptr)) {
		WARN_LOG(Log::Common, "This FFmpeg build has no MP4 muxer, recording AVI instead");
		s_is_mp4 = false;
	}
	const bool useFFV1 = g_Config.bUseFFV1 && !s_is_mp4;
	s_video_pts = 0;

	// Use gameID_EmulatedTimestamp for filename
	std::string discID = g_paramSFO.GetDiscID();
	g_filename = GetSysDirectory(DIRECTORY_VIDEO) / StringFromFormat("%s_%s.%s", discID.c_str(), KernelTimeNowFormatted().c_str(), s_is_mp4 ? "mp4" : "avi");

	s_format_context = avformat_alloc_context();

#if LIBAVFORMAT_VERSION_INT >= AV_VERSION_INT(58, 7, 0)
	char *filename = av_strdup(g_filename.c_str());
	// Freed when the context is freed.
	s_format_context->url = filename;
#else
	const char *filename = s_format_context->filename;
	snprintf(s_format_context->filename, sizeof(s_format_context->filename), "%s", g_filename.c_str());
#endif
	INFO_LOG(Log::Common, "Recording Video to: %s", g_filename.ToVisualString().c_str());

	// Make sure that the path exists
	if (!File::Exists(GetSysDirectory(DIRECTORY_VIDEO)))
		File::CreateDir(GetSysDirectory(DIRECTORY_VIDEO));

	if (File::Exists(g_filename))
		File::Delete(g_filename);

	s_format_context->oformat = av_guess_format(s_is_mp4 ? "mp4" : "avi", nullptr, nullptr);
	if (!s_format_context->oformat)
		return false;
	s_stream = avformat_new_stream(s_format_context, codec);
	if (!s_stream)
		return false;

#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(57, 48, 101)
	s_codec_context = s_stream->codec;
#else
	s_codec_context = avcodec_alloc_context3(codec);
#endif
	// The mp4 muxer's default video codec is H.264, which this FFmpeg build doesn't have an encoder for.
	s_codec_context->codec_id = s_is_mp4 ? AV_CODEC_ID_MPEG4 : (useFFV1 ? AV_CODEC_ID_FFV1 : s_format_context->oformat->video_codec);
	if (!useFFV1 && !s_is_mp4)
		s_codec_context->codec_tag = MKTAG('X', 'V', 'I', 'D');  // Force XVID FourCC for better compatibility
	s_codec_context->codec_type = AVMEDIA_TYPE_VIDEO;
	s_codec_context->bit_rate = s_is_mp4 ? 8000000 : 400000;
	s_codec_context->width = s_width;
	s_codec_context->height = s_height;
	s_codec_context->time_base.num = 1001;
	s_codec_context->time_base.den = 60000;
	s_codec_context->gop_size = 12;
	s_codec_context->pix_fmt = useFFV1 ? AV_PIX_FMT_BGRA : AV_PIX_FMT_YUV420P;
	// MP4 stores the codec setup in the file header rather than in the stream.
	if (s_format_context->oformat->flags & AVFMT_GLOBALHEADER)
		s_codec_context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(57, 48, 101)
	if (avcodec_parameters_from_context(s_stream->codecpar, s_codec_context) < 0)
		return false;
#endif

	codec = avcodec_find_encoder(s_codec_context->codec_id);
	if (!codec)
		return false;
	if (avcodec_open2(s_codec_context, codec, nullptr) < 0)
		return false;
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(57, 48, 101)
	// Again now that the encoder has produced its extradata, which MP4 needs.
	if (avcodec_parameters_from_context(s_stream->codecpar, s_codec_context) < 0)
		return false;
#endif

	if (s_is_mp4 && g_Config.bDumpAudio) {
		if (!avcodec_find_encoder(AV_CODEC_ID_AAC)) {
			WARN_LOG(Log::Common, "This FFmpeg build has no AAC encoder, recording MP4 without audio");
		} else if (!CreateAudioStream()) {
			WARN_LOG(Log::Common, "Could not set up AAC audio");
			return false;
		}
	}

	s_src_frame = av_frame_alloc();
	s_scaled_frame = av_frame_alloc();

	s_scaled_frame->format = s_codec_context->pix_fmt;
	s_scaled_frame->width = s_width;
	s_scaled_frame->height = s_height;

#if LIBAVCODEC_VERSION_MAJOR >= 55
	if (av_frame_get_buffer(s_scaled_frame, 1))
		return false;
#else
	if (avcodec_default_get_buffer(s_codec_context, s_scaled_frame))
		return false;
#endif

	NOTICE_LOG(Log::G3D, "Opening file '%s' for dumping", g_filename.ToVisualString().c_str());
	if (avio_open(&s_format_context->pb, filename, AVIO_FLAG_WRITE) < 0 || avformat_write_header(s_format_context, nullptr)) {
		WARN_LOG(Log::G3D, "Could not open %s", g_filename.ToVisualString().c_str());
		return false;
	}

	s_audioActive = s_audio_context != nullptr;
	return true;
#else
	return false;
#endif
}

#ifdef USE_FFMPEG

static void PreparePacket(AVPacket* pkt) {
	av_init_packet(pkt);
	pkt->data = nullptr;
	pkt->size = 0;
}

#endif

void AVIDump::AddFrame() {
	std::lock_guard<std::recursive_mutex> guard(s_dumpMutex);
#ifdef USE_FFMPEG
	if (!s_format_context || !s_codec_context)
		return;
#endif
	u32 w = 0;
	u32 h = 0;
	if (g_Config.bDumpVideoOutput) {
		gpu->GetOutputFramebuffer(buf);
		w = buf.GetStride();
		h = buf.GetHeight();
	} else {
		gpu->GetCurrentFramebuffer(buf, GPU_DBG_FRAMEBUF_RENDER);
		w = PSP_CoreParameter().renderWidth;
		h = PSP_CoreParameter().renderHeight;
	}
	CheckResolution(w, h);
	u8 *flipbuffer = nullptr;
	const u8 *buffer = ConvertBufferToScreenshot(buf, false, flipbuffer, w, h);

#ifdef USE_FFMPEG

	s_src_frame->data[0] = const_cast<u8*>(buffer);
	s_src_frame->linesize[0] = w * 3;
	s_src_frame->format = AV_PIX_FMT_RGB24;
	s_src_frame->width = s_width;
	s_src_frame->height = s_height;

	// Convert image from BGR24 to desired pixel format, and scale to initial width and height
	if ((s_sws_context = sws_getCachedContext(s_sws_context, w, h, AV_PIX_FMT_RGB24, s_width, s_height, s_codec_context->pix_fmt, SWS_BICUBIC, nullptr, nullptr, nullptr))) {
		sws_scale(s_sws_context, s_src_frame->data, s_src_frame->linesize, 0, h, s_scaled_frame->data, s_scaled_frame->linesize);
	}

	s_scaled_frame->format = s_codec_context->pix_fmt;
	s_scaled_frame->width = s_width;
	s_scaled_frame->height = s_height;
	s_scaled_frame->pts = s_video_pts++;

	// Encode and write the image.
	AVPacket pkt;
	PreparePacket(&pkt);
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(57, 48, 101)
	int error = avcodec_send_frame(s_codec_context, s_scaled_frame);
	int got_packet = 0;
	if (avcodec_receive_packet(s_codec_context, &pkt) >= 0) {
		got_packet = 1;
	}
#else
	int got_packet;
	int error = avcodec_encode_video2(s_codec_context, &pkt, s_scaled_frame, &got_packet);
#endif
	while (error >= 0 && got_packet) {
		// Write the compressed frame in the media file.
		if (pkt.pts != (s64)AV_NOPTS_VALUE) {
			pkt.pts = av_rescale_q(pkt.pts, s_codec_context->time_base, s_stream->time_base);
		}
		if (pkt.dts != (s64)AV_NOPTS_VALUE) {
			pkt.dts = av_rescale_q(pkt.dts, s_codec_context->time_base, s_stream->time_base);
		}
#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(56, 60, 100)
		if (s_codec_context->coded_frame->key_frame)
			pkt.flags |= AV_PKT_FLAG_KEY;
#endif
		pkt.stream_index = s_stream->index;
		av_interleaved_write_frame(s_format_context, &pkt);

		// Handle delayed frames.
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(57, 48, 101)
		av_packet_unref(&pkt);
		error = avcodec_receive_packet(s_codec_context, &pkt);
		got_packet = error >= 0 ? 1 : 0;
#else
		PreparePacket(&pkt);
		error = avcodec_encode_video2(s_codec_context, &pkt, nullptr, &got_packet);
#endif
	}
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(57, 48, 101)
	av_packet_unref(&pkt);
	if (error < 0 && error != AVERROR(EAGAIN) && error != AVERROR_EOF)
		ERROR_LOG(Log::G3D, "Error while encoding video: %d", error);
#else
	if (error < 0)
		ERROR_LOG(Log::G3D, "Error while encoding video: %d", error);
#endif
#endif
	delete[] flipbuffer;
}

#ifdef USE_FFMPEG

// Encodes one frame of audio (or flushes the encoder, with nullptr) and writes the packets.
static void EncodeAudioFrame(AVFrame *frame) {
	AVPacket pkt;
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(57, 48, 101)
	int error = avcodec_send_frame(s_audio_context, frame);
	if (error < 0 && error != AVERROR_EOF) {
		ERROR_LOG(Log::G3D, "Error while encoding audio: %d", error);
		return;
	}
	while (true) {
		PreparePacket(&pkt);
		if (avcodec_receive_packet(s_audio_context, &pkt) < 0)
			break;
		pkt.stream_index = s_audio_stream->index;
		av_packet_rescale_ts(&pkt, s_audio_context->time_base, s_audio_stream->time_base);
		av_interleaved_write_frame(s_format_context, &pkt);
		av_packet_unref(&pkt);
	}
#else
	int got_packet = 0;
	do {
		PreparePacket(&pkt);
		got_packet = 0;
		int error = avcodec_encode_audio2(s_audio_context, &pkt, frame, &got_packet);
		if (error < 0) {
			ERROR_LOG(Log::G3D, "Error while encoding audio: %d", error);
			return;
		}
		if (got_packet) {
			pkt.stream_index = s_audio_stream->index;
			av_packet_rescale_ts(&pkt, s_audio_context->time_base, s_audio_stream->time_base);
			av_interleaved_write_frame(s_format_context, &pkt);
			av_packet_unref(&pkt);
		}
		// With a frame, one call is enough. Without, keep going until the encoder is drained.
	} while (!frame && got_packet);
#endif
}

// Encodes the next frame_size samples from the queue.
static void EncodeQueuedAudioFrame() {
	const int frameSize = s_audio_frame->nb_samples;
	if (av_frame_make_writable(s_audio_frame) < 0)
		return;
	for (int c = 0; c < 2; c++) {
		memcpy(s_audio_frame->data[c], s_audio_fifo[c].data(), frameSize * sizeof(float));
		s_audio_fifo[c].erase(s_audio_fifo[c].begin(), s_audio_fifo[c].begin() + frameSize);
	}
	s_audio_frame->pts = s_audio_pts;
	s_audio_pts += frameSize;
	EncodeAudioFrame(s_audio_frame);
}

#endif

void AVIDump::AddAudioSamples(const s16 *samples, int numFrames) {
#ifdef USE_FFMPEG
	if (!s_audioActive)
		return;
	std::lock_guard<std::recursive_mutex> guard(s_dumpMutex);
	if (!s_audio_context)
		return;

	// s16 interleaved stereo in, planar float out, which is what the AAC encoder takes.
	for (int i = 0; i < numFrames; i++) {
		s_audio_fifo[0].push_back(samples[i * 2] * (1.0f / 32768.0f));
		s_audio_fifo[1].push_back(samples[i * 2 + 1] * (1.0f / 32768.0f));
	}
	while ((int)s_audio_fifo[0].size() >= s_audio_frame->nb_samples) {
		EncodeQueuedAudioFrame();
	}
#endif
}

bool AVIDump::IsRecordingAudio() {
	return s_audioActive;
}

Path AVIDump::LastFilename() {
	return g_filename;
}

void AVIDump::Stop() {
	std::lock_guard<std::recursive_mutex> guard(s_dumpMutex);
#ifdef USE_FFMPEG
	if (s_audio_context) {
		s_audioActive = false;
		// AAC needs whole frames, so pad the tail with silence.
		if (!s_audio_fifo[0].empty()) {
			for (int c = 0; c < 2; c++) {
				s_audio_fifo[c].resize(s_audio_frame->nb_samples, 0.0f);
			}
			EncodeQueuedAudioFrame();
		}
		EncodeAudioFrame(nullptr);
	}
	av_write_trailer(s_format_context);
	CloseFile();
	s_file_index = 0;
#endif
	NOTICE_LOG(Log::G3D, "Stopping frame dump to '%s'", g_filename.ToVisualString().c_str());
}

void AVIDump::CloseFile() {
#ifdef USE_FFMPEG
	if (s_codec_context) {
#if LIBAVCODEC_VERSION_MAJOR < 55
		avcodec_default_release_buffer(s_codec_context, s_src_frame);
#endif
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(57, 48, 101)
		avcodec_free_context(&s_codec_context);
#else
		avcodec_close(s_codec_context);
		s_codec_context = nullptr;
#endif
	}
	av_freep(&s_stream);

	s_audioActive = false;
	if (s_audio_context) {
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(57, 48, 101)
		avcodec_free_context(&s_audio_context);
#else
		avcodec_close(s_audio_context);
		s_audio_context = nullptr;
#endif
	}
	av_freep(&s_audio_stream);
	av_frame_free(&s_audio_frame);

	av_frame_free(&s_src_frame);
	av_frame_free(&s_scaled_frame);

	if (s_format_context)
	{
		if (s_format_context->pb)
			avio_close(s_format_context->pb);
		av_freep(&s_format_context);
	}

	if (s_sws_context)
	{
		sws_freeContext(s_sws_context);
		s_sws_context = nullptr;
	}
#endif
}

void AVIDump::CheckResolution(int width, int height) {
#ifdef USE_FFMPEG
	// We check here to see if the requested width and height have changed since the last frame which
	// was dumped, then create a new file accordingly. However, is it possible for the width and height
	// to have a value of zero. If this is the case, simply keep the last known resolution of the video
	// for the added frame.
	if ((width != s_current_width || height != s_current_height) && (width > 0 && height > 0))
	{
		int temp_file_index = s_file_index;
		Stop();
		s_file_index = temp_file_index + 1;
		Start(width, height);
		s_current_width = width;
		s_current_height = height;
	}
#endif // USE_FFMPEG
}
#endif
