# HyperGlass 1.0.2

使用 finall release 证书签名，支持覆盖升级。

- 完善 LSPosed Modern API 102 集成：声明 `exceptionMode=protective`，开启 `extractNativeLibs="false"` 16KB 页对齐零拷贝映射，支持从管理器设置入口直接打开模块说明页，统一由 `native_init.list` 承载原生加载。
- 识别 `hyos_spawner` 孵化期进程并在非目标进程中拒绝注册加载器回调；新增对 `libhyper_surface_context.so` 的加载回调触发，全部特性组就绪后退出后台观察线程。
- 为 `madvise` 代码页保护槽位、动态图标资源 Hook 及材质/背景流 Hook 组增加原子回滚事务。
- 增强无调试符号场景下的 ARM64 指令邻域与分配器 tag 退避扫描，并在私有跳板页未使用时自动回收虚拟内存映射。

作用域为系统桌面，需要支持 HyperOS Runtime 的 LSPosed / Vector、Modern API 102 和 native API 2。覆盖安装后重新加载系统桌面生效。
