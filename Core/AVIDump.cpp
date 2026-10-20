// Copyright 2009 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#ifndef MOBILE_DEVICE

#if defined(__FreeBSD__)
#define __STDC_CONSTANT_MACROS 1
#endif

#include <string>
#include <cstdint>
#include <sstream>
#include <algorithm>
#include <atomic>
#include <deque>
#include <mutex>
#include <vector>

#ifdef USE_FFMPEG

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/mathematics.h>
#include <libavutil/channel_layout.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
#include <libswresample/version.h>
#include <libswscale/swscale.h>
}

#endif

#include "Common/Data/Convert/ColorConv.h"
#include "Common/File/FileUtil.h"
#include "Common/File/Path.h"

#include "Core/Config.h"
#include "Core/AVIDump.h"
#include "Core/System.h"
#include "Core/CoreTiming.h"
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
static AVCodecContext *s_audio_codec_context = nullptr;
static AVStream *s_stream = nullptr;
static AVStream *s_audio_stream = nullptr;
static AVFrame *s_src_frame = nullptr;
static AVFrame *s_scaled_frame = nullptr;
static SwsContext *s_sws_context = nullptr;
static SwrContext *s_swr_context = nullptr;
static AVAudioFifo *s_audio_fifo = nullptr;
static std::atomic<bool> s_recording{false};
static std::mutex s_audio_queue_mutex;
struct QueuedAudioBlock {
	std::vector<s16> samples;
	u64 timeUs;
};
static std::deque<QueuedAudioBlock> s_audio_queue;
static u64 s_start_time_us = 0;
static s64 s_video_pts = -1;
static s64 s_audio_pts = 0;
static bool s_audio_pts_initialized = false;

#endif

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
	s_width = w & ~1;
	s_height = h & ~1;
	s_current_width = s_width;
	s_current_height = s_height;
#ifdef USE_FFMPEG
	g_filename = Path();
	s_video_pts = -1;
	s_audio_pts_initialized = false;
	s_start_time_us = CoreTiming::GetGlobalTimeUs();
	{
		std::lock_guard<std::mutex> guard(s_audio_queue_mutex);
		s_audio_queue.clear();
	}
#endif

	InitAVCodec();
	bool success = CreateMP4();
	if (!success) {
		CloseFile();
		if (File::Exists(g_filename))
			File::Delete(g_filename);
	}
#ifdef USE_FFMPEG
	else
		s_recording.store(true);
#endif
	return success;
}

