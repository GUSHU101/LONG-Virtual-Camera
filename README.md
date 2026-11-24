# LONG Virtual Camera

LONG Virtual Camera 是把 [ReCam](https://github.com/aimardcr/ReCam) 与
[CamReplace](https://github.com/pIat0n/CamReplace) 融合成的一套 Windows 原生工具，面向
自有或明确授权的 Root Android 测试设备。它不是两个项目的启动菜单，也没有“稳定/增强”
引擎选择：构建只产生一个 `LONG-Virtual-Camera.exe`，所有输入和功能共享同一设备会话、
RTMP 解析器、H.264 通道、帧环和生命周期控制器。

> 仅用于你拥有或获准测试的设备。不要用于身份验证、金融、门禁或欺骗他人的场景。
> 工具会向 Android 相机/音频系统服务加载原生代码；未知固件 ABI 会失败关闭。

## 真正融合后的运行路径

桌面输入可以选择 OBS、测试图、文件/URL 或 Windows 摄像头。ReCam 的输入能力已作为
`DesktopSource` 适配器进入 CamReplace-derived 原生主机，而不是保留成第二套 Python
运行时。所有适配器最终进入同一条发布路径：

```text
OBS / 测试图 / 文件或 URL / Windows 摄像头
                       │
              本机 RTMP（仅 127.0.0.1）
                       │
        H.264 + 可选 AAC、统一预览和状态
                       │
       认证的 ADB 本地通道（一个设备会话）
                       │
        Android feeder → 共享帧环 → camera hook
                       └──────────→ 可选 audio hook
```

“摄像头替换”“照片替换”“虚拟麦克风”是同一会话上的三个可组合功能开关，不是三个引擎。
摄像头与照片共享视频 lease；麦克风有独立 lease，三者可以同时工作。

## 麦克风隐私语义

虚拟麦克风默认 **OFF**，且只能由用户显式开启：

- 初始 OFF 时不启动 PCM feeder，也不注入音频 hook，手机原生麦克风直接工作；
- 摄像头和照片操作绝不会隐式开启麦克风；
- ON 后，只有认证通道 READY 才替换音频；虚拟数据不足或格式异常时输出静音，绝不混回
  真实麦克风尾部；
- UI 区分 STARTING、READY、STALLED、UNSUPPORTED；
- Disable 会先使手机端音频通道失效，再停止主机 pump；若本次进程中曾经启用过，已加载的
  dormant hook 可能保留到服务/会话清理，但它只调用原始 HAL 路径、不再替换任何字节，
  因而下一次回调仍使用手机原生麦克风。

## 当前能力

- 一套 Windows x64 原生 UI 和可执行文件；
- OBS、测试图、文件/URL、DirectShow 摄像头输入；
- 单一发布级 H.264 视频通道、实时预览与 JPEG 静态照片替换；
- 默认关闭、独立启停的虚拟麦克风；
- 精确 Android 私有 ABI 候选、`status_t` 原值返回、未知符号拒绝 hook；
- reader pin / writer claim 三槽帧环、重连 writer lease、事务化发布；
- 有界 RTMP/AVCC/AMF 解析、1/2/4 字节 AVCC 长度字段；
- 每消费者连续音频重采样相位；
- 进程拥有的串行操作队列、设备会话绑定和阶段回滚；
- 临时 X25519 会话、票据证明、记录认证及重放/大小限制；
- Release 构建拒绝缺失或零字节资源，并生成 SHA-256 构建清单。

上游保留的 raw NV21、direct RTMP 和 threaded feeder 源码仅用于兼容性/回归编译，不在
产品 UI 中形成第二个模式。

## 构建与启动

当前完整产品只支持 Windows 10/11 x64。构建需要 CMake 3.24+、Ninja、Android SDK
Platform Tools、Android NDK `29.0.14033849` 以及 LLVM-MinGW x86_64。使用测试图、文件/
URL 或 DirectShow 输入时，运行环境还必须能找到 `ffmpeg.exe`；可放入 `PATH`，或用
`LONGVC_FFMPEG` 指向绝对路径。OBS 直接推流模式不依赖桌面适配器启动 FFmpeg。设置自定义
构建路径时可使用 `ANDROID_SDK_ROOT`、`ANDROID_NDK_ROOT`、`LLVM_MINGW_ROOT` 和
`MINGW_RUNTIME_DIR`。

```bat
build.bat
```

输出为：

```text
engines\camreplace\build\host-dev-dev\bin\LONG-Virtual-Camera.exe
```

也可用统一启动器检查/启动该文件：

```text
python long_virtual_camera.py --check
python long_virtual_camera.py
```

非 Windows 平台目前会明确报“不支持”，不会静默切换到另一套功能不完整的引擎。

## 验证状态

```text
python -m compileall -q desktop tests long_virtual_camera.py
python -m unittest discover -s tests -p "test_*.py" -v
```

CI 还会实际编译并运行帧环、安全通道、hook ABI、帧布局、AVCC 和一小时音频相位测试，
构建 Android ARM64 载荷与 Windows 主机。CI 通过只能证明代码级和构建级门禁；不同 Pixel/
Android/供应商 gralloc 与 Audio HAL 仍必须在实体设备上执行矩阵验证，仓库不会把“能编译”
表述成“所有手机稳定可用”。

## 审计、架构与来源

- [上游全量代码审计](docs/UPSTREAM-CODE-AUDIT.md)
- [融合研究记录](docs/FUSION-RESEARCH.md)
- [架构和安全契约](docs/ARCHITECTURE.md)

审计基线：ReCam `44f99681079c04e3b3e6d3a74fef96f03b480348`（MIT），CamReplace
`8456a5d8041f4c81a26b5c19fa0d6409f57db048`（GPL-3.0-only）。组合项目按
[GPL-3.0](LICENSE) 发布；原始许可与第三方归属保存在 `LICENSES/`、[NOTICE](NOTICE) 和
`engines/camreplace/THIRD_PARTY_NOTICES.txt`。
