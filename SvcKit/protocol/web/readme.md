# Web 前端

多客户端控制面的 Web 前端（本期为骨架，不实现）。三件事共用同一自研
HTTP 服务器底座（挂 EventLoop）：

1. **静态页面托管**：设备管理页面（HTML/JS 固化进固件或随固件打包）
2. **私有 HTTP REST API**（端点表见 docs/控制面设计.md 第 5 节）：

   | 端点 | 说明 |
   |---|---|
   | `GET /api/v1/params` | 全部参数（JSON 数组，含 schema） |
   | `GET /api/v1/params/<name>` | 单个参数 |
   | `PUT /api/v1/params/<name>` | 设值 `{"value": ...}` |
   | `POST /api/v1/actions/<动作>` | 动作（reboot 等） |
   | `GET /api/v1/events`（WS） | 事件推送（参数变更、告警等） |

   认证沿用 Digest（与 RTSP 同账号体系）；响应统一 `{"ok": ...}` 形态，
   `reboot` 生效参数带 `"reboot_required": true`。
3. **WebRTC 信令**：浏览器低延迟预览（远期），信令走本 HTTP 底座

纪律：本层只做翻译，get/set/动作全部落到 Services/control 的
ControlService（设备模型唯一权威），不含业务判断。
