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

## 后续规划

联网、天气、喝水/久坐提醒和热点设置页在 [`firmware/idf`](firmware/idf)。那是单独的 ESP-IDF 工程，不塞进 v0.1 的裸机镜像：Wi-Fi 放不进 16KB IRAM。v0.1 的气泡和动画保持原样。刷 IDF 版本前先做整片闪存备份，说明写在该目录的 README 里。
