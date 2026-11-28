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

Windows x64 的 OBS 可用 Lua 脚本自动管理本机 MediaMTX。解压官方 MediaMTX 后，在 OBS「工具 → 脚本」加载 `longvc-mediamtx.lua`，选择 `mediamtx.exe`，再点「启动 / 应用中继设置」。勾选「沿用 OBS 本机推流地址」可从已有 OBS 设置读取端口与流路径。脚本配置随 OBS 场景集合保存。 [配置与排查](tools/obs/README.md) · [MediaMTX 下载](https://mediamtx.org/docs/kickoff/install)

1. 在 OBS「设置 → 直播」选择「自定义」，服务器填 `rtmp://127.0.0.1:1935/longvc`，推流码留空。视频使用 H.264，然后开始推流。 [OBS 推流示例](https://mediamtx.org/docs/publish/obs-studio)
2. 用 USB 时，在电脑执行 `adb reverse tcp:1935 tcp:1935`。用局域网时，在脚本勾选「允许局域网拉流」并应用。
3. 在 APK 的「电脑 OBS 拉流」填 `rtmp://127.0.0.1:1935/longvc`（USB）或 `rtmp://<电脑局域网 IP>:1935/longvc`（局域网）。保存后开启相机或照片替换。需要 OBS 音频时，选中 OBS AAC 并打开虚拟麦克风。

也可连接自行管理的 MediaMTX。若该中继启用了读取认证，手机地址可加 `?user=用户名&pass=密码`。RTMP 为明文，适用于可信网络。 [MediaMTX 读取认证](https://mediamtx.org/docs/features/authentication)

低延迟可先用 H.264、30 fps 和 1 秒关键帧间隔。连接稳定时，可用 720p 约 8 Mbps 或 1080p 约 14 Mbps 的 CBR 测试清晰度。OBS 的「视频」中将输出分辨率设为目标尺寸，并检查源素材是否有足够的原始分辨率。使用 x264 时可选 `zerolatency`。使用 NVENC 时可选低延迟调优、关闭 Look-ahead 并设 B 帧为 0。网络持续丢帧时，先降到 720p 并检查 Wi-Fi 或切换 USB。 [OBS 丢帧排查](https://obsproject.com/kb/stream-connection-troubleshooting) [H.264 码率参考](https://support.google.com/youtube/answer/2853702)

本地媒体和 OBS 共用一套 NV21 发布及相机输出管线。支持设备可保留最高 3840 × 2160 或 2160 × 3840 的源画面。预览、录像和拍照依相机实际请求的尺寸生成，并在比例不同时保留比例进行裁切。用 OBS 播放照片时，可用构图脚本把完整内容放入手机可见区域。实际解码分辨率和帧率受设备 MediaCodec、网络和源素材限制。Pixel 的 HDR+ 从替换帧生成的 RAW10 制作照片，成片的清晰度上限取决于源画面在宽高比及数字变焦裁切后保留的细节。 [手机画面构图](tools/obs/README.md#手机画面构图)

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

Windows 开发机可用 `lsposed-module\gradlew.bat` 执行相同的 Gradle 任务。载荷清单会核验 APK 内的文件大小、SHA-256 与 ARM64 ELF 架构。

## 从 GitHub 获取 APK

| Actions 入口 | 何时使用 | APK 和下载位置 |
| --- | --- | --- |
| [01 开发调试版 APK](https://github.com/GUSHU101/LONG-Virtual-Camera/actions/workflows/development-build.yml) | 给手机装测试版。点 **Run workflow**，选择分支，填写版本号 | 从该次运行的 **Summary → Artifacts** 下载压缩包，解压安装 `*-development.apk`。应用 ID 为 `com.gushu.longvc.debug` |
| [02 正式发布版 APK](https://github.com/GUSHU101/LONG-Virtual-Camera/actions/workflows/release-build.yml) | 出正式签名包并公开发布。手动填写新版本号，或推送对应版本标签 | 从 [Releases](https://github.com/GUSHU101/LONG-Virtual-Camera/releases) 下载 `*-release.apk`。应用 ID 为 `com.gushu.longvc` |
| [03 自动质量检查](https://github.com/GUSHU101/LONG-Virtual-Camera/actions/workflows/quality.yml) | 推送 `main` 和提交 PR 时自动测试、编译和运行 Lint | 运行摘要显示检查结果 |

版本号格式为 `vMAJOR.MINOR.PATCH`，例如 `v2.0.43`。两个渠道使用各自的应用 ID 和签名，可分别安装。同一渠道覆盖升级时使用等于或高于已安装版本的版本号，并沿用相同签名。

旧 CI 开发包使用过一次性签名，需要先卸载旧 APK，再安装固定签名版。通过控制面板导入的媒体和来源配置保存在独立的 `/data/cr` 下。首次重装后，在 Root 管理器为新安装的 App 恢复授权，并在 LSPosed 恢复 `system` 作用域。打开控制面板，用“校验并安装载荷”加载新 APK 的系统组件。

### 构建签名设置

仓库维护者在 **Settings → Secrets and variables → Actions** 配置对应渠道的密钥。

- 开发调试版读取 `LONGVC_DEVELOPMENT_KEYSTORE_BASE64` 和 `LONGVC_DEVELOPMENT_KEYSTORE_PASSWORD`。开发密钥别名为 `longvc-development`，证书 SHA-256 必须与 `.github/development-cert.sha256` 一致。
- 正式发布版读取 `LONGVC_ANDROID_KEYSTORE_BASE64`、`LONGVC_ANDROID_KEYSTORE_PASSWORD`、`LONGVC_ANDROID_KEY_ALIAS` 和 `LONGVC_ANDROID_KEY_PASSWORD`。可用 Actions Variable `LONGVC_ANDROID_CERT_SHA256` 锁定正式签名证书。

同一正式版本的发布任务串行执行。构建前会检查版本标签和已有发布，发布前会按构建返回的制品 ID 下载，并再次校验 APK 的 SHA-256。GitHub 草稿里的 APK 与校验文件均匹配后才公开发布。同一次运行上传中断时，可以用 **Re-run failed jobs** 从该运行的草稿继续。已有正式版本请继续使用新版本号。

开发构建和正式发布的运行摘要均提供对应的下载位置、构建提交和 APK SHA-256。自动质量检查会链接到开发版构建入口。

当前测试覆盖桌面端移除、APK 契约、恢复故障注入和原生像素布局。设备上的相机、麦克风以及 OBS 全链路仍需在对应固件实测。

[模块操作说明](docs/LSPOSED-ROOT-MODULE.md) · [架构](docs/ARCHITECTURE.md) · [上游代码审计](docs/UPSTREAM-CODE-AUDIT.md) · [许可证及来源](NOTICE)
