# 桌面适配验证

在 `emulator-5580` 验证新桌面。模拟器须已启用 HyperGlass，模块作用域为 `com.miui.home`，小组件单色和通知角标单色均开启。系统 APK、原始截图、录屏和完整设备日志保存在本地忽略目录。

## 离线解析

1. 把新 APK 放到 `module/.build/host-tests/candidate.apk`。按 README 使用正式签名构建模块，生成 `module/.build/adaptive_profiles.hpp` 和适配器。
2. 在 Linux ARM64 环境运行 `timeout 180 python3 module/test_host.py`，准备 C/C++ 编译器与 `requirements.txt` 中的 Python 依赖。macOS 可以在 Colima 或其中的容器运行该命令。在同一环境执行以下命令，直接解析 APK 内的 ELF。

   ```sh
   timeout 30 env HG_EXPECT_READY=247 module/.build/host-tests/test_resolver 0 \
     'module/.build/host-tests/candidate.apk!/lib/arm64-v8a/libapp.so' \
     module/.build/host-tests/candidate.json
   ```

3. 核对 `ready=247`、`diagnostic` 和 `UNRESOLVED`。用同一解析器跑一遍已验收的 APK，逐条核对新增未解析项。富文本备用路径、已被新绘制路径替代的 vector/compound 定位会出现在部分旧样本的输出里。若当前输入另外包含 Flutter 引擎或 MAML 库，分别用索引 `1` 和 `2` 检查，期望功能值为 `2` 和 `8`。

## 模拟器验收

`validation/visual/scenes.json` 描述一套固定桌面布局。测试使用 1120×2436 主屏、480 dpi、手势导航、抽屉模式和固定四色测试壁纸。第一页右下放置「设置」，右上「系统工具」文件夹里使用「时钟」，左侧堆叠小组件的当前卡片为 2×2「天气预报」。在抽屉「全部」页面使用「计算器」。第二页中部保留未登录的星穹铁道 Android 小组件。第四页右侧放置独立的 2×2「天气预报」MAML 卡片，使卡片横跨壁纸的红绿边界。像素基线使用北京的夜间图案，前景天气文字在静态裁剪中遮蔽。先打开天气以处理首次使用提示，保证随后点击卡片能直达天气页面。

新增场景时在配置中声明桌面页、进入路径 `enter`、目标包名、点击位置和裁剪范围。可以用 `before_enter` 记录进入前的状态，例如关闭的文件夹。坐标仅用于模拟器测试。

安装候选桌面和正式签名模块后，重新加载主屏桌面。

```sh
timeout 30 adb -s emulator-5580 push module/.build/host-tests/candidate.apk /data/local/tmp/hyperglass-candidate.apk
timeout 60 adb -s emulator-5580 shell "su -c 'pm install -r -d /data/local/tmp/hyperglass-candidate.apk'"
timeout 15 adb -s emulator-5580 shell rm -f /data/local/tmp/hyperglass-candidate.apk
timeout 60 adb -s emulator-5580 install -r dist/HyperGlass-LSPosed-1.0.8.apk
timeout 15 adb -s emulator-5580 shell "su -c 'am force-stop com.miui.home'"
timeout 15 adb -s emulator-5580 shell \
  "su -c 'am start --display 0 -a android.intent.action.MAIN -c android.intent.category.HOME -n com.miui.home/.launcher.Launcher'"
timeout 360 .venv/bin/python -I validation/visual/capture.py --serial emulator-5580 \
  --output module/.build/visual-candidate
timeout 240 .venv/bin/python module/test_native.py emulator-5580
```

视觉脚本需要 FFmpeg、NumPy 和 Pillow。它等待主屏桌面，并从当前桌面 PID 的日志检查三种库的解析值、`APPLIED features=255` 及材质和背景流就绪标记。它比较桌面、文件夹和抽屉三条应用打开与上滑返回路径，以及 Android 小组件、天气单色图案和天气卡片转场。脚本核对主屏前台应用和桌面页，遇到弹窗或错页时报告实际状态。像素对照保存裁剪图和差异图。脚本按录制中应用画面所占比例和局部前景限定转场阶段，再选取最接近基线构图的帧。每个方向最多录制五次，重试时错开手势与录制的间隔。`report.json` 保存所选帧、比例、RGB 平均绝对误差和差异像素占比。需要回看原始帧时传入 `--keep-recordings`，该目录继续留在本地。脚本完成后恢复三个动画缩放设置。

