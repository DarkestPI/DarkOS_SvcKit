
# RV1126B IPC Application

本项目在启动时读取两类 JSON 配置，并可启动与 `generic_ipc` 相同的摄像头、编码视频
Rockchip `VI -> VENC` 硬件直连、存储和 RTSP 服务：

- `boards/<board>.json`：板级硬件资源清单；
- `etc/app.json`：业务服务到硬件资源的绑定。

默认构建打包 `rv1126b_ipc_v1`：

```bash
cmake --preset rv1126b_ipc -DDARKOS_BOARD=rv1126b_ipc_v1
cmake --build --preset rv1126b_ipc -j
```

Preset 使用 `DARKOS_VENDOR=rockchip` 和 `DARKOS_SOC=rv1126b` 选择具体芯片 HAL，
生成 `output/rv1126b_ipc/lib/hal.rockchip.rv1126b.so`。

构建结果中的配置为 `output/rv1126b_ipc/etc/board.json`、
`output/rv1126b_ipc/etc/app.json` 和 `output/rv1126b_ipc/etc/iqfiles/`。其中
`iqfiles/gc8613_default_default.json` 是当前 GC8613 模组匹配的 ISP IQ 文件。
安装到设备后程序默认从可执行文件旁边的
`../etc` 读取，也可以显式指定配置：

```bash
/oem/usr2/bin/rv1126b_ipc --serve
```

RTSP 默认地址为 `rtsp://<设备IP>:8554/live`，默认认证为 `admin/admin`。
标准 `/oem/usr2` 输出布局已经把 HAL 和 Rockchip 运行库放在程序旁边的 `lib/`，
并设置了相对运行时搜索路径，通常不需要额外设置 `LD_LIBRARY_PATH`。非标准部署
目录仍可使用 `LD_LIBRARY_PATH` 覆盖。程序会优先使用
`/oem/usr2/etc/iqfiles` 中的 IQ 文件，也可以通过 `DARKOS_IQ_FILE_DIR` 覆盖。
当前实验视频配置为 3840x2160、30 fps、H.264 Main、8 Mbps；RV1126B 路径在
Camera HAL 内通过 `RK_MPI_SYS_Bind(VI, VENC)` 建立通路，应用层只读取已经编码的
H.264 包再分发到 RTSP/录像，因此不会复制 4K 原始 NV12 帧。音频为 16 kHz
单声道 G.711A。4K RTSP 需要约 1 MB/s 网络带宽和更高的存储吞吐。

不带 `--serve` 时仅执行配置校验并退出；`SIGINT`/`SIGTERM` 会正常停止采集和 RTSP 服务。

## IVA / RKNN 调用示例

RV1126B 的 `rv1126b_ipc` 通过 Platform inference HAL 调用 RKNN。当前 SDK 中可直接
验证的模型是 `model.int8.320x240.rv1126b.rknn`，先把它放到
`/oem/usr2/etc/models/`，然后在
`/oem/usr2/etc/app.json` 中启用 IVA：

```json
"iva": {
  "enabled": true,
  "model_path": "/oem/usr2/etc/models/model.int8.320x240.rv1126b.rknn"
}
```

之后不需要再把模型路径写在命令行：

```bash
/oem/usr2/bin/rv1126b_ipc
```

仍然可以用 `--iva-model` 或 `DARKOS_IVA_MODEL` 临时覆盖 `app.json`。

该示例提交一帧合成的 640x640 NV12 数据，用于验证模型加载、硬件推理 HAL 和 IVA
Manager 的调用链；当前示例解码器只记录输出 Tensor 数量，尚未内置某一个 YOLO
模型的 Tensor 排布、阈值和 NMS 规则。接入实际模型时，需要在应用侧替换该解码器。
正常运行应看到 `IVA example completed`；模型格式、输入尺寸或 RKNN runtime 不匹配时，
程序会直接打印 RKNN 错误并返回失败。

## 原始项目模板说明

cpp_project_template/
├── CMakeLists.txt                 # 根CMake配置
├── .clang-format                  # 代码格式化配置
├── .clang-tidy                    # 静态分析配置
├── .gitignore
├── README.md
├── cmake/                         # 自定义CMake模块
│   ├── FindDependencies.cmake
│   └── CodeCoverage.cmake
├── include/                       # 公共头文件
│   └── rv1126b_ipc/
├── src/                           # 源代码
│   ├── main.cpp
│   └── lib/
├── tests/                         # 单元测试
│   ├── CMakeLists.txt
│   └── test_basic.cpp
├── third_party/                   # 第三方依赖
├── scripts/                       # 工具脚本
│   └── setup_project.py
├── .github/workflows/             # CI/CD流水线
│   └── ci.yml
└── docs/                          # 项目文档
    └── design.md
