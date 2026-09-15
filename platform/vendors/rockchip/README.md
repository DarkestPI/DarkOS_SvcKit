# Rockchip HAL

`common/` 只保存经确认能被多个 Rockchip SoC 复用的辅助代码；每颗芯片的实现放在
`socs/<soc>/`。当前支持 `rv1126b`，构建目标为 `hal_rockchip_rv1126b`，产物为
`hal.rockchip.rv1126b.so`。

新增芯片时需要：

1. 新建 `socs/<soc>/` 并实现对应 HAL 模块；
2. 在本目录 `CMakeLists.txt` 的支持列表中登记；
3. 只把经过实际复用验证的代码提升到 `common/` 或 `platform/shared/`；
4. 在应用 preset 中设置 `DARKOS_VENDOR` 和 `DARKOS_SOC`。
