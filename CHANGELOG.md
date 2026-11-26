# HyperGlass 1.0.13

使用 finall release 证书签名，支持覆盖升级。

- 修复部分机型（如 REDMI K Pad）从应用返回桌面的动画中图标失去玻璃材质的问题。这些机型系统的 Flutter 引擎在 `impeller::Context` 虚表中多一个虚函数，资源分配器与命令缓冲的槽位各后移 8 字节，原先按参考值校验后整组停用快照材质。现在由解析器从当前 ELF 的调用点解码这两个槽位交给 GPU 背景拷贝使用；指令形态不符时仍停用该组并记录原因。

覆盖安装模块后重新加载系统桌面。作用域为系统桌面，需要支持 HyperOS Runtime 的 LSPosed / Vector、Modern API 102 和 native API 2。

# HyperGlass 1.0.12

使用 finall release 证书签名，支持覆盖升级。

- 修复应用返回桌面的动画尚未结束时点开另一个应用，前一个应用的图标在新应用打开期间消失的问题。图标捕获边界的 GlobalKey 只保留在桌面上的图标；Hero 飞行副本和 MultiFlyUnderlay 内容自行构建边界，不再从仍挂载的图标取走该边界，也不覆盖其注册。两处守卫按当前 ELF 的方法符号与指令定位 `Element._retakeInactiveElement` 和 `Element.mount`，无法定位时仅记录原因并保持原有行为。
- 新增快照导出的异常路径日志：返回快照未固定到桌面稳态材质，或导出时未在目标位置绘制玻璃底板时记录一行，便于定位返回动画中偶发的非玻璃底板。

覆盖安装模块后重新加载系统桌面。作用域为系统桌面，需要支持 HyperOS Runtime 的 LSPosed / Vector、Modern API 102 和 native API 2。
