# HyperGlass 1.0.11

HyperOS 系统桌面的 LSPosed 模块，提供单色玻璃图标、文件夹、动态日历和时钟，以及 MAML / Android 小组件样式。包名为 `dev.hyperglass.lsposed`，作用域为 `com.miui.home`。

支持图标内容圆角裁剪、实时日期和指针、小组件内容与交互更新。应用打开和上滑返回使用当前图标边界，后台小组件刷新独立处理材质；GPU 先裁剪目标区域，再导出像素。

## 安装

1. 从本仓库 Release 下载 `HyperGlass-LSPosed-1.0.11.apk` 并覆盖安装。
2. 在支持 HyperOS Runtime 注入的 LSPosed / Vector 中启用模块，作用域选择系统桌面。
3. 打开模块设置页，按需隐藏桌面图标或关闭小组件单色；隐藏后从 LSPosed / Vector 的模块页面重新打开设置页，小组件单色和通知角标白底黑字开关修改后重新加载桌面生效。
   通知角标白底黑字默认关闭，开启后数字角标和圆点使用白底，数字为黑色；关闭后恢复系统配色。
4. 重新加载系统桌面。停用、卸载后重新加载桌面可恢复系统渲染。

框架需要 Modern API 102 与 native API 2。已验证 LSPosed 2.2.1 (7912) + Zygisk Next 加载链。使用旧 HyperGlass root helper 的设备先停用旧 helper。

## 自动适配

运行时从当前 ELF、Dart 方法符号、ARM64 指令的寄存器关系和调用边定位，类编号、字段和栈槽随当前二进制推导。支持在校验通过时适配扩展的栈帧和迁移到同类辅助方法的代码。支持 APK 内直接映射的库，以及原生 Hook 分割后的连续可执行映射。各功能组校验失败时停用对应组并记录原因。

定位规则从参考桌面 801030308（8.01.03.308）生成，版本号、Build ID 和文件哈希不参与运行时启用判断。参考桌面及其地址、字段、类编号变化样本必须全部就绪；旧桌面只报告兼容结果，不阻塞构建。当前规则在 9239、9226 上保留可选矢量图形滤镜以外的全部功能，在 7722 上保留图标、动画和角标。9226 和 8.01.03.308 已在真机与模拟器验证，9239 和 9261 已在模拟器验证。对照 9239，9261 上的矢量小组件、普通及带 tint 的 TextView 复合图标在单色开启和关闭时均通过逐像素对照。新版规则沿当前绘制调用定位滤镜，并复用桌面的物理密度换算保持 Android 小组件布局。

## 源码

| 路径 | 内容 |
| --- | --- |
| `module/native` | 原生加载、语义解析、材质处理、代码页保护与原生测试 |
| `module/src`、`module/res`、`module/META-INF` | LSPosed 注册和 Android 应用 |
| `module/adaptive` | 定位规则生成、变更样本和 ARM64 执行验证 |
| `adapters` | 图标、动画、小组件的 C / ARM64 适配器 |
| `resources/dynamicicons` | 保留日期、时钟与动作的动态图标资源 |
| `fixtures/widgets` | 原始小组件 XML 验证样本 |
| `config` | 公开签名轮换证明、证书指纹与 CI 输入完整性信息 |

仓库保存模拟器视觉验收所需的裁剪基线。调试采样、完整截图、录屏、设备日志、系统 APK/SO 和签名私钥均排除出源码仓库。APK 关闭 `debuggable`，native 库剥离静态符号和调试段，仅导出 `native_init`；保留必要的适配、加载和错误日志。

## 本地构建

需要 macOS、JDK 21、Python 3.12、Android SDK 37、build-tools 36.0.0 和 NDK 29.0.14206865。Linux 构建由 GitHub Actions 覆盖。`ANDROID_HOME` 可指定 SDK，`HYPERGLASS_ANDROID_PLATFORM` 指定 SDK 平台目录名。

先将原始参考文件放到 `fixtures/reference`，或通过 `HYPERGLASS_REFERENCE_DIR` 指定同结构目录：

- `launcher-801030308/MiuiHome.apk`、`launcher-801030308/libapp.so`：参考桌面
- `launcher-801027722/libhyper_os_flutter.so`、`launcher-801027722/librust_maml_sdk.so`：引擎与 MAML 参考库
- 可选兼容样本：`launcher-801029239/MiuiHome.apk`、`launcher-801029226/MiuiHome.apk`、`launcher-801027722/libapp.so`

这些文件单独保存在私有 `build-inputs-v1` Release，供 CI 下载，不进入 Git 历史。

```sh
python3 -m venv .venv
.venv/bin/pip install -r requirements.txt
export JAVA_HOME=$(/usr/libexec/java_home -v 21)
export PATH="$JAVA_HOME/bin:$PATH"
.venv/bin/python build.py --signing-dir "$HOME/Code/rear-card-bridge/signing"
.venv/bin/python module/test_native.py emulator-5580
```

签名目录需要 `release.jks` 和 `keychain.properties`。属性为 `keyAlias`、`keychainService`、`keychainAccount`；本地密码从 macOS Keychain 读取。构建没有 debug key 回退。

产物位于 `dist`。本地保留未剥离库用于定位原生问题；APK 中包含剥离后的库。`config/signing-lineage.bin` 仅含公钥证书与轮换证明，用于已有 0.5.x 安装原地升级。

## CI 发布

推送 `v` 开头的版本标签，或运行 Release 工作流，即可执行离线适配器检查、参考桌面的 native 解析器及其重定位后 ARM64 代码测试、旧桌面兼容报告、签名与 APK 清理检查，然后发布正式 Release。

工作流使用与本地相同的 release key。私钥和密码保存在仓库 Actions Secrets：`RELEASE_JKS_BASE64`、`RELEASE_STORE_PASSWORD`、`RELEASE_KEY_PASSWORD`、`RELEASE_KEY_ALIAS`。构建强制核对公开证书指纹，缺失配置直接失败。

CI 不连接真机。Android 原生并发、页保护、GPU 所有权与设备视觉验证通过 `module/test_native.py` 及经授权的设备操作完成。

## 许可与参考

第三方许可随 APK 打包，源文件保存在 `module/third_party`。

- [MiuiBackGestureHook](https://github.com/wxxsfxyzm/MiuiBackGestureHook)：HYOS 加载链和 native hook 研究。
- [HyperOS4SwipeGate](https://github.com/PzHown/HyperOS4SwipeGate)：ARM64 定位与代码页保护；改编部分使用 MIT 许可。
- [XZ Embedded](https://github.com/tukaani-project/xz-embedded)：GNU debugdata 解压，0BSD 许可。
- Android NDK：静态 C++ 运行库相关 NOTICE 随包分发。
