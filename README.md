# LONG Virtual Camera / LONG 虚拟摄像头

首要目标：**Google Pixel 5 / redfin / Android 14（API 34）/ UP1A.231105.001.B2**。

> 当前交付是可构建的基线控制器与自有媒体帧验证通道，**不是已经完成的跨应用虚拟相机**。
> Camera1、Camera2、CameraX、NDK、WebRTC 虚拟输入均为 NOT_RUN。无 Root 也可运行本地功能；不会自动 Root、解锁或刷机。

## 已实现

- 系统文件选择器导入到应用私有目录；256 MiB 上限，H.264 视频轨/首帧验证，最大 1080p 像素面积。
- 版本化媒体提交；导入失败保留旧媒体，成功后移除旧副本；可显式删除应用内媒体。
- 前台静音视频预览；退到后台停止播放。
- 自有 MediaCodec 解码诊断：最多 90 帧 / 循环检查 10 秒超时，YUV_420_888 → NV21，记录帧数、PTS 和末帧 CRC32。
- YUV 转换处理行跨度、像素跨度、偶数裁剪区域、非零 Buffer position 和末行无填充；转换前校验容量。
- 原生 Camera2 按实际枚举 ID、受支持尺寸采集 3 秒，统计帧/时间戳；不保存画面、不录音。
- Pixel 5 目标属性匹配、诊断文本分享。属性匹配不是真实性认证或兼容证明。
- GitHub Actions 单测、Lint、APK 构建、实际 DEX 入口检查、签名校验与 SHA-256 产物。
- Provider 不对外导出；模块只检测控制器内 LSPosed 是否生效，不安装任何第三方应用相机 Hook。

此前不安全的实验 Hook 保留在 `research/legacy-prototypes/*.disabled`，不参与构建。没有引入 Camera2Magic 的源码或闭源原生库。

## 获取 APK

仓库：[GUSHU101/LONG-Virtual-Camera](https://github.com/GUSHU101/LONG-Virtual-Camera)

进入 Actions → **Pixel 5 baseline APK** → 成功运行 → 下载 `LONG-redfin-baseline-debug`，解压后安装 `LONG-Controller.apk`。这是 Debug 签名测试包，非正式发布包。

**不同机器/不同云端运行生成的 Debug 证书可能不同。** 签名冲突时不能直接覆盖安装；卸载会删除应用数据。需要稳定升级时，配置自己保管的 Release 签名密钥，不要随意卸载包含重要媒体的安装。

## 本地构建

JDK 17 或 21、SDK Platform 35、Build Tools 36.0.0。AGP 8.13.2 / Gradle 8.13 固定版本，Gradle wrapper 与分发包有 SHA-256 校验。

Linux / macOS：

```sh
sh gradlew --no-daemon :app-control:testDebugUnitTest :app-control:lintDebug :app-control:assembleDebug
pwsh -File scripts/ci/Package-Apk.ps1 -Variant debug
```

Windows PowerShell 7（支持当前中文工作目录，通过唯一英文临时目录构建）：

```powershell
$env:JAVA_HOME = 'C:\Program Files\Android\openjdk\jdk-21.0.8'
$env:ANDROID_HOME = "$env:LOCALAPPDATA\Android\Sdk"
.\scripts\Build-Local.ps1
```

输出：`dist/debug/LONG-Controller.apk`、`SHA256SUMS.txt`、`BUILD-EVIDENCE.txt` 和报告目录。临时构建目录保留用于排错，不会自动删除用户目录。

## 下一步

1. 在指定 Pixel 5 上安装、验证媒体预览/解码、前后摄原生采集与重复打开。
2. 根据设备实测数据完成媒体帧节奏、旋转/镜像与独立消费者测试。
3. 用户自行准备可恢复的 Root/LSPosed 测试环境后，开发按设备/会话隔离的跨应用后端。
4. 五条采集路径逐项验收，不从“预览成功”推导“拍照/录像/浏览器成功”。

详见 [开发与云构建](docs/DEVELOPMENT.md)、[Pixel 5 验收](docs/PIXEL5-VALIDATION.md)、[架构及路线](docs/ARCHITECTURE.md)、[官方资料与审查](docs/RESEARCH.md)。
