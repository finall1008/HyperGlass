# HyperGlass 1.0.5

使用 finall release 证书签名，支持覆盖升级。

- MAML 小组件玻璃改用 `Glass_Bionic_Medium_Normal`，饱和度与文件夹、Android 小组件的原生玻璃一致。
- MAML 中带 `srcType` 的提供方图片（相册照片、封面）保持显示，按自身透明度的 0.6 叠加在玻璃上；铺满卡片的包内底图仍作为背景隐藏。
- Android 小组件中由位图和 Uri 加载的图片以 0.6 透明度叠加在玻璃上，资源图标保持原有透明度。
- 设置页增加小组件单色开关，默认开启；关闭后图标、图片和照片保留原色，文字仍为白色，修改后重新加载桌面生效。

作用域为系统桌面，需要支持 HyperOS Runtime 的 LSPosed / Vector、Modern API 102 和 native API 2。覆盖安装后重新加载系统桌面生效。
