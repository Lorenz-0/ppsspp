#!/usr/bin/env bash
# Rebuild the bundled x64 FFmpeg libraries with the muxer/encoder needed by
# PPSSPP's MP4 gameplay recorder. Run from an MSYS2 shell started from the
# Visual Studio x64 Native Tools Command Prompt.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ffmpeg_dir="$repo_root/ffmpeg"
build_script="$ffmpeg_dir/windows_x64-build.sh"

if [[ ! -f "$build_script" ]]; then
	echo "Missing FFmpeg submodule build script: $build_script" >&2
	echo "Initialize the ffmpeg submodule first: git submodule update --init ffmpeg" >&2
	exit 1
fi

# PPSSPP's stock Windows FFmpeg build only enables AVI and PCM. MP4 recording
# needs the MP4/MOV muxer and FFmpeg's native AAC encoder. MPEG-4 Part 2 video
# is already enabled in that build and is used when H.264 encoding is absent.
if ! grep -q -- '--enable-encoder=aac' "$build_script"; then
	sed -i 's/--enable-encoder=pcm_s16le/--enable-encoder=pcm_s16le\n    --enable-encoder=aac/' "$build_script"
fi
if ! grep -q -- '--enable-muxer=mp4' "$build_script"; then
	sed -i 's/--enable-muxer=avi/--enable-muxer=avi\n    --enable-muxer=mp4\n    --enable-muxer=mov/' "$build_script"
fi

cd "$ffmpeg_dir"
./windows_x64-build.sh
