#!/usr/bin/env bash
# DarkOS SDK 根目录构建入口。
#
# 用法：
#   ./build.sh ide              生成 clangd 编译数据库：配置根目录 host 工程，
#                               并把 rv1126b 厂商目录链接到 rv1126b_ipc 的
#                               交叉编译数据库（详见 README「IDE 跳转」一节）
#   ./build.sh host [参数...]    配置并构建根目录 host 工程（默认构建测试聚合
#                               目标 svckit_host_tests；SDK 目标按设计不进
#                               all，可用 --target xxx 指定其它目标）
#   ./build.sh test [参数...]    构建 host 测试并运行 ctest
#   ./build.sh app <name>        按应用自己的 preset 配置并构建
#                               applications/<name>（如 generic_ipc、rv1126b_ipc）
#   ./build.sh clean [<name>]    删除应用的 build 目录；省略 <name> 清理全部应用
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_BUILD="$ROOT/build"
RV_DB_DIR="$ROOT/platform/vendors/rockchip/socs/rv1126b/build"
RV_APP_DB="$ROOT/applications/rv1126b_ipc/build/compile_commands.json"

usage() {
    sed -n '2,14p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//; s/^#//'
}

# 根目录 host 工程：不写 output/（ENABLE_OUTPUT_LAYOUT=OFF），产物与编译
# 数据库都留在 build/，ctest 也能在构建树里找到可执行文件。
configure_host() {
    cmake -S "$ROOT" -B "$ROOT_BUILD" \
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
        -DDARKOS_BUILD_HAL_IMPLEMENTATIONS=ON \
        -DDARKOS_BUILD_TESTS=ON \
        -DDARKOS_PLATFORM=ubuntu_x86_64 \
        -DDARKOS_ENABLE_OUTPUT_LAYOUT=OFF \
        "$@"
}

cmd_ide() {
    echo "==> 配置根目录 host 工程（clangd 数据库）"
    configure_host
    echo "==> 链接 rv1126b 厂商编译数据库"
    mkdir -p "$RV_DB_DIR"
    ln -sfn ../../../../../../applications/rv1126b_ipc/build/compile_commands.json \
        "$RV_DB_DIR/compile_commands.json"
    if [ ! -f "$RV_APP_DB" ]; then
        echo "提示：$RV_APP_DB 尚不存在，先执行 ./build.sh app rv1126b_ipc 后再试"
    fi
    echo "完成。VSCode 中执行 \"clangd: Restart language server\" 后生效。"
}

cmd_host() {
    configure_host
    cmake --build "$ROOT_BUILD" --target svckit_host_tests "$@"
}

cmd_test() {
    cmake --build "$ROOT_BUILD" --target svckit_host_tests >/dev/null
    ctest --test-dir "$ROOT_BUILD" --output-on-failure "$@"
}

cmd_app() {
    local name="${1:-}"
    if [ -z "$name" ] || [ ! -f "$ROOT/applications/$name/CMakePresets.json" ]; then
        echo "用法：./build.sh app <name>（applications 目录下的应用名）" >&2
        exit 1
    fi
    (cd "$ROOT/applications/$name" && cmake --preset "$name" && cmake --build --preset "$name")
}

# 删除单个应用的构建目录。只认含 CMakeCache.txt/CMakeFiles 的目录，避免误删
# 恰好叫 build 的普通源码目录。
clean_one() {
    local app="$1"
    local dir="$ROOT/applications/$app/build"

    if [ ! -d "$dir" ]; then
        echo "跳过 $app：无 build 目录"
        return 0
    fi
    if [ ! -f "$dir/CMakeCache.txt" ] && [ ! -d "$dir/CMakeFiles" ]; then
        echo "跳过 $app：$dir 不是 CMake 构建目录" >&2
        return 0
    fi
    rm -rf "$dir"
    echo "已清理 $app/build"
    if [ "$app" = "rv1126b_ipc" ]; then
        echo "提示：rv1126b 厂商跳转库已悬空，重建应用（./build.sh app rv1126b_ipc）后自动恢复"
    fi
    return 0
}

cmd_clean() {
    local name="${1:-}"

    if [ -n "$name" ]; then
        if [[ ! "$name" =~ ^[A-Za-z0-9_-]+$ ]] ||
           [ ! -f "$ROOT/applications/$name/CMakePresets.json" ]; then
            echo "用法：./build.sh clean [<name>]（applications 下的应用名，省略则清理全部）" >&2
            exit 1
        fi
        clean_one "$name"
        return
    fi

    local app
    for app in "$ROOT"/applications/*/; do
        app="$(basename "$app")"
        if [ -f "$ROOT/applications/$app/CMakePresets.json" ]; then
            clean_one "$app"
        fi
    done
}

case "${1:-help}" in
    ide) shift && cmd_ide "$@" ;;
    host) shift && cmd_host "$@" ;;
    test) shift && cmd_test "$@" ;;
    app) shift && cmd_app "$@" ;;
    clean) shift && cmd_clean "$@" ;;
    *) usage ;;
esac
