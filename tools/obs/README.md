# OBS 自动管理 MediaMTX

用 Windows x64 的 OBS 启动和管理中继。已验证 OBS 33.0.1 与 MediaMTX 1.21.4。MediaMTX 转发 OBS 已编码的音视频。手机可继续使用已安装的 LONG Virtual Camera APK。

## 首次加载

1. 下载本目录的 [longvc-mediamtx.lua](longvc-mediamtx.lua) 和 [mediamtx-obs.yml](mediamtx-obs.yml)，放到同一目录。
2. 在 OBS「工具 → 脚本」点「+」，选择 `longvc-mediamtx.lua`。在脚本里选择已经解压的 `mediamtx.exe`，或把该程序放在脚本旁边。 [OBS 脚本](https://docs.obsproject.com/scripting) · [MediaMTX 安装](https://mediamtx.org/docs/kickoff/install)
3. 保留默认端口 `1935`，点击「启动 / 应用并重启中继」，在「脚本日志」确认“中继已就绪”。如果已有中继占用 `1935`，脚本会等待。停流并退出旧实例后，脚本会接手该端口。也可为脚本选择其他空闲端口。
4. OBS「设置 → 直播」选择「自定义」，服务器使用 `rtmp://127.0.0.1:1935/longvc`，推流码留空，视频选择 H.264。如果已有端口或流路径，可在脚本里填相同值。脚本、OBS 和手机使用同一个端口和流路径。 [OBS 发布设置](https://mediamtx.org/docs/publish/obs-studio)

「随 OBS 自动启动中继」默认开启。OBS 会为当前场景集合加载脚本，并在退出或卸载脚本时关闭它创建的中继。子进程启动成功后异常退出时，脚本会尝试重启。开始和停止推流仍通过 OBS 控制。

## 手机拉流

| 连接 | 操作 | 手机上的拉流地址 |
| --- | --- | --- |
| USB | 手机 IPv4 留空，电脑运行 `adb reverse tcp:1935 tcp:1935` | `rtmp://127.0.0.1:1935/longvc` |
| 同一局域网 | 填入手机的局域网 IPv4，再应用脚本设置 | `rtmp://电脑局域网IP:1935/longvc` |

默认仅监听本机。局域网模式下，只有本机能推流，指定的手机 IPv4 和本机能读取配置的流路径。手机 IP 变化时，更新脚本里的 IP 并应用。Windows 防火墙如需入站规则，可将 TCP 端口限制在专用网络和该手机 IP。RTMP 为明文。

## 排查

- **中继就绪，手机等待帧**。确认 OBS 已开始推流、视频使用 H.264、手机拉流地址和端口正确。
- **端口被占用**。在脚本日志查看端口，检查是否另有 MediaMTX 实例，或为脚本使用其他空闲端口。
- **更换场景集合后停流**。OBS 将脚本保存在场景集合中。需要使用中继的集合应加载此脚本。
