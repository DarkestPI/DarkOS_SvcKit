# svc_board

`svc_board` 提供与具体产品无关的 JSON 配置解析和资源绑定校验。具体板型配置
由 Application 持有；本组件只定义配置模型和校验规则。

```cpp
darkos::BoardConfig board;
darkos::AppConfig app;
std::string error;

if (!darkos::BoardConfig::load("/etc/board.json", board, error) ||
    !darkos::AppConfig::load("/etc/app.json", app, error) ||
    !app.validate(board, error)) {
    // error 包含文件、字段或资源冲突信息
}
```

当前 Schema 版本是字符串 `0.0.1`，第一阶段只覆盖串口资源和串口业务绑定：

- Board：设备节点、电气类型、支持的波特率、是否独占；
- Application：服务名、引用的 Board 资源、实际波特率；
- 校验：严格字段类型、未知字段、资源存在性、波特率和独占冲突。

JSON 解析使用 `SvcKit/third_party/cJSON`，cJSON 作为 `svc_board` 的私有依赖，
不会出现在公开的 `BoardConfig/AppConfig` 头文件中。Camera、GPIO、Audio 等
资源应在确认公共字段和生命周期后按新的 Schema 版本逐步加入。
