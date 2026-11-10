# ESP32-C3 陪伴小人

一个基于 ESP32-C3 和 1.8 英寸 ST7735 屏幕的桌面陪伴小人。v0.1 提供待机立绘、中文椭圆气泡、HC-SR04 扫过触发和 Q 弹下压动画，可作为生日礼物原型继续扩展。

## v0.1 功能

- 128×160 屏幕，画面旋转 180°；人物相对原稿下移 10 个屏幕像素。
- 11px 中文点阵字；文字留在椭圆气泡内，优先在逗号、句号等标点后换行，长句自动分页。
- HC-SR04 在约 650ms 采样窗口内检测到超过 20cm 的有效距离变化时触发角色动画。
- 角色做两次快速下压与回弹；气泡和文字保持静止。
- 动画目标约 30 帧/秒。

## 硬件

主控：ESP32-C3 Super Mini。屏幕：ST7735，128×160。测距：HC-SR04，ECHO 信号必须是 3.3V 电平。

| 模块 | 接线 | ESP32-C3 引脚 |
|---|---|---:|
| 屏幕 | SCK | GPIO4 |
| 屏幕 | SDA / MOSI | GPIO20 |
| 屏幕 | CS | GPIO10 |
| 屏幕 | DC | GPIO21 |
| 屏幕 | RST | GPIO3 |
| HC-SR04 | TRIG | GPIO7 |
| HC-SR04 | ECHO | GPIO6 |
| 两个模块 | GND | GND |

屏幕 VCC 按模块规格接 3.3V；HC-SR04 按模块规格供电，并确认 ECHO 到 GPIO6 不超过 3.3V。

## 构建

需要 Python 3、Pillow、esptool 和 Zig。默认字体为 macOS 苹方/冬青黑体，可用 `--font` 指定其他字体。

```sh
python3 -m pip install Pillow esptool
python3 tools/build.py --zig /path/to/zig --out /tmp/companion-build
```

输出包括 `companion-v0.1.bin`、`companion.elf`、`idle-preview.png` 和生成的点阵字库。构建所用 ESP32-C3 RISC-V 目标为 freestanding 模式。

## 临时运行

```sh
python3 tools/device.py --port /dev/cu.usbmodem1101 --image /tmp/companion-build/companion-v0.1.bin --demo
```

这会通过 USB 把程序载入 RAM，不写入闪存。保持终端打开：输入一行文字可显示气泡，输入 `/pet` 可播放动画，输入 `/quit` 退出。

## 持久安装

发布目录里的 `companion-v0.1.bin` 是应用镜像，不是引导程序。板上原有引导程序和分区表需要保留；ESP32-C3 Super Mini 的现有应用槽位从 `0x10000` 开始，容量为 `0x140000`。

先备份整片 4MB 闪存，再只写应用槽位：

```sh
python3 -m esptool --chip esp32c3 --port /dev/cu.usbmodem1101 read-flash 0x0 0x400000 original-flash.bin
python3 -m esptool --chip esp32c3 --port /dev/cu.usbmodem1101 write-flash --flash-mode dio --flash-size 4MB --flash-freq 40m 0x10000 releases/v0.1/companion-v0.1.bin
```

如需恢复，使用备份完整写回地址 `0x0`：

```sh
python3 -m esptool --chip esp32c3 --port /dev/cu.usbmodem1101 write-flash 0x0 original-flash.bin
```

开机后 USB 串口会输出 `PET READY v0.1`。本程序直接占用应用槽位，不保留原来的应用；恢复备份可还原原固件。

## 联网版 v0.5（ESP-IDF）

联网版在 [`firmware/idf`](firmware/idf)，是单独的 ESP-IDF 工程（Wi-Fi 放不进 v0.1 裸机镜像的 16KB IRAM）。v0.5 平时是一张时钟 + 天气图标卡片，挥手播放 v0.1 的下压回弹动画并弹出椭圆气泡说一句话，第一次挥手后 1 到 10 秒内再挥一次查余额（不到 1 秒的算同一次）；有陪伴模式和生日模式，`http://pet.local/` 设置页，连不上 Wi-Fi 时自动开临时配网热点。气泡里是「大肥鱼」人设的句子，按当前时段、天气、周末挑选：20 句内置种子 + 句子池，快用完时调一次 `deepseek-flash` 批量补 20 句（完整人设提示词 + 此刻情况，至少隔 12 小时，每天最多 4 次）。天气每 30 分钟刷新，双挥手在气泡里用大号数字显示余额，断电重启后设置和句子池都在。应用镜像在 `releases/v0.5/companion-v0.5.bin`，刷之前先做整片闪存备份，命令、引脚和说明都写在该目录的 README 里。注意 IDF 版本的屏幕 MOSI / DC 引脚是 GPIO9 / GPIO8，和上面 v0.1 的接线表不同。v0.1 的气泡和动画保持原样。

## 下一步：v1.0（心情 / 表情版本）

v1.0 计划在设置页加一个开关，在「表情版」（大肥鱼的表情跟着心情变，所有人物图按表情重画，图存在单独的 assets 数据分区）和现在的「经典版」之间切换。详细目标、资源包格式、心情映射、里程碑、验收和测试计划见 [`docs/v1.0-roadmap.md`](docs/v1.0-roadmap.md)。
