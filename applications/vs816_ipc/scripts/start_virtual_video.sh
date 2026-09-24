#!/bin/sh

set -eu

APP_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
VIDEO_FILE=${1:-"$APP_ROOT/data/sample-5s.h264"}

if [ ! -r "$VIDEO_FILE" ]; then
    echo "virtual video file is not readable: $VIDEO_FILE" >&2
    exit 1
fi

export LD_LIBRARY_PATH="$APP_ROOT/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export DARKOS_HAL_LIBRARY_PATH="$APP_ROOT/lib"
export DARKOS_HAL_VARIANT=visinextek.vs816
export DARKOS_H264_FILE="$VIDEO_FILE"
export DARKOS_H264_FPS=${DARKOS_H264_FPS:-30}

exec "$APP_ROOT/bin/vs816_ipc" --serve
