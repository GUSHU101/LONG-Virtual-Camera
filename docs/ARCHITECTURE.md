# 架构复审与实施路线

## 当前运行边界

单 APK：控制 UI → 私有媒体存储 → 本地预览 / 视频帧探针；原生 Camera2 探针独立 Activity。ContentProvider exported=false，仅应用自己读取固定媒体 URI。Xposed 入口只检测控制器进程模块状态，不进入目标 App。

本次优先建立可靠基线，**不等于放弃跨应用目标**。旧 Hook 代码存在格式、线程、全局映射与错误恢复缺陷，隔离到 research/legacy-prototypes，不把它们当作可交付实现。

## 自有媒体核心

MediaExtractor 只选 H.264 视频轨 → MediaCodec 无 Surface 输出 → Image 三平面 → stride/crop 校验 → NV21。解码和释放在同一 worker 线程；逐个释放输出 Image/codec buffer；仅复用一个 NV21 数组，不累计队列。所有平面在任何目标写入前完成校验。无第三方原生二进制依赖。

转换器目前拒绝奇数尺寸或奇数 crop origin。保留原始 YUV 色彩样本，不进行色域、旋转、镜像或宽高适配；这些必须成为显式处理阶段，不能靠修改宽高元数据假装完成。

## 后续里程碑与退出条件

### M0 — 可复现基线（当前）

构建、JVM 单测、Lint、签名和实际 DEX 检查通过；云端实际运行成功。Pixel 5 真机测试仍需执行。

### M1 — 视频帧管线设备验收

指定 Pixel 5 验证 H.264 解码、stride/crop、PTS；补帧节奏/循环/停止状态机及镜像旋转。屏幕显示与字节输出来自同一源时间轴。独立自有消费者检查帧标记，内存和线程数有界。

### M2 — Camera1 实验后端

先限定 NV21、单相机、受支持尺寸。回调遵循 Camera.open 所在线程；缓冲池大小/所有权明确；one-shot 与 buffered callback 分开；JPEG 与 RAW 不混用。会话绑定媒体 generation，撤销停止并要求重开，不承诺半配置会话透明回退。

### M3 — Camera2 实验后端

每设备/每会话独立映射；请求 target 和 session outputs 一致；保留 OutputConfiguration 的 physical ID、共享、动态范围等语义。不原地改写调用者私有字段，不固定占位尺寸，按实际能力选择。
Preview、YUV、JPEG、录像逐一实现；不支持的组合在建立会话前拒绝实验，保留原生。处理异步配置失败、late callback、session close 和多摄竞争。

### M4 — 独立消费者矩阵

CameraX、NDK、WebRTC 各有实际消费者证据，不能从 Camera2 Hook 存在推导兼容。普通 APK 不注册系统 Camera ID；HAL/AOSP 方案是单独的大范围项目，不能靠 APK 签名解决。

## 安全与运维

用户选择普通媒体测试场景；不用于冒充真实采集来源的认证流程。默认不开替换、不自动 Root、不传视频、不申请麦克风/联网/全盘存储权限。未来外部 Provider 必须增加 UID、包签名、versionCode、设备/配置 generation 绑定和共享 UID 拒绝策略，不能直接把 exported 改为 true。
