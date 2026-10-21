# 可选的 macOS VP8 播放

English: [MOVIE.md](MOVIE.md)

`MOVIE_FFMPEG=1` 将 Mac 宿主仅按时长空播的 `movie::` 后端替换为 FFmpeg VP8 解码。缺省构建和 iOS 仍使用原后端。仓库不含游戏内容或 FFmpeg 二进制。

适配层实现现有的提取器、解码器虚表：客体提供随机读取器，拥有输入输出事件，按 PTS 调度帧，并处理跳过与循环。输出为紧密排列的 NV12。音频和字幕保留原有客体路径，FFmpeg 不播放音频。提取器和解码器共享读取器；取得的帧在释放前保持有效，输出有八个槽位，满时暂停输入。

## 构建

安装 `pkg-config` 与提供 `libavformat`、`libavcodec`、`libavutil`、`libswscale` 的 FFmpeg 开发库。合成测试还需要带 `libvpx`、`libopus` 和 lavfi 的 `ffmpeg` 命令。开发验证使用 FFmpeg 9.0.2，库版本依次为 63.1.102 / 63.1.102 / 61.1.102 / 10.1.102。

用正常流程准备自己的游戏文件后，切换后端时强制重新构建：

```sh
make -C dm/host -B dmhost MOVIE_FFMPEG=1
```

不传该选项或使用 `MOVIE_FFMPEG=0` 即使用原后端。`dmhost_m3` 也支持该选项，但安装的 FFmpeg 库可能要求比该目标更新的 macOS，应单独检查依赖的最低系统版本。本改动不把库打包进 app，也不改变 iOS 工程。

## 合成测试，无需游戏文件

```sh
make -C dm/host/tests movie-real PY=python3
make -C dm/host/tests movie-real PY=python3 B=/tmp/dm-movie-sanitized \
  MOVIE_TEST_FLAGS='-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer'
make -C dm/host/tests build/t_movie
./dm/host/tests/build/t_movie
```

测试在临时目录生成两个各含 60 帧的 VP8 WebM，其中一个带 Opus 音轨，并将所有解码后的 NV12 帧与 FFmpeg 的原始输出逐字节对照。还检查元数据、单调 PTS、流尾、定位和重启、循环定位后的空包、输出容量、已取得帧的所有权、背压、停止/跳过和共享读取器的释放。测试结束后删除合成数据。客体事件由测试回调表示；不执行游戏程序，也不验证 Metal 呈现、扬声器输出或真机行为。

不改变重写点、启动跳转、触控布局、产品版本或视频选择界面。只有当前客体构建已经能够到达的视频才使用该后端；恢复启动片头属于独立改动，本次不宣称实现。

API 依据为已安装 FFmpeg 头文件中的自定义 `AVIOContext` 所有权规则及解码发送/接收状态机。官方参考：[自定义 IO 示例](https://ffmpeg.org/doxygen/trunk/avio_read_callback_8c-example.html)、[解码 API](https://ffmpeg.org/doxygen/trunk/group__lavc__encdec.html)。在线文档会更新，应以安装版本的头文件为准。
