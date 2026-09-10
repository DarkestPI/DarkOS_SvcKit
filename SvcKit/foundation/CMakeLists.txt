# 基础设施骨架。加入源码后可将 INTERFACE 改为 STATIC/SHARED，目标名保持不变。
add_library(svckit_base INTERFACE)
add_library(DarkOS::Base ALIAS svckit_base)
