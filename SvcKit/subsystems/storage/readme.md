# 存储子系统

## 已实现

- `StorageManager` 管理存储目录、持久化索引和容量统计；
- `Recorder` 直接作为 Media Pipeline 的 `VideoPacketSink`；
- H.264/H.265/MJPEG 编码包原样落盘，按容量并在关键帧切片；
- 录像按 label、alarm id 和时间范围查询；
- 按 offset 回放/下载读取；
- 索引重启恢复、删除及最大占用/最小剩余空间清理。

## 边界

当前录像文件是可直接解码的 elementary stream（`.h264`/`.h265`/`.mjpg`）。
MP4/TS mux、JPEG 抓拍、缩略图和云上传目前仍为预留骨架，未加入构建。
