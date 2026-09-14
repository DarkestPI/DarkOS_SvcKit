#!/usr/bin/env bash

# SvcKit 宿主机测试统一入口。
# 默认完成 CMake 配置、编译全部宿主机测试，并输出失败用例的详细信息。

set -Eeuo pipefail

readonly SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
readonly SDK_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
# 使用独立构建目录，避免与具体 Application 的 CMake 缓存冲突。
readonly BUILD_DIR="${SDK_ROOT}/build/svckit-host-test"

show_usage() {
    printf '%s\n' \
        "用法：" \
        "  $0             配置、编译并运行全部 SvcKit 宿主机测试" \
        "  $0 <组件名>    只运行指定组件，例如 svc_log、svc_crypto、svc_keystore" \
        "  $0 --list      列出已经注册的宿主机测试" \
        "  $0 --help      显示本帮助"
}

require_command() {
    if ! command -v "$1" >/dev/null 2>&1; then
        printf '错误：未找到命令 %s。\n' "$1" >&2
        exit 1
    fi
}

configure_host_tests() {
    printf '==> 配置 SvcKit 宿主机测试\n'
    cmake \
        -S "${SDK_ROOT}" \
        -B "${BUILD_DIR}" \
        -DCMAKE_BUILD_TYPE=Debug \
        -DDARKOS_BUILD_TESTS=ON \
        -DDARKOS_BUILD_SECURITY_COMPONENTS=ON \
        -DDARKOS_ENABLE_OUTPUT_LAYOUT=OFF
}

require_command cmake
require_command ctest

case "${1:-}" in
    --help|-h)
        show_usage
        exit 0
        ;;
    --list)
        configure_host_tests
        ctest --test-dir "${BUILD_DIR}" --show-only
        exit 0
        ;;
    -* )
        printf '错误：未知选项 %s。\n' "$1" >&2
        show_usage >&2
        exit 2
        ;;
esac

if (( $# > 1 )); then
    printf '错误：最多只能指定一个组件名。\n' >&2
    show_usage >&2
    exit 2
fi

configure_host_tests

printf '==> 编译全部 SvcKit 宿主机测试\n'
cmake --build "${BUILD_DIR}" --target svckit_host_tests --parallel

if [[ -n "${1:-}" ]]; then
    # 将组件名视为普通文本，避免其中的正则表达式字符改变匹配范围。
    component_regex="$(printf '%s' "$1" | sed 's/[][\\.^$*+?{}|()]/\\&/g')"
    test_regex="^${component_regex}\\.host$"
    printf '==> 运行组件测试：%s\n' "$1"
else
    test_regex='^svc_.*\.host$'
    printf '==> 运行全部 SvcKit 宿主机测试\n'
fi

ctest \
    --test-dir "${BUILD_DIR}" \
    --output-on-failure \
    --no-tests=error \
    -R "${test_regex}"
