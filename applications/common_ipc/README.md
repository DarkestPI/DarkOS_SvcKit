# Common IPC application layer

这里放置 Ubuntu `generic_ipc` 和 RV1126B `rv1126b_ipc` 共同使用的应用编排代码：

- 配置和命令行选项解析；
- 摄像头、编码、存储和 RTSP 服务编排；
- IVA 示例调用；
- 通用媒体参数。

这里不放置具体芯片 SDK。应用通过 SvcKit 接口访问硬件，实际的 V4L2、Rockchip
MPI、RKNN 等实现仍位于 `platform/` 对应的平台适配层。

`generic_ipc` 和 `rv1126b_ipc` 仍然是两个独立可执行文件，只是在编译时共同使用
这里的源码。RV1126B 的 `DARKOS_CAMERA_ENCODED_MEDIA` 只对 RV 应用目标生效，
用于选择其编码视频输出路径。
