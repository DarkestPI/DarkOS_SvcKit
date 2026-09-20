# 智能视频分析（Intelligent video analysis）


## 物体检测功能

人、车、宠物


## 周界检测 

周界功能有四种类型：

- 区域入侵：目标进入设定区域并停留超过设定时间触发；
- 区域进入：目标从设定区域外进入区域内触发；
- 区域离开：目标从设定区域内离开区域外触发；
- 越界检测：目标从设定的线段越过（符合设定方向）触发；


## 支持的功能

1. 视频帧接入
2. 预处理
3. 推理引擎抽象
4. 模型管理
5. 算法插件
6. 目标跟踪
7. 规则检测
8. 事件输出

## 当前实现状态

当前 IVA 已接入 SvcKit 构建，目标为 `svckit_iva`，上层也可以通过
`DarkOS::IVA` 链接。已完成一条不依赖具体 NPU 的规则分析链路：

- `FrameView`、`Detection`、`Track`、`Event` 和统计信息等基础类型；
- 可注入的 `Algorithm` 回调接口，便于接入不同推理后端；
- 基于 IoU 的目标跟踪，保证相邻帧目标 ID 稳定；
- 四个明确的周界规则接口：`createRegionIntrusionRule`（区域入侵）、
  `createRegionEnterRule`（区域进入）、`createRegionLeaveRule`（区域离开）和
  `createLineCrossRule`（越界检测）；
- `Manager` 门面，以及 `process()` 和 `processDetections()` 两种入口；
- 模型登记、激活、卸载的内存模型管理器；
- 宿主机测试 `svc_iva.pipeline`。

推理部分只依赖 Platform 的 `inference` HAL，不直接链接任何厂商 SDK。RV1126B
的 Rockchip HAL 内部使用 RKNN Runtime，负责模型加载、NV12/RGB/YUYV/灰度输入
转换、NCHW/NHWC 输入和输出 Tensor 快照。由于不同模型的输出布局不同，Detection
解码通过 `InferenceOutputDecoder` 注入。海思、联咏等平台只需实现同一套
`platform/interfaces/inference` SPI，IVA 核心不需要增加厂商枚举或 SDK 依赖。

### Platform 推理调用方式

```cpp
iva::InferenceEngineConfig config;
config.backend = iva::InferenceBackend::Platform;
config.decoder = decodeMyModelOutputs; // 模型专属输出解码/NMS

std::string error;
auto engine = iva::createInferenceEngine(config, error);
iva::ModelInfo model{"person", "1.0", "/oem/models/person.model"};
if (!engine || engine->loadModel(model, error) != 0)
    return false;

std::vector<iva::Detection> detections;
if (engine->infer(frame, detections, error) != 0)
    return false;

std::vector<iva::Event> events;
ivaManager->processDetections(frame.timestampNs, detections, events);
```

`decodeMyModelOutputs` 只处理该模型的输出张量，例如类别、置信度、框坐标
和 NMS；Platform Runtime 不假定模型一定是某个 YOLO 版本。模型格式由当前
平台适配器解释，例如 RV1126B 的 inference HAL 识别 RKNN 模型文件。
