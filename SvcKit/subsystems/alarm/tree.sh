#!/usr/bin/env bash
set -e

ROOT="."
AUTHOR="your_name"
DATE=$(date +%Y-%m-%d)

# ---------- 目录 ----------
DIRS=(
    "$ROOT/inc"
    "$ROOT/src/core"
    "$ROOT/src/source"
    "$ROOT/src/rule"
    "$ROOT/src/action"
    "$ROOT/src/store"
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

namespace alarm {

} // namespace alarm
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

namespace alarm {

} // namespace alarm
EOF
}

# ---------- 头文件 ----------
mk_header "$ROOT/inc/alarm_types.h"    "报警基础类型定义（报警、状态、级别、枚举）"
mk_header "$ROOT/inc/alarm_source.h"   "报警源抽象接口"
mk_header "$ROOT/inc/alarm_rule.h"     "报警规则接口（布防、计划、去重、升级）"
mk_header "$ROOT/inc/alarm_action.h"   "报警联动动作接口"
mk_header "$ROOT/inc/alarm_store.h"    "报警记录存储接口"
mk_header "$ROOT/inc/alarm_manager.h"  "报警管理器门面"

# ---------- core ----------
mk_source "$ROOT/src/core/alarm_manager.cpp"   "报警管理器实现"       "alarm_manager.h"
mk_source "$ROOT/src/core/alarm_scheduler.cpp" "报警任务调度器"       "alarm_manager.h"
mk_source "$ROOT/src/core/alarm_stats.cpp"     "报警统计信息"         "alarm_manager.h"

# ---------- source ----------
mk_source "$ROOT/src/source/iva_source.cpp"     "IVA 事件报警源"       "alarm_source.h"
mk_source "$ROOT/src/source/io_source.cpp"      "IO 输入报警源"        "alarm_source.h"
mk_source "$ROOT/src/source/network_source.cpp" "网络报警源（ONVIF/GB28181）" "alarm_source.h"
mk_source "$ROOT/src/source/manual_source.cpp"  "手动报警源"           "alarm_source.h"
mk_source "$ROOT/src/source/system_source.cpp"  "系统报警源（硬盘满/断网/温度）" "alarm_source.h"

# ---------- rule ----------
mk_source "$ROOT/src/rule/arm_rule.cpp"       "布防撤防规则"       "alarm_rule.h"
mk_source "$ROOT/src/rule/schedule_rule.cpp"  "时间段规则"         "alarm_rule.h"
mk_source "$ROOT/src/rule/filter_rule.cpp"    "去重与屏蔽规则"     "alarm_rule.h"
mk_source "$ROOT/src/rule/priority_rule.cpp"  "优先级升级规则"     "alarm_rule.h"

# ---------- action ----------
mk_source "$ROOT/src/action/record_action.cpp"   "报警录像联动"     "alarm_action.h"
mk_source "$ROOT/src/action/snapshot_action.cpp" "报警抓拍联动"     "alarm_action.h"
mk_source "$ROOT/src/action/io_output_action.cpp" "IO 输出联动"     "alarm_action.h"
mk_source "$ROOT/src/action/notify_action.cpp"   "通知推送联动"     "alarm_action.h"
mk_source "$ROOT/src/action/ptz_action.cpp"      "PTZ 联动"        "alarm_action.h"
mk_source "$ROOT/src/action/light_action.cpp"    "声光联动"        "alarm_action.h"

# ---------- store ----------
mk_source "$ROOT/src/store/alarm_store.cpp"  "报警记录存储实现"   "alarm_store.h"
mk_source "$ROOT/src/store/alarm_query.cpp"  "报警检索"           "alarm_store.h"

# ---------- factory ----------
mk_source "$ROOT/src/factory/alarm_factory.cpp" "报警工厂" "alarm_manager.h"

echo "✅ $ROOT 报警子系统目录结构已生成"
echo
find "$ROOT" -type d | sort
echo
echo "文件总数: $(find "$ROOT" -type f | wc -l)"