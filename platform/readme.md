# hardware 层

仿 Android HAL（纯 C）的硬件抽象层：**接口定义与厂商实现彻底分离**，
上层通过字符串 id 在运行时加载厂商实现，编译期不依赖任何厂商代码。

## 目录结构

rockchip: 瑞芯微
novatek: 联咏
artosyn: 库芯微