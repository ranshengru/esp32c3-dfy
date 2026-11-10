# ESP32-C3 联网桌宠 v1.0

在 v0.5 上加了表情。默认还是经典立绘。

- 八个表情循环：平常、开心、困、打盹、傲娇、惊讶、担心、兴奋。打盹才戴眼罩。每张脸 4 帧，大约 0.45 秒换一帧。
- 每帧是整屏 RGB565 的 zlib 无损压缩，大约 13–16 KB。32 帧共约 456 KB，在 `assets` 分区，不在应用里。
- 设置页「形象」或串口 `EXPR 0`（经典）/ `EXPR 1`（表情）。`EXPR SHOW happy` 强制 30 秒，`EXPR CYCLE` 轮播。
- 空闲表情看时间、气温、天气，随机后保持 60 秒。挥手的句子、提醒、余额会换脸。
- 字库只留简体 GB2312 一级。以前额外加的繁体人名用字去掉了，那些字显示成「?」。
- 句子池升到版本 3，生成句带心情。旧池开机迁移，不丢句子。

- 头顶以上的空白行不再放进 JPEG。平时高 112，Z 或跳起来会到 128 或 144，贴在屏幕底部。白块几乎不占字节，整包从约 845KB 到约 825KB。
- 上次成功的天气、气温和余额写在 NVS。重启先显示这份；天气请求失败时保留上次的，不改成「天气失败」。
- 旧的表情包（zlib 或原始 RGB565）不能用了，要刷这份新的 `assets-v1.0.bin`。

开机串口：`PET READY v1.0`。

用 ESP-IDF 5.4.2 编译。应用镜像 `companion-v1.0.bin` 写到 `0x10000`（1,434,144 字节，1.5 MB 槽位还剩约 9%）。分区表变了，NVS 仍在 `0x9000`。不要整片擦除。

```
python -m esptool --chip esp32c3 -b 460800 --before default_reset --after hard_reset write_flash --flash_mode dio --flash_size 4MB --flash_freq 40m 0x0 bootloader.bin 0x8000 partition-table.bin 0x10000 companion-v1.0.bin 0x190000 assets-v1.0.bin
```

已经刷过 v0.5、引导程序没变的话，可以不刷 `0x0` 的 bootloader。三个必刷的是分区表、应用、表情包。
