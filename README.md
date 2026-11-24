# LONG Virtual Camera

LONG Virtual Camera 是面向自有或明确授权测试设备的 Android ARM64 LSPosed 模块。安装一个 APK 即可管理本地视频、图片、可选本地音频和 OBS。日常操作在手机控制面板完成。Windows 主程序、Python UI 和桌面打包入口已从当前源码树移除，历史版本仍可在 Git 中查阅。

## 手机端使用

1. 在支持 libxposed API 102 系统作用域的 LSPosed 中启用模块，只勾选 `system`，按管理器提示重启。
2. 打开控制面板，授权 Root，校验并安装 ARM64 载荷。
3. 选择本地视频、图片或 OBS，按需开启全局摄像头或照片替换。
4. 只有要替换麦克风时才选择音频并打开虚拟麦克风。开关关闭时使用手机自带麦克风。选择本地音频不会切换画面来源。
5. 使用“停止全部并恢复系统服务”结束。界面会显示恢复是否通过验证。

`system` 作用域用于激活模块。相机和音频替换由原生系统服务路径处理，覆盖范围取决于固件及实际使用的 CameraService、gralloc 和 Audio HAL 路径。

## OBS 与画面分辨率

默认情况下手机只监听本机回环。在电脑端使用 OBS 时，可通过 USB 执行 `adb forward tcp:1935 tcp:1935`。在 OBS 自定义直播服务中填入 `rtmp://127.0.0.1:1935/live`，使用手机面板新生成的推流密钥。视频编码使用 H.264。需要虚拟麦克风时使用 AAC。

可信局域网连接需在手机面板明确开启，再使用面板显示的局域网地址。RTMP 不加密。密钥仅在生成后的面板会话显示，普通状态输出不包含密钥。切换共享 OBS 会话的视频或音频轨道时，OBS 可能需要重连。

本地视频和 OBS 共用一套 NV21 发布及相机输出管线。ReCam 的按比例适配方式用于中心裁剪，预览目标按实际请求尺寸生成。输入和输出比例不同时会裁掉部分边缘画面以保持比例。共享帧有 1920 × 1080 的像素预算，也允许 1080 × 1920 的竖屏方向。实际解码分辨率受设备 MediaCodec 限制。

## 从源码构建

使用 JDK 21、CMake 3.22 或更新版本、Android SDK `platforms;android-37.0`、Build Tools 37.0.0 和 CI 固定的 Android NDK 27.3.13750724。设置 `ANDROID_NDK_ROOT` 指向 NDK 后，从仓库根目录执行。

```sh
cmake -S engines/camreplace/android -B build/camreplace-android \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/engines/camreplace/toolchains/android-ndk.cmake" \
  -DCR_FEED_ENABLE_RTMP_DIRECT=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build/camreplace-android --parallel
./lsposed-module/gradlew -p lsposed-module :app:assembleDebug :app:lintDebug
python tests/verify_apk.py lsposed-module/app/build/outputs/apk/debug/app-debug.apk
```

Windows 开发机可用 `lsposed-module\gradlew.bat` 执行相同 Gradle 任务，输出仍然是 Android APK。正式发布要求配置项目的 Android 签名密钥。CI 的开发包使用独立的应用 ID。载荷清单会核验 APK 内的文件大小、SHA-256 与 ARM64 ELF 架构。

在 GitHub Actions 手动运行 **Release APK** 时，保持 `publish` 关闭即可生成可安装的开发版 APK 和 SHA-256 校验文件，从该次运行的 Artifacts 下载。开发版使用 CI 调试签名，已有不同签名的同包名 APK 无法覆盖安装。

勾选 `publish` 或推送版本标签会走正式发布。提前在仓库的 **Settings → Secrets and variables → Actions** 配置 `LONGVC_ANDROID_KEYSTORE_BASE64`、`LONGVC_ANDROID_KEYSTORE_PASSWORD`、`LONGVC_ANDROID_KEY_ALIAS`、`LONGVC_ANDROID_KEY_PASSWORD`。正式版更新必须继续使用同一签名密钥。

当前测试覆盖桌面端移除、APK 契约、恢复故障注入和原生像素布局。设备上的相机、麦克风以及 OBS 全链路仍需在对应固件实测。

[模块操作说明](docs/LSPOSED-ROOT-MODULE.md) · [架构](docs/ARCHITECTURE.md) · [上游代码审计](docs/UPSTREAM-CODE-AUDIT.md) · [许可证及来源](NOTICE)
