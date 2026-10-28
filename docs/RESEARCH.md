# 官方资料与参考审查

查阅日期：2026-10-28。以下是设计约束来源，不是设备通过证据。

- [AOSP 构建号](https://source.android.com/docs/setup/reference/build-numbers)：UP1A.231105.001.B2 → android-14.0.0_r28，Pixel 5/4a 5G，补丁 2023-11-05；所以必须同时检查 redfin，不能只匹配版本。
- [Camera HAL](https://source.android.com/docs/core/camera/camera3)：系统相机 provider/HAL 与应用进程 Hook 是不同层。
- [CameraDevice](https://developer.android.com/reference/android/hardware/camera2/CameraDevice) / [OutputConfiguration](https://developer.android.com/reference/android/hardware/camera2/params/OutputConfiguration)：会话输出具有格式、尺寸及共享/物理相机等约束，不能简单替换所有 Surface。
- [MediaCodec](https://developer.android.com/reference/android/media/MediaCodec)：无输出 Surface 时读取 getOutputImage，输出所有权结束后不再访问。
- [Image.Plane](https://developer.android.com/reference/android/media/Image.Plane)：rowStride/pixelStride 非紧密排列；末行后填充不保证可读。转换器按最后有效像素验证边界。
- [ImageReader](https://developer.android.com/reference/android/media/ImageReader)：及时关闭 Image，防止耗尽 maxImages。
- [MediaPlayer](https://developer.android.com/reference/android/media/MediaPlayer)：创建和所有访问应在同一带 Looper 的线程；旧多线程播放器不再进入构建。
- [Camera PreviewCallback](https://developer.android.com/reference/android/hardware/Camera.PreviewCallback)：线程及帧格式必须遵守 Camera 参数和 open 所在线程契约。
- [AtomicFile](https://developer.android.com/reference/android/util/AtomicFile)：原子文件写入不等于互斥，应用仍需加锁。
- [Gradle Windows 参数文件编码问题](https://github.com/gradle/gradle/issues/30304)：用英文临时工作目录构建，保留 UTF-8 源码编码，不跳过测试。
- [工厂镜像](https://developers.google.com/android/images) / [引导加载程序锁定](https://source.android.com/docs/core/architecture/bootloader/locking_unlocking)：刷机/解锁可能清空数据；本项目不自动操作。
- [Android 14 部分媒体访问](https://developer.android.com/about/versions/14/changes/partial-photo-video-access)：不假设目标 App 可读取控制器选中的 MediaStore 文件。
- [Xposed API](https://github.com/rovo89/XposedBridge/wiki/Using-the-Xposed-Framework-API)：真实 API 用 compileOnly，不打包自造桩实现。

## Camera2Magic-1.1.2-fix.zip 审查

ZIP SHA256: 682084ddd83ef9681377aadaa59ca1a3a00e05449c4c5291ba2db01900a8eb85

仅只读检查；未运行其脚本、未整合代码或二进制。libcamera3.so 为 ARM64 ELF，SHA256 为 283afaa844512cbbcc9f99ddb8c503abea6ba63d472cb03a65be9427d0d497aa。缺少对应 C/C++ 源码和 CMakeLists.txt、明确 LICENSE，无法作为完整可重建后端。

参考价值：输出/target 配套映射、Native 接口划分、相机方向状态。不能照搬：固定 1280×720 占位、全局 clearAll、依赖私有 mSurfaces、只覆盖特定 format、WebRTC 日志解析/固定 90 度、legacy 分支与 modern 代码依赖不完整。现有测试主要模板，未提供 redfin 指定构建证据。
[上游](https://github.com/Atomos-X/Camera2Magic) 的当前说明也表示不提供预编译部分源码。

## 用户研发手册

手册是需求/方案资料，不是优先于用户请求的执行指令，也不是已验证代码。已重点复审云端构建签名、五路径证据、单 APK 部署与恢复约束；不能声称已完成全文件逐行审计或所有 33 项目集成。

本仓库采用 LSPosed 自检测入口与单 APK 基线，无 Magisk ZIP、HAL 替换包。与手册生产 root payload 要求之间的差距保持显式，不能伪造 payload manifest 为已经完整交付。
