# HyperGlass 1.0.4

使用 finall release 证书签名，支持覆盖升级。

- 模块设置页增加桌面图标开关，隐藏后应用从桌面和应用列表移除，设置页仍可通过 LSPosed / Vector 的模块页面打开。
- 桌面图标入口改用独立 `activity-alias`，开关只启停该别名，不影响模块注册、作用域配置和已保存状态。

作用域为系统桌面，需要支持 HyperOS Runtime 的 LSPosed / Vector、Modern API 102 和 native API 2。覆盖安装后重新加载系统桌面生效。
