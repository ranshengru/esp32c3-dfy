# 联网桌宠 v0.4（ESP-IDF）

在 v0.1 立绘上面叠四行字。这一版有家里的 Wi-Fi、北京或东京时间、Open-Meteo 天气、DeepSeek 余额、喝水和久坐提醒。设置页通过桌宠连接的家用路由器访问，不广播设备热点；USB 串口命令也保留。气泡和下压动画仍留在 v0.1，没有搬过来。

扫一下说一句大肥鱼的话，或者关掉当前提醒。连续两次扫（大约 2.5 秒内）才查余额，15 秒内不重复打接口。本地语录 64 条。余额可用时，大约每 6 小时向 `deepseek-chat` 要一句新的；句子必须是 4 到 7 个字，而且每个字都在小字库里，否则丢掉，屏幕不会出现方块。

## 不要直接刷

`idf.py flash` 会重写引导程序和分区表，不只是 `0x10000` 上的应用。先备份整片 4MB：

```sh
python3 -m esptool --chip esp32c3 --port /dev/cu.usbmodem1101 read-flash 0x0 0x400000 original-flash.bin
```

分区：nvs `0x9000`，phy `0xf000`，应用 `0x10000` 起、1.5MB。确认备份能写回之后再刷。密钥只进 NVS，不会打进固件，也不会在日志里打印。

## 电脑网页设置

设备只运行 Wi-Fi 客户端模式，不开自己的热点。连上家里的 Wi-Fi 后，在 USB 串口输入 `STATUS` 查看设备 IP；让电脑也连到同一个路由器（有线或 Wi-Fi），浏览器打开 `http://设备IP/`。网页设置页只在这个局域网可访问，不需要让电脑直连桌宠热点。首次设置 Wi-Fi 时，或桌宠连不上路由器时，可改用 USB 串口命令配置。

网页和串口都能改：家里的 Wi-Fi、DeepSeek 密钥、城市、时区（北京 / 东京）、喝水间隔、久坐间隔、勿扰时段。网页不会回显已保存的 Wi-Fi 密码或 API 密钥；输入框留空表示不修改。喝水或久坐填 `0` 就是关掉。勿扰开始和结束填同一个小时，表示没有勿扰。默认 45 分钟喝水、60 分钟久坐、23 点到 8 点不提醒。扫掉「该喝水了」记一次今天喝水，过了午夜清零。提醒在屏幕上停 10 分钟，没人理就当这次过了。

天气用 Open-Meteo，不需要密钥。城市留空就不查。查到经纬度会存下来，大约 30 分钟更新一次。

## 构建

需要 ESP-IDF 5.4.x（本仓库按 5.4.2 编过）。在 macOS 上：

```sh
. $HOME/esp/esp-idf/export.sh
cd firmware/idf
idf.py set-target esp32c3 build
idf.py -p /dev/cu.usbmodem1101 flash monitor
```

退出监视器是 `Ctrl-]`。

字形采用 [Noto Sans CJK SC Regular](https://github.com/notofonts/noto-cjk) 的 16×16 常规字重。字体按 SIL Open Font License 1.1 提供，许可文本在 [`assets/fonts/OFL.txt`](../../assets/fonts/OFL.txt)。重做字形和立绘字节（需 Pillow）：

```sh
python3 tools/gen_idf_assets.py
```

不带参数时，脚本会找文泉驿正黑或冬青黑体。生成物是 `firmware/idf/main/glyphs.h` 和 `idle.rgb565`。语录在 `firmware/idf/main/quotes.h`，改完要重新生成字库。

## 串口

通过 USB 数据线连接电脑，在串口终端（115200 波特率）中输入命令。也可运行 `idf.py -p /dev/cu.usbmodem1101 monitor`；打开监视器可能让开发板重启一次。上电打印 `PET READY v0.4` 和命令提示。一行一条命令：

| 命令 | 作用 |
|---|---|
| `SSID 名字` | 保存家里的网络并重连。生僻字在屏幕上会变成问号 |
| `PASS 密码` | 至少 8 位。单独一行 `PASS` 表示开放网络 |
| `KEY sk-...` | 保存密钥并立刻查一次余额 |
| `CITY 北京` | 天气城市。单独一行 `CITY` 表示清空 |
| `DRINK 45` | 喝水间隔，分钟，0 关闭 |
| `MOVE 60` | 久坐间隔，分钟，0 关闭 |
| `QUIET 23 8` | 勿扰从 23 点到 8 点 |
| `TZ 8` 或 `TZ 9` | 北京时间或东京时间 |
| `FETCH` | 手动查余额，间隔 3 秒 |
| `STATUS` | 连接、余额、天气、语录、堆。不打印密钥 |
| `SCAN` | 扫描 ESP32‑C3 可见的 2.4 GHz Wi-Fi，显示信道、信号强度和加密方式 |
| `FORGET` | 只清家里的网络和密钥，城市和提醒间隔还在 |

时区默认北京时间。证书校验要先完成 SNTP，没校时之前不发 HTTPS。余额接口是 `GET https://api.deepseek.com/user/balance`，屏幕优先显示人民币总额。
