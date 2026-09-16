#!/usr/bin/env bash
set -e

ROOT="."
AUTHOR="your_name"
DATE=$(date +%Y-%m-%d)

# ---------- 目录 ----------
DIRS=(
    "$ROOT/inc"
    "$ROOT/src/core"
    "$ROOT/src/inference"
    "$ROOT/src/model"
    "$ROOT/src/algorithm/traditional"
    "$ROOT/src/algorithm/dnn"
    "$ROOT/src/tracker"
    "$ROOT/src/perimeter"
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

namespace iva {

} // namespace iva
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

namespace iva {

} // namespace iva
EOF
}

# ---------- 头文件 ----------
mk_header "$ROOT/inc/iva_types.h"           "IVA 基础类型定义（帧、目标、事件、枚举）"
mk_header "$ROOT/inc/iva_inference.h"       "推理引擎抽象接口（NPU / GPU / CPU）"
mk_header "$ROOT/inc/iva_model.h"           "模型管理接口（加载、版本、热更新）"
mk_header "$ROOT/inc/iva_algorithm.h"       "算法插件统一接口（传统 CV + 深度学习）"
mk_header "$ROOT/inc/iva_tracker.h"         "目标跟踪接口"
mk_header "$ROOT/inc/iva_rule.h"            "通用规则接口与规则引擎"
mk_header "$ROOT/inc/iva_perimeter.h"       "周界检测规则（入侵、越界、徘徊）"
mk_header "$ROOT/inc/iva_manager.h"         "IVA 管理器门面"

# ---------- core ----------
mk_source "$ROOT/src/core/iva_manager.cpp"   "IVA 管理器实现"       "iva_manager.h"
mk_source "$ROOT/src/core/iva_scheduler.cpp" "IVA 任务调度器"       "iva_manager.h"
mk_source "$ROOT/src/core/iva_stats.cpp"     "IVA 统计信息"         "iva_manager.h"

# ---------- model ----------
mk_source "$ROOT/src/model/model_manager.cpp" "模型管理实现"       "iva_model.h"

# ---------- inference ----------
mk_source "$ROOT/src/inference/cpu_engine.cpp"  "CPU 推理引擎"     "iva_inference.h"
mk_source "$ROOT/src/inference/rknn_engine.cpp" "瑞芯微 RKNN 推理引擎" "iva_inference.h"
mk_source "$ROOT/src/inference/nnie_engine.cpp" "海思 NNIE 推理引擎"   "iva_inference.h"
mk_source "$ROOT/src/inference/onnx_engine.cpp" "ONNX Runtime 推理引擎" "iva_inference.h"

# ---------- algorithm / traditional ----------
mk_source "$ROOT/src/algorithm/traditional/motion_algorithm.cpp" "移动侦测算法"   "iva_algorithm.h"
mk_source "$ROOT/src/algorithm/traditional/region_algorithm.cpp" "区域检测算法"   "iva_algorithm.h"

# ---------- algorithm / dnn ----------
mk_source "$ROOT/src/algorithm/dnn/person_detect.cpp"  "人形检测算法"   "iva_algorithm.h"
mk_source "$ROOT/src/algorithm/dnn/face_detect.cpp"    "人脸检测算法"   "iva_algorithm.h"
mk_source "$ROOT/src/algorithm/dnn/vehicle_detect.cpp" "车辆检测算法"   "iva_algorithm.h"

# ---------- tracker ----------
mk_source "$ROOT/src/tracker/byte_track_tracker.cpp" "ByteTrack 跟踪器" "iva_tracker.h"
mk_source "$ROOT/src/tracker/deep_sort_tracker.cpp"  "DeepSORT 跟踪器"  "iva_tracker.h"
mk_source "$ROOT/src/tracker/kalman_tracker.cpp"     "Kalman 跟踪器"    "iva_tracker.h"

# ---------- perimeter ----------
mk_source "$ROOT/src/perimeter/intrusion_rule.cpp"  "区域入侵规则"   "iva_perimeter.h"
mk_source "$ROOT/src/perimeter/line_cross_rule.cpp" "越界检测规则"   "iva_perimeter.h"
mk_source "$ROOT/src/perimeter/loitering_rule.cpp"  "徘徊检测规则"   "iva_perimeter.h"

# ---------- factory ----------
mk_source "$ROOT/src/factory/iva_factory.cpp" "IVA 工厂，桥接推理引擎与算法" "iva_manager.h"

echo "✅ $ROOT IVA 目录结构已生成"
echo
find "$ROOT" -type d | sort
echo
echo "文件总数: $(find "$ROOT" -type f | wc -l)"