# RV1126B IPC 板级配置

`rv1126b_ipc_v1.json` 描述板上的硬件资源，不描述 Pelco-D 等业务协议。
Application 的 `etc/app.json` 负责把业务服务绑定到这些资源。

当前串口路径是第一版装配值。接板前必须根据产品原理图、设备树和目标 rootfs
中的实际设备节点核实 `/dev/ttyS1`、`/dev/ttyS3`。RS-485 收发方向由内核
`TIOCSRS485` 还是额外 GPIO 控制，也需要在 Serial HAL 落地前确认。
