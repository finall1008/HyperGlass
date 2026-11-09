# HyperGlass 1.0.3

使用 finall release 证书签名，支持覆盖升级。

- 为 Android RemoteViews 小组件的图片、复合图标、矢量与形状背景（`GradientDrawable`、`ColorDrawable`、`VectorDrawableBox`）及 MAML 画布接入黑白灰度颜色矩阵滤镜。
- 为富文本（`SpannedString`、`ForegroundColorSpan`）、提示文本（`textColorHint`）和 MAML 动态文本颜色及显式滤镜补齐单色白前景归一化，保留原有透明度。
- 在无填充描边图形上保留透明内部，维持小组件毛玻璃背景、圆角裁剪与实时更新行为。
- 增加多版本重定位与 ARM64 执行回归验证。

作用域为系统桌面，需要支持 HyperOS Runtime 的 LSPosed / Vector、Modern API 102 和 native API 2。覆盖安装后重新加载系统桌面生效。
