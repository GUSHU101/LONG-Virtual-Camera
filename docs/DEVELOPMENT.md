# 开发与 GitHub 构建

## 工具链与产物

唯一安装包模块：`app-control`。Java 17 语言级，minSdk 33、targetSdk 34、compileSdk 35；首要验证设备限定 Pixel 5 的指定 Android 14 构建，不宣称支持所有满足 minSdk 的设备。

工作流 `.github/workflows/android.yml` 在 push、PR、手动触发运行 Debug 构建。权限仅 contents:read；Actions 固定完整提交 SHA；无 PR secrets、无 pull_request_target、无远程安装脚本。

SDK 通过固定提交的 android-actions/setup-android 安装，不假设 runner 预装 sdkmanager；命令行工具版本固定为 15859902。SDK 安装步骤按常规 Android 构建流程接受其许可证。首次云端运行的 sdkmanager PATH 失败已通过显式安装修正。

Debug 步骤：校验 wrapper → 安装固定 SDK/Build Tools → JUnit → Lint → assemble → 校验唯一 APK、包名、目标配置、DEX 中真实入口定义、Xposed API 未打包 → apksigner → SHA256 → Artifact。Artifact 保留 14 天，报告独立上传。云端通过不代表真机通过。

## Release 签名

只有默认分支手动运行并勾选 `signed_release` 才执行 Release。它仍然是基线功能，不会自动打开替换后端。

在 GitHub 的 `release` Environment 设置以下 Secrets（私钥由用户生成、备份并持有；不要发到聊天或提交 Git）：

- LONG_RELEASE_KEYSTORE_B64
- LONG_RELEASE_STORE_PASSWORD
- LONG_RELEASE_KEY_ALIAS
- LONG_RELEASE_KEY_PASSWORD

签名文件仅在 runner 临时目录解码，finally 删除；缺少任意 secret 则失败，不降级为 Debug 密钥。不会自动创建公开 Release，也不会把私钥打包进 Artifact。Environment 保护/审批需要根据账户套餐设置，不能把仅声明 environment 当作已启用审批。

固定 applicationId + 相同签名密钥 + 递增 versionCode 才能稳定升级。当前 release applicationId 为 `io.github.longcontroller`，debug 为其 `.debug` 后缀，二者可并存。

## 本地质量门槛

使用 README 中的 Build-Local.ps1。中文 Windows 路径下 Gradle 8.13 worker 参数文件存在编码问题，不能用关闭 Lint 或忽略测试失败规避；脚本将明确白名单源码复制到唯一 ASCII 临时目录，执行完整校验，再回传产物。

若要清理构建目录，仅清理脚本输出的具体临时目录，先确认路径；不要对工作区根目录执行递归删除。

## 证据分层

- JVM_TEST_PASS：仅证明纯 Java 模型和布局算法，不证明 Android 硬解。
- ANDROID_BUILD_PASS：成功构建并验证 APK 完整性，不证明安装启动。
- LOCAL_VIDEO_FRAMES_OBSERVED：设备本地解码并转换出帧，不证明跨应用输入。
- NATIVE_FRAMES_OBSERVED：原生相机取得帧，不证明虚拟输入。
- CROSS_APP_PATH_PASS：必须绑定设备完整指纹、模块版本、消费者包签名/版本、路径/格式/尺寸、帧来源与关闭恢复结果。

`LongSafetyGate` 是离线决策模型；当前没有可用后端可由它开启，不把它描述为已完整接入的运行期保护机制。

## 当前限制

- MediaCodec 同步调用可能被厂商实现阻塞；10 秒为循环检查时限，不保证驱动级硬超时。
- 导入只做首帧验证，损坏的后续帧须由解码诊断发现；大文件读取可能等待内容提供者，取消是协作式。
- 视频帧诊断尚不做实时播放节奏、旋转、镜像、音频或任意 Surface 输出；CRC32 不是安全摘要或来源认证。
- 进程在提交中断时可能留下孤立媒体文件；“删除已导入媒体”清理本应用已知命名的副本/临时文件。不会扫描外部存储。
- 原生诊断不持久化“恢复成功”，须重复打开和系统相机复核。
- UI 当前以中文为主，Lint 国际化提示保留；没有用基线文件隐藏错误。
