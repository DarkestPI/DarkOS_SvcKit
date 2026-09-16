#!/usr/bin/env bash
set -e

ROOT="."
AUTHOR="your_name"
DATE=$(date +%Y-%m-%d)

# ---------- 目录 ----------
DIRS=(
    "$ROOT/inc"
    "$ROOT/src/core"
    "$ROOT/src/buffer"
    "$ROOT/src/filter"
    "$ROOT/src/muxer"
    "$ROOT/src/source"
    "$ROOT/src/sink"
    "$ROOT/src/codec"
    "$ROOT/src/factory"
    "$ROOT/test/unit"
    "$ROOT/test/example"
)

for d in "${DIRS[@]}"; do
    mkdir -p "$d"
done

# ---------- 工具函数 ----------
mk_header() {
    local path="$1"
    local brief="$2"
    cat > "$path" <<EOF
/**
 * @file $(basename "$path")
 * @brief $brief
 * @author $AUTHOR
 * @date $DATE
 */
#pragma once

namespace media {

} // namespace media
EOF
}

mk_source() {
    local path="$1"
    local brief="$2"
    local include="$3"
    cat > "$path" <<EOF
/**
 * @file $(basename "$path")
 * @brief $brief
 * @author $AUTHOR
 * @date $DATE
 */

#include "$include"

namespace media {

} // namespace media
EOF
}

# ---------- 头文件 ----------
mk_header "$ROOT/inc/media_types.h"          "媒体基础类型定义（帧、包、参数、枚举）"
mk_header "$ROOT/inc/media_buffer.h"         "媒体缓冲区与内存池抽象"
mk_header "$ROOT/inc/media_video_source.h"   "视频采集源接口"
mk_header "$ROOT/inc/media_audio_source.h"   "音频采集源接口"
mk_header "$ROOT/inc/media_video_encoder.h"  "视频编码器接口"
mk_header "$ROOT/inc/media_video_decoder.h"  "视频解码器接口"
mk_header "$ROOT/inc/media_audio_codec.h"    "音频编解码器接口"
mk_header "$ROOT/inc/media_video_filter.h"   "视频滤镜接口"
mk_header "$ROOT/inc/media_audio_filter.h"   "音频滤镜接口"
mk_header "$ROOT/inc/media_video_sink.h"     "视频输出（显示）接口"
mk_header "$ROOT/inc/media_audio_sink.h"     "音频输出（播放）接口"
mk_header "$ROOT/inc/media_muxer.h"          "封装/解封装接口"
mk_header "$ROOT/inc/media_factory.h"        "工厂接口，桥接 SoC 适配层"
mk_header "$ROOT/inc/media_manager.h"        "媒体管理器门面"

# ---------- core ----------
mk_source "$ROOT/src/core/media_manager.cpp"   "媒体管理器实现"     "media_manager.h"
mk_source "$ROOT/src/core/media_pipeline.cpp"  "媒体管线编排"       "media_manager.h"
mk_source "$ROOT/src/core/media_scheduler.cpp" "任务调度器"         "media_manager.h"
mk_source "$ROOT/src/core/media_stats.cpp"     "统计信息实现"       "media_manager.h"

# ---------- buffer ----------
mk_source "$ROOT/src/buffer/media_buffer.cpp" "缓冲区实现"         "media_buffer.h"
mk_source "$ROOT/src/buffer/buffer_pool.cpp"  "内存池实现"         "media_buffer.h"

# ---------- filter ----------
mk_source "$ROOT/src/filter/scale_filter.cpp"    "缩放滤镜"           "media_video_filter.h"
mk_source "$ROOT/src/filter/csc_filter.cpp"      "色彩空间转换滤镜"   "media_video_filter.h"
mk_source "$ROOT/src/filter/crop_filter.cpp"     "裁剪滤镜"           "media_video_filter.h"
mk_source "$ROOT/src/filter/rotate_filter.cpp"   "旋转/镜像滤镜"      "media_video_filter.h"
mk_source "$ROOT/src/filter/osd_filter.cpp"      "OSD 叠加滤镜"       "media_video_filter.h"
mk_source "$ROOT/src/filter/resample_filter.cpp" "音频重采样滤镜"     "media_audio_filter.h"
mk_source "$ROOT/src/filter/aec_filter.cpp"      "回声消除滤镜"       "media_audio_filter.h"

# ---------- muxer ----------
mk_source "$ROOT/src/muxer/mp4_muxer.cpp"  "MP4 封装实现"    "media_muxer.h"
mk_source "$ROOT/src/muxer/ts_muxer.cpp"   "TS 封装实现"     "media_muxer.h"
mk_source "$ROOT/src/muxer/rtsp_muxer.cpp" "RTSP 封装实现"   "media_muxer.h"

# ---------- source ----------
mk_source "$ROOT/src/source/file_video_source.cpp"   "文件视频源"       "media_video_source.h"
mk_source "$ROOT/src/source/file_audio_source.cpp"   "文件音频源"       "media_audio_source.h"
mk_source "$ROOT/src/source/test_pattern_source.cpp" "测试图案视频源"   "media_video_source.h"

# ---------- sink ----------
mk_source "$ROOT/src/sink/file_video_sink.cpp" "文件视频输出"     "media_video_sink.h"
mk_source "$ROOT/src/sink/file_audio_sink.cpp" "文件音频输出"     "media_audio_sink.h"
mk_source "$ROOT/src/sink/null_sink.cpp"       "空输出（丢弃）"   "media_video_sink.h"

# ---------- codec ----------
mk_source "$ROOT/src/codec/software_video_encoder.cpp" "软件视频编码"   "media_video_encoder.h"
mk_source "$ROOT/src/codec/software_video_decoder.cpp" "软件视频解码"   "media_video_decoder.h"
mk_source "$ROOT/src/codec/aac_encoder.cpp"            "AAC 编码"       "media_audio_codec.h"
mk_source "$ROOT/src/codec/aac_decoder.cpp"            "AAC 解码"       "media_audio_codec.h"

# ---------- factory ----------
mk_source "$ROOT/src/factory/media_factory.cpp" "工厂实现，桥接 SoC 层" "media_factory.h"

echo "✅ $ROOT 目录结构已生成"
echo
find "$ROOT" -type d | sort
echo
echo "文件总数: $(find "$ROOT" -type f | wc -l)"