bool AVIDump::CreateMP4() {
#ifdef USE_FFMPEG
	// Use gameID_EmulatedTimestamp for filename.
	std::string discID = g_paramSFO.GetDiscID();
	const std::string fileIndex = s_file_index == 0 ? "" : StringFromFormat("_%d", s_file_index);
	++s_file_index;
	g_filename = GetSysDirectory(DIRECTORY_VIDEO) / StringFromFormat("%s_%s%s.mp4", discID.c_str(), KernelTimeNowFormatted().c_str(), fileIndex.c_str());
	INFO_LOG(Log::Common, "Recording MP4 to: %s", g_filename.ToVisualString().c_str());

	if (!File::Exists(GetSysDirectory(DIRECTORY_VIDEO)))
		File::CreateDir(GetSysDirectory(DIRECTORY_VIDEO));
	if (File::Exists(g_filename))
		File::Delete(g_filename);

	if (avformat_alloc_output_context2(&s_format_context, nullptr, "mp4", g_filename.c_str()) < 0 || !s_format_context)
		return false;

	AVCodec *videoCodec = avcodec_find_encoder(AV_CODEC_ID_H264);
	if (!videoCodec)
		videoCodec = avcodec_find_encoder(AV_CODEC_ID_MPEG4);
	if (!videoCodec)
		return false;
	s_stream = avformat_new_stream(s_format_context, nullptr);
	s_codec_context = avcodec_alloc_context3(videoCodec);
	if (!s_stream || !s_codec_context)
		return false;
#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(57, 48, 101)
	s_stream->codec = s_codec_context;
#endif
	s_codec_context->codec_type = AVMEDIA_TYPE_VIDEO;
	s_codec_context->codec_id = videoCodec->id;
	s_codec_context->width = s_width;
	s_codec_context->height = s_height;
	s_codec_context->bit_rate = 2000000;
	s_codec_context->time_base = AVRational{ 1, 90000 };
	s_codec_context->framerate = AVRational{ 60, 1 };
	s_codec_context->gop_size = 60;
	s_codec_context->pix_fmt = AV_PIX_FMT_YUV420P;
	if (s_format_context->oformat->flags & AVFMT_GLOBALHEADER)
		s_codec_context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
	if (avcodec_open2(s_codec_context, videoCodec, nullptr) < 0)
		return false;
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(57, 48, 101)
	if (avcodec_parameters_from_context(s_stream->codecpar, s_codec_context) < 0)
		return false;
#endif

	AVCodec *audioCodec = avcodec_find_encoder(AV_CODEC_ID_AAC);
	if (!audioCodec)
		return false;
	s_audio_stream = avformat_new_stream(s_format_context, nullptr);
	s_audio_codec_context = avcodec_alloc_context3(audioCodec);
	if (!s_audio_stream || !s_audio_codec_context)
		return false;
#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(57, 48, 101)
	s_audio_stream->codec = s_audio_codec_context;
#endif
	s_audio_codec_context->codec_type = AVMEDIA_TYPE_AUDIO;
	s_audio_codec_context->codec_id = AV_CODEC_ID_AAC;
	s_audio_codec_context->sample_rate = 44100;
#if LIBAVUTIL_VERSION_MAJOR >= 57
	av_channel_layout_default(&s_audio_codec_context->ch_layout, 2);
#else
	s_audio_codec_context->channel_layout = AV_CH_LAYOUT_STEREO;
	s_audio_codec_context->channels = 2;
#endif
	s_audio_codec_context->sample_fmt = audioCodec->sample_fmts ? audioCodec->sample_fmts[0] : AV_SAMPLE_FMT_FLTP;
	s_audio_codec_context->time_base = AVRational{ 1, s_audio_codec_context->sample_rate };
	s_audio_codec_context->bit_rate = 128000;
	if (s_format_context->oformat->flags & AVFMT_GLOBALHEADER)
		s_audio_codec_context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
	if (avcodec_open2(s_audio_codec_context, audioCodec, nullptr) < 0)
		return false;
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(57, 48, 101)
	if (avcodec_parameters_from_context(s_audio_stream->codecpar, s_audio_codec_context) < 0)
		return false;
#endif

	s_audio_fifo = av_audio_fifo_alloc(s_audio_codec_context->sample_fmt, 2, 1);
	if (!s_audio_fifo)
		return false;
#if LIBSWRESAMPLE_VERSION_MAJOR >= 4
	AVChannelLayout inputLayout{};
	av_channel_layout_default(&inputLayout, 2);
	int swrResult = swr_alloc_set_opts2(&s_swr_context, &s_audio_codec_context->ch_layout, s_audio_codec_context->sample_fmt,
		s_audio_codec_context->sample_rate, &inputLayout, AV_SAMPLE_FMT_S16, 44100, 0, nullptr);
	av_channel_layout_uninit(&inputLayout);
	if (swrResult < 0)
		return false;
#else
	s_swr_context = swr_alloc_set_opts(nullptr, s_audio_codec_context->channel_layout, s_audio_codec_context->sample_fmt,
		s_audio_codec_context->sample_rate, AV_CH_LAYOUT_STEREO, AV_SAMPLE_FMT_S16, 44100, 0, nullptr);
#endif
	if (!s_swr_context || swr_init(s_swr_context) < 0)
		return false;

	s_src_frame = av_frame_alloc();
	s_scaled_frame = av_frame_alloc();
	if (!s_src_frame || !s_scaled_frame)
		return false;
	s_scaled_frame->format = s_codec_context->pix_fmt;
	s_scaled_frame->width = s_width;
	s_scaled_frame->height = s_height;
	if (av_frame_get_buffer(s_scaled_frame, 1) < 0)
		return false;

	if (!(s_format_context->oformat->flags & AVFMT_NOFILE) && avio_open(&s_format_context->pb, g_filename.c_str(), AVIO_FLAG_WRITE) < 0) {
		WARN_LOG(Log::G3D, "Could not open %s", g_filename.ToVisualString().c_str());
		return false;
	}
	if (avformat_write_header(s_format_context, nullptr) < 0)
		return false;
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

#ifdef USE_FFMPEG
static void WriteEncodedPacket(AVCodecContext *codec, AVStream *stream, AVPacket *packet) {
	packet->stream_index = stream->index;
	if (packet->pts != AV_NOPTS_VALUE)
		packet->pts = av_rescale_q(packet->pts, codec->time_base, stream->time_base);
	if (packet->dts != AV_NOPTS_VALUE)
		packet->dts = av_rescale_q(packet->dts, codec->time_base, stream->time_base);
	if (packet->duration > 0)
		packet->duration = av_rescale_q(packet->duration, codec->time_base, stream->time_base);
	av_interleaved_write_frame(s_format_context, packet);
}

static void EncodeFrame(AVCodecContext *codec, AVStream *stream, AVFrame *frame) {
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(57, 48, 101)
	int result = avcodec_send_frame(codec, frame);
	if (result < 0 && result != AVERROR_EOF) {
		ERROR_LOG(Log::G3D, "Could not send frame to encoder: %d", result);
		return;
	}
	AVPacket packet;
	PreparePacket(&packet);
	while ((result = avcodec_receive_packet(codec, &packet)) >= 0) {
		WriteEncodedPacket(codec, stream, &packet);
		av_packet_unref(&packet);
	}
	if (result != AVERROR(EAGAIN) && result != AVERROR_EOF)
		ERROR_LOG(Log::G3D, "Could not receive encoded packet: %d", result);
#else
	AVPacket packet;
	PreparePacket(&packet);
	int gotPacket = 0;
	int result = codec->codec_type == AVMEDIA_TYPE_VIDEO
		? avcodec_encode_video2(codec, &packet, frame, &gotPacket)
		: avcodec_encode_audio2(codec, &packet, frame, &gotPacket);
	if (result >= 0 && gotPacket) {
		WriteEncodedPacket(codec, stream, &packet);
		av_free_packet(&packet);
	}
#endif
}

static void EncodeQueuedAudioBlock(const QueuedAudioBlock &block) {
	if (!s_audio_codec_context || !s_swr_context || !s_audio_fifo || block.samples.empty())
		return;
	if (!s_audio_pts_initialized) {
		const s64 relativeUs = (s64)block.timeUs - (s64)s_start_time_us;
		s_audio_pts = av_rescale_q(std::max<s64>(0, relativeUs), AVRational{ 1, 1000000 }, s_audio_codec_context->time_base);
		s_audio_pts_initialized = true;
	}

	int capacity = (int)av_rescale_rnd(swr_get_delay(s_swr_context, 44100) + (int)block.samples.size() / 2,
		s_audio_codec_context->sample_rate, 44100, AV_ROUND_UP);
	AVFrame *converted = av_frame_alloc();
	if (!converted)
		return;
	converted->format = s_audio_codec_context->sample_fmt;
	converted->sample_rate = s_audio_codec_context->sample_rate;
#if LIBAVUTIL_VERSION_MAJOR >= 57
	av_channel_layout_copy(&converted->ch_layout, &s_audio_codec_context->ch_layout);
#else
	converted->channel_layout = s_audio_codec_context->channel_layout;
	converted->channels = 2;
#endif
	converted->nb_samples = capacity;
	if (av_frame_get_buffer(converted, 0) < 0) {
		av_frame_free(&converted);
		return;
	}
	const uint8_t *input[] = { reinterpret_cast<const uint8_t *>(block.samples.data()) };
	int convertedSamples = swr_convert(s_swr_context, converted->data, capacity, input, (int)block.samples.size() / 2);
	if (convertedSamples > 0) {
		av_audio_fifo_realloc(s_audio_fifo, av_audio_fifo_size(s_audio_fifo) + convertedSamples);
		av_audio_fifo_write(s_audio_fifo, (void **)converted->data, convertedSamples);
	}
	av_frame_free(&converted);

	const int frameSize = s_audio_codec_context->frame_size > 0 ? s_audio_codec_context->frame_size : 1024;
	while (av_audio_fifo_size(s_audio_fifo) >= frameSize) {
		AVFrame *frame = av_frame_alloc();
		if (!frame)
			return;
		frame->format = s_audio_codec_context->sample_fmt;
		frame->sample_rate = s_audio_codec_context->sample_rate;
#if LIBAVUTIL_VERSION_MAJOR >= 57
		av_channel_layout_copy(&frame->ch_layout, &s_audio_codec_context->ch_layout);
#else
		frame->channel_layout = s_audio_codec_context->channel_layout;
		frame->channels = 2;
#endif
		frame->nb_samples = frameSize;
		frame->pts = s_audio_pts;
		if (av_frame_get_buffer(frame, 0) < 0) {
			av_frame_free(&frame);
			return;
		}
		int read = av_audio_fifo_read(s_audio_fifo, (void **)frame->data, frameSize);
		if (read != frameSize) {
			av_frame_free(&frame);
			return;
		}
		EncodeFrame(s_audio_codec_context, s_audio_stream, frame);
		s_audio_pts += frameSize;
		av_frame_free(&frame);
	}
}

static void DrainAudioQueue() {
	std::deque<QueuedAudioBlock> blocks;
	{
		std::lock_guard<std::mutex> guard(s_audio_queue_mutex);
		blocks.swap(s_audio_queue);
	}
	for (const QueuedAudioBlock &block : blocks)
		EncodeQueuedAudioBlock(block);
}
#endif

void AVIDump::AddFrame() {
#ifdef USE_FFMPEG
	if (!s_recording.load())
		return;
	const u64 frameTimeUs = CoreTiming::GetGlobalTimeUs();
	DrainAudioQueue();
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
#ifdef USE_FFMPEG
	if (!s_recording.load())
		return;
#endif
	u8 *flipbuffer = nullptr;
	const u8 *buffer = ConvertBufferToScreenshot(buf, false, flipbuffer, w, h);

#ifdef USE_FFMPEG

	s_src_frame->data[0] = const_cast<u8*>(buffer);
	s_src_frame->linesize[0] = w * 3;
	s_src_frame->format = AV_PIX_FMT_RGB24;
	s_src_frame->width = s_width;
	s_src_frame->height = s_height;

	// Convert the framebuffer to the encoder's pixel format.
	s_sws_context = sws_getCachedContext(s_sws_context, w, h, AV_PIX_FMT_RGB24, s_width, s_height, s_codec_context->pix_fmt, SWS_BICUBIC, nullptr, nullptr, nullptr);
	if (!s_sws_context || av_frame_make_writable(s_scaled_frame) < 0) {
		delete[] flipbuffer;
		return;
	}
	if (sws_scale(s_sws_context, s_src_frame->data, s_src_frame->linesize, 0, h, s_scaled_frame->data, s_scaled_frame->linesize) < 0) {
		delete[] flipbuffer;
		return;
	}

	s_scaled_frame->format = s_codec_context->pix_fmt;
	s_scaled_frame->width = s_width;
	s_scaled_frame->height = s_height;
	s64 relativeTimeUs = (s64)frameTimeUs - (s64)s_start_time_us;
	s64 relativeTimeTicks = av_rescale_q(std::max<s64>(0, relativeTimeUs), AVRational{ 1, 1000000 }, s_codec_context->time_base);
	s_scaled_frame->pts = std::max<s64>(s_video_pts + 1, relativeTimeTicks);
	s_video_pts = s_scaled_frame->pts;
	EncodeFrame(s_codec_context, s_stream, s_scaled_frame);
#endif
	delete[] flipbuffer;
}

void AVIDump::AddAudio(const s16 *samples, int numSamples, u64 timeUs) {
#ifdef USE_FFMPEG
	if (!samples || numSamples <= 0 || !s_recording.load())
		return;
	QueuedAudioBlock block;
	block.samples.assign(samples, samples + numSamples * 2);
	block.timeUs = timeUs;
	std::lock_guard<std::mutex> guard(s_audio_queue_mutex);
	if (s_recording.load())
		s_audio_queue.push_back(std::move(block));
#endif
}

Path AVIDump::LastFilename() {
	return g_filename;
}

void AVIDump::Stop() {
#ifdef USE_FFMPEG
	if (!s_format_context)
		return;
	s_recording.store(false);
	DrainAudioQueue();
	if (s_audio_fifo && av_audio_fifo_size(s_audio_fifo) > 0) {
		const int frameSize = s_audio_codec_context->frame_size > 0 ? s_audio_codec_context->frame_size : 1024;
		AVFrame *frame = av_frame_alloc();
		if (frame) {
			frame->format = s_audio_codec_context->sample_fmt;
			frame->sample_rate = s_audio_codec_context->sample_rate;
#if LIBAVUTIL_VERSION_MAJOR >= 57
			av_channel_layout_copy(&frame->ch_layout, &s_audio_codec_context->ch_layout);
#else
			frame->channel_layout = s_audio_codec_context->channel_layout;
			frame->channels = 2;
#endif
			frame->nb_samples = frameSize;
			frame->pts = s_audio_pts;
			if (av_frame_get_buffer(frame, 0) >= 0) {
				av_samples_set_silence(frame->data, 0, frameSize, 2, s_audio_codec_context->sample_fmt);
				int count = av_audio_fifo_size(s_audio_fifo);
				int read = av_audio_fifo_read(s_audio_fifo, (void **)frame->data, count);
				if (read > 0)
					EncodeFrame(s_audio_codec_context, s_audio_stream, frame);
			}
			av_frame_free(&frame);
		}
	}
	EncodeFrame(s_codec_context, s_stream, nullptr);
	EncodeFrame(s_audio_codec_context, s_audio_stream, nullptr);
	av_write_trailer(s_format_context);
	CloseFile();
#endif
	NOTICE_LOG(Log::G3D, "Stopped MP4 recording: '%s'", g_filename.ToVisualString().c_str());
}

void AVIDump::CloseFile() {
#ifdef USE_FFMPEG
	s_recording.store(false);
	{
		std::lock_guard<std::mutex> guard(s_audio_queue_mutex);
		s_audio_queue.clear();
	}
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(57, 48, 101)
	avcodec_free_context(&s_codec_context);
	avcodec_free_context(&s_audio_codec_context);
#else
	if (s_codec_context) {
		avcodec_close(s_codec_context);
		s_codec_context = nullptr;
	}
	if (s_audio_codec_context) {
		avcodec_close(s_audio_codec_context);
		s_audio_codec_context = nullptr;
	}
#endif
	av_audio_fifo_free(s_audio_fifo);
	s_audio_fifo = nullptr;
	swr_free(&s_swr_context);
	av_frame_free(&s_src_frame);
	av_frame_free(&s_scaled_frame);
	if (s_format_context) {
		if (s_format_context->pb && (!s_format_context->oformat || !(s_format_context->oformat->flags & AVFMT_NOFILE)))
			avio_close(s_format_context->pb);
		avformat_free_context(s_format_context);
		s_format_context = nullptr;
	}
	s_stream = nullptr;
	s_audio_stream = nullptr;
	if (s_sws_context) {
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
		Stop();
		Start(width, height);
		s_current_width = width;
		s_current_height = height;
	}
#endif // USE_FFMPEG
}
#endif
