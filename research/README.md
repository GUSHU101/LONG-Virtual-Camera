# 隔离的早期实验

legacy-prototypes 中是本项目早期自写但未经设备验证的实现，不是 Camera2Magic 源码。
文件使用 .java.disabled 后缀并位于 Android source set 之外，绝不进入 APK。

保留用于回溯审查；不能直接改后缀启用。已知问题包括非同线程播放器操作、合成帧冒充所选视频、Camera1 参数与回调契约不完整、Camera2 全局输出映射、缺少会话 generation 和失败恢复。
后续后端应基于经过测试的媒体核心重写，参考 docs/ARCHITECTURE.md。
