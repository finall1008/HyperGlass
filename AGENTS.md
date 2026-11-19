# HyperGlass

正式模块源码。`build.py` 构建 release APK，`module/test_host.py` 执行 CI 解析器与 ARM64 测试，`module/test_native.py` 执行 Android 原生测试。

- 保留当前图标、动态图标、小组件和动画行为；修复共同原因，禁止按 App 名称、内容或桌面坐标做渲染特判。
- 适配采用当前 ELF、符号、ARM64 语义和调用关系。禁止增加版本、Build ID、文件哈希允许列表。CI 样本完整性与签名证书校验独立于运行时适配。
- 样式、快照、Android 容器、MAML 分组校验；ABI 或定位歧义停用对应组并给出原因。
- 保留后台小组件独立阶段、前景实时更新、GPU 裁剪和材质存储隔离。
- 快照对应的 Dart / native 标记与 ABI 共同启用。批量写入暂停其他线程，回读验证，刷新指令缓存并恢复页面权限。
- 加载器回调禁止等待后台运行时锁；允许原生 Hook 导致的连续可执行映射分段。
- native hook 通过框架 API，登记拥有的代码页，保护 HyperRT 回收路径。
- 正式 APK 仅导出 `native_init`，禁止加入像素、纹理采样或逐帧日志；保留必要错误与适配信息。
- 所有构建使用既定 release key。私钥、密码和参考系统 APK/SO 不入 Git；本地通过 Keychain，CI 通过 Actions Secrets。签名缺失直接失败，禁止生成或回退 debug key。
- `config/signing-lineage.bin` 为公开证书轮换证明，保留已有安装的覆盖升级能力。
- 原生代码修改运行离线、host 与 Android 适用测试。CI 通过后再发布相同提交的 APK。
- 新桌面 APK 按 [桌面适配验证](docs/launcher-adaptation.md) 完成离线解析、模拟器日志和视觉对照。
- 真机安装、输入和截图须有用户授权；使用 root ADB，禁止 CUA、录屏和整机重启。保留真机壁纸、应用数据及其他模块配置。需要整机重启时交给用户操作。
- Git 提交身份：`finall1008 <wymxx55@outlook.com>`。