`maml-weather-stack-blur` 临时使用条纹壁纸，检验第一页堆叠天气卡片打开初期和返回晚期的背景模糊。此场景使用与构建相同的 JDK 21 和 Android SDK 编译临时壁纸设置工具。主屏和锁屏须共用未裁剪的 1120×2436 静态原图。脚本备份原图，并在场景结束时恢复并核对壁纸文件摘要。

两个天气场景均在采集前先打开天气并返回一次，让卡片的实时材质完成更新，再截取稳态画面。

`large-folder-clock` 在第五页左侧另设一个 2×2 大文件夹，和第四页的天气卡片在同一行。将时钟放在文件夹左上第一枚可直接启动的预览图标位置。第一页系统工具继续保留原有时钟。场景从桌面预览直接点击时钟，再上滑返回同一页。

回放根据图标区以外的桌面变化筛选动画落地阶段的原始帧。表盘圆形裁剪中的亮度中位数与同次落地截图比较，亮度分布的两端与中位数之差用于检查亮刻度、暗表盘的极性。报告保存逐帧亮度、极性及偏差最大的一帧。该检查直接比较同一次采集中的画面，无需新增像素基线。

抽屉打开的基线姿态很短。重试会同时错开录制起点和按压时长，帮助捕获这一帧。

模糊检查从卡片中部避开文字的区域取列亮度中位数，比较条纹的二次谐波与基频振幅之比。强模糊会优先压低较细条纹的振幅。脚本按应用画面占比选帧，并用裁剪区色度排除已被天气应用覆盖的帧。报告记录同次稳态截图的比值、通过筛选的原始帧比值和最大相对偏差。打开最多允许 40% 偏差，返回最多允许 22%。输出目录保存偏差最大的一帧及对应的稳态裁剪图。该检查直接比较同一次采集中的画面，无需新增像素基线。

### 从模拟器中断继续

若 QEMU 退出，脚本报告 `device lost`，并在 `report.json` 保留已完成的场景及 `next_scene`。按原配置在后台重新启动 `Xiaomi_18_Pro`，沿用 5580 端口、GPU 模式和用户数据。用限时命令等待 Android 启动完成，并解锁模拟器主屏。

```sh
timeout 150 sh -c '
  until [ "$(timeout 5 adb -s emulator-5580 shell getprop sys.boot_completed 2>/dev/null | tr -d "\r")" = 1 ]; do
    sleep 2
  done
'
```

在原录制或回放命令上添加 `--resume`，保留相同的 `--output`、`--baseline`、`--profile` 和其他参数。脚本核对桌面版本及已安装的 HyperGlass APK 摘要，等待桌面及 HyperGlass 就绪后，从中断处重做该场景，沿用已完成的结果。若就绪日志已被日志缓冲区淘汰，使用上面的两条 `am` 命令重载桌面。动画缩放与壁纸恢复到各自备份的状态。录制的基线在整套场景完成并通过后才写入指定目录。

## 根据差异继续定位

- 若功能组未就绪，从日志中的首个失败定位开始。对照参考与当前 ELF 的符号、指令操作数和调用边。快照需要核对圆整分支、视图 ID 到原生场景的传递、await 写回槽和捕获范围的返回 PC。定位或 ABI 有歧义时停用相应组并保留原因。
- 若就绪但视觉失败，先用差异图和录制确认相同布局和转场阶段，再沿受影响的图标、Android 容器或 MAML 路径找共同原因。将变化补入 host 的可迁移样本或失败样本，验证失败只影响相应功能组。
- 重新生成适配器、构建、运行 host 测试并在模拟器重测。候选 APK 和一个已验收桌面都通过后，才接受适配结果。

[视觉基线](../validation/visual/baseline/)保存了 308 验收时的裁剪图。经人工检查确认视觉预期有意改变时，用 `--record --baseline module/.build/new-visual-baseline` 写入新目录，再核对并替换相应裁剪图。若采集流程改变了可重复捕获的转场阶段，只替换受影响的姿态，并用原像素阈值独立回放。一次正常适配应与现有基线比较。

复验旧桌面时，可以用原先验收过的模块在同一桌面版本上录制独立对照，再给候选模块的对照命令传入该目录的 `--baseline`。回放会核对基线 `capture.json` 的桌面版本，避免把两个桌面版本的转场轨迹变化混入回归结果。
