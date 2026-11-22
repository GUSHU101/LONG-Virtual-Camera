# LONG Virtual Camera

LONG Virtual Camera 是一个面向自有或已授权 Root Android 测试设备的虚拟摄像头工具。
它不是给两个旧项目套一层说明，而是把
[ReCam](https://github.com/aimardcr/ReCam) 与
[CamReplace](https://github.com/pIat0n/CamReplace) 的完整源码纳入同一仓库，在共同的安全、
同步和生命周期约束下维护为两个可切换运行引擎。

> 仅用于你拥有或明确获准测试的设备。不要用于身份验证、金融、门禁或欺骗他人的场景。
> 本工具需要 Root，并向 Android 相机/音频系统服务加载原生代码。

## 两个融合引擎

| 模式 | 平台 | 视频 | 虚拟麦克风 | JPEG 照片 | 适用场景 |
|---|---|---:|---:|---:|---|
| 稳定引擎（ReCam-derived） | Windows / Linux / macOS | 是 | 否 | 否 | 跨平台、较保守的相机替换 |
| 增强引擎（CamReplace-derived） | Windows x64 | 是 | 可选，默认关闭 | 是 | OBS 视频、可选音频和静态照片 |

统一入口：

```sh
python long_virtual_camera.py                 # 自动选择
python long_virtual_camera.py --engine stable
python long_virtual_camera.py --engine enhanced
python long_virtual_camera.py --check
```

Windows 上如果已经构建增强引擎，`auto` 优先使用它；否则回退到稳定引擎。也可以通过
`LONGVC_ENHANCED_EXE` 指定增强引擎可执行文件。

## 麦克风行为（重要）

虚拟麦克风是独立的显式开关，默认 **OFF**：

- 未点击 **Enable virtual microphone** 时，不启动 PCM feeder，不向 `audioserver` 注入
  音频 hook，应用继续直接使用手机自带麦克风；
- 开启后才把 OBS/AAC 解码后的 PCM 送入虚拟麦克风；
- 开启状态发生断流、格式尚未识别或数据不足时，整段输出静音，绝不把真实麦克风尾部
  混入虚拟音频；
- 点击 **Disable virtual microphone** 后停止 PCM feeder、清除共享音频 magic；如果 hook
  曾经加载，它会直接透传手机麦克风，重启音频服务后彻底卸载。

摄像头替换和麦克风替换互相独立，可以只替换视频而保留手机麦克风。

## 已融合的稳定性改进

- 两套帧环都具有 reader pin / writer claim，生产者不会覆盖正在读取的帧；
- 相机 hook 等待 HAL 输入 fence，优先使用官方
  `ANativeWindowBuffer_getHardwareBuffer` 转换，并同步完成 CPU unlock；
- 增强引擎的 JPEG 编码在完整生命周期内固定源帧；
- 不再执行 `setenforce 0`，运行文件按 `cameraserver` / `audioserver` 分离所有权并使用
  `0600` 权限；
- 注入器只接受受支持的 ABI / 返回陷阱，未知签名失败关闭；
- RTMP 和 ADB 媒体通道仅绑定 loopback；日志隐藏 URL 凭据、token、headers 和 cookies；
- 本地媒体握手使用双方临时 X25519 密钥派生的票据证明，拒绝旧版全零票据、篡改和重放；
- Start/Stop 作为完整事务串行执行，启动失败回滚手机与主机的所有已完成阶段；
- 部署缓存按手机序列号隔离，运行中的 OBS/ADB 会话绑定其所属设备，防止切换手机后串线；
- 异步 UI 使用原子状态和操作代数，过期回调不能覆盖用户刚执行的新操作；
- Stop、启动失败回滚与关闭窗口共享完整清理路径；
- 增强引擎的视频、音频和照片通道各自启停，不再把音频隐式绑定到相机开关。

技术依据、修复清单和仍然存在的边界见
[融合研究记录](docs/FUSION-RESEARCH.md) 与 [架构约束](docs/ARCHITECTURE.md)。

## 构建稳定引擎

要求 Python 3.13、Android NDK `27.3.13750724`、ADB 和 FFmpeg：

```sh
ndk-build NDK_PROJECT_PATH=android NDK_APPLICATION_MK=android/jni/Application.mk -j8
python -m pip install -r desktop/requirements.txt
python long_virtual_camera.py --engine stable
```

稳定引擎支持文件循环、自定义 FFmpeg 输入、桌面摄像头、OBS Virtual Camera 和本机
RTMP。内部设备程序仍名为 `recam` / `librecam.so`，以保持上游部署协议兼容。

## 构建增强引擎（Windows x64）

要求 CMake 3.24+、Ninja、Android SDK Platform Tools、Android NDK
`29.0.14033849` 和 LLVM-MinGW x86_64 MSVCRT：

```bat
cd engines\camreplace
build.bat
cd ..\..
python long_virtual_camera.py --engine enhanced
```

输出为：

```text
engines\camreplace\build\host-dev-dev\bin\LONG-Virtual-Camera-Enhanced.exe
```

增强引擎目前只构建 `arm64-v8a` Android 组件。摄像头、麦克风和照片替换都由独立按钮
控制；麦克风不会随“Start replace camera”自动开启。

## 测试

```sh
python -m compileall -q desktop tests long_virtual_camera.py
python -m unittest discover -s tests -p "test_*.py" -v
```

GitHub Actions 另外会：

- 编译并运行 ReCam 与 CamReplace 两种共享内存 ring 的并发不变量测试；
- 编译并运行增强媒体通道的握手、票据篡改和重放拒绝测试；
- 用固定 NDK 构建稳定引擎的 ARM64/x86_64 Android 组件；
- 用固定 NDK 构建增强引擎的 ARM64 Android 注入器、feeder、相机与音频 hook。

## 目录

| 路径 | 内容 |
|---|---|
| `long_virtual_camera.py` | 统一引擎选择与启动入口 |
| `android/`, `desktop/` | ReCam-derived 稳定引擎 |
| `engines/camreplace/` | CamReplace-derived 增强引擎完整源码 |
| `tests/` | 生命周期、脱敏、ring、融合安全约束与启动器测试 |
| `docs/` | 融合研究和跨引擎安全契约 |

## 来源与许可证

- ReCam 基线：`44f99681079c04e3b3e6d3a74fef96f03b480348`（MIT）；
- CamReplace 基线：`8456a5d8041f4c81a26b5c19fa0d6409f57db048`（GPL-3.0-only）。

由于仓库包含并修改了 CamReplace 的 GPL-3.0 源码，融合项目整体按
[GNU GPL v3.0](LICENSE) 发布。ReCam 的 MIT 许可和归属保留在
[LICENSES/ReCam-MIT.txt](LICENSES/ReCam-MIT.txt)，CamReplace 原始许可证保留在
[LICENSES/CamReplace-GPL-3.0.txt](LICENSES/CamReplace-GPL-3.0.txt)。第三方归属见
[NOTICE](NOTICE) 以及各引擎内随附的许可证文件。
