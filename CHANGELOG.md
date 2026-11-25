# HyperGlass 1.0.12

使用 finall release 证书签名，支持覆盖升级。

- 修复应用返回桌面的动画尚未结束时点开另一个应用，前一个应用的图标在新应用打开期间消失的问题。图标捕获边界的 GlobalKey 只保留在桌面上的图标；Hero 飞行副本和 MultiFlyUnderlay 内容自行构建边界，不再从仍挂载的图标取走该边界，也不覆盖其注册。两处守卫按当前 ELF 的方法符号与指令定位 `Element._retakeInactiveElement` 和 `Element.mount`，无法定位时仅记录原因并保持原有行为。
- 新增快照导出的异常路径日志：返回快照未固定到桌面稳态材质，或导出时未在目标位置绘制玻璃底板时记录一行，便于定位返回动画中偶发的非玻璃底板。

覆盖安装模块后重新加载系统桌面。作用域为系统桌面，需要支持 HyperOS Runtime 的 LSPosed / Vector、Modern API 102 和 native API 2。
