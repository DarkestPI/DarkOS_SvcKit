#!/usr/bin/env bash
set -e

ROOT="."
AUTHOR="your_name"
DATE=$(date +%Y-%m-%d)

# ---------- 目录 ----------
DIRS=(
    "$ROOT/inc"
    "$ROOT/src/core"
    "$ROOT/src/device"
    "$ROOT/src/index"
    "$ROOT/src/record"
    "$ROOT/src/snapshot"
    "$ROOT/src/playback"
    "$ROOT/src/cleanup"
    "$ROOT/src/upload"
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

namespace storage {

} // namespace storage
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

namespace storage {

} // namespace storage
EOF
}

# ---------- 头文件 ----------
mk_header "$ROOT/inc/storage_types.h"     "存储基础类型定义（配置、信息、事件、枚举）"
mk_header "$ROOT/inc/storage_device.h"    "存储介质抽象接口"
mk_header "$ROOT/inc/storage_index.h"     "索引与元数据接口"
mk_header "$ROOT/inc/storage_record.h"    "录像写入接口"
mk_header "$ROOT/inc/storage_snapshot.h"  "图片抓拍接口"
mk_header "$ROOT/inc/storage_playback.h"  "回放读取接口"
mk_header "$ROOT/inc/storage_cleanup.h"   "清理策略接口"
mk_header "$ROOT/inc/storage_upload.h"    "上传队列接口"
mk_header "$ROOT/inc/storage_manager.h"   "存储管理器门面"

# ---------- core ----------
mk_source "$ROOT/src/core/storage_manager.cpp"   "存储管理器实现"     "storage_manager.h"
mk_source "$ROOT/src/core/storage_scheduler.cpp" "存储任务调度器"     "storage_manager.h"
mk_source "$ROOT/src/core/storage_stats.cpp"     "存储统计信息"       "storage_manager.h"

# ---------- device ----------
mk_source "$ROOT/src/device/local_device.cpp"   "本地介质（SD/eMMC/USB）" "storage_device.h"
mk_source "$ROOT/src/device/device_monitor.cpp" "介质健康与插拔监控"      "storage_device.h"

# ---------- index ----------
mk_source "$ROOT/src/index/sqlite_index.cpp"   "SQLite 索引实现"    "storage_index.h"
mk_source "$ROOT/src/index/index_repair.cpp"   "索引修复与校验"     "storage_index.h"

# ---------- record ----------
mk_source "$ROOT/src/record/recorder.cpp"          "录像写入实现"       "storage_record.h"
mk_source "$ROOT/src/record/record_slice.cpp"      "录像切片管理"       "storage_record.h"
mk_source "$ROOT/src/record/ring_buffer.cpp"       "预录环形缓冲"       "storage_record.h"
mk_source "$ROOT/src/record/mp4_record_file.cpp"   "MP4 录像文件"       "storage_record.h"
mk_source "$ROOT/src/record/ts_record_file.cpp"    "TS 录像文件"        "storage_record.h"

# ---------- snapshot ----------
mk_source "$ROOT/src/snapshot/snapshot.cpp"          "抓拍实现"       "storage_snapshot.h"
mk_source "$ROOT/src/snapshot/jpeg_encoder.cpp"      "JPEG 编码"      "storage_snapshot.h"
mk_source "$ROOT/src/snapshot/thumbnail.cpp"         "缩略图生成"     "storage_snapshot.h"

# ---------- playback ----------
mk_source "$ROOT/src/playback/playback_session.cpp" "回放会话实现"   "storage_playback.h"
mk_source "$ROOT/src/playback/playback_query.cpp"   "录像检索"       "storage_playback.h"
mk_source "$ROOT/src/playback/download.cpp"         "录像下载"       "storage_playback.h"

# ---------- cleanup ----------
mk_source "$ROOT/src/cleanup/cleanup_policy.cpp"    "清理策略实现"   "storage_cleanup.h"
mk_source "$ROOT/src/cleanup/space_manager.cpp"     "空间配额管理"   "storage_cleanup.h"

# ---------- upload ----------
mk_source "$ROOT/src/upload/upload_queue.cpp"       "上传队列实现"   "storage_upload.h"
mk_source "$ROOT/src/upload/upload_task.cpp"        "上传任务与重试" "storage_upload.h"
mk_source "$ROOT/src/upload/upload_persist.cpp"     "上传任务持久化" "storage_upload.h"

# ---------- factory ----------
mk_source "$ROOT/src/factory/storage_factory.cpp" "存储工厂" "storage_manager.h"

echo "✅ $ROOT 存储子系统目录结构已生成"
echo
find "$ROOT" -type d | sort
echo
echo "文件总数: $(find "$ROOT" -type f | wc -l)"