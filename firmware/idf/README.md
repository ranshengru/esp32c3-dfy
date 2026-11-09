# 联网与余额（ESP-IDF）

这一版只做 Wi-Fi、校时和 DeepSeek 余额。气泡、随机台词、喝水提醒还没做。屏幕上方最多四行状态字，下面仍是 v0.1 的待机立绘。字库只收了界面里实际会出现的字，大约几 KB，放在闪存里，不再占用那 170KB 的全字库 RAM。

扫过 HC-SR04（和 v0.1 同一套距离变化判断）会刷新余额，15 秒内不重复请求。串口 `FETCH` 的间隔是 3 秒。

## 不要直接刷

`idf.py flash` 会重写引导程序和分区表，不只是 `0x10000` 上的应用。先备份整片 4MB：

```sh
python3 -m esptool --chip esp32c3 --port /dev/cu.usbmodem1101 read-flash 0x0 0x400000 original-flash.bin
```

分区：nvs `0x9000`，phy `0xf000`，应用 `0x10000` 起、1.5MB。确认备份能写回之后再刷。密钥只进 NVS，不会打进固件，也不会在日志里打印。

## 构建

需要 ESP-IDF 5.4.x（本仓库按 5.4.2 编过）。在 macOS 上：

```sh
. $HOME/esp/esp-idf/export.sh
cd firmware/idf
idf.py set-target esp32c3 build
idf.py -p /dev/cu.usbmodem1101 flash monitor
```

退出监视器是 `Ctrl-]`。

换字体重做点阵和立绘字节（需 Pillow）：

```sh
python3 tools/gen_idf_assets.py --font "/System/Library/Fonts/Hiragino Sans GB.ttc" --font-index 2
```

不带参数时，脚本会找文泉驿正黑或冬青黑体。生成物是 `firmware/idf/main/glyphs.h` 和 `idle.rgb565`。

## 串口

上电打印 `PET READY net`。一行一条命令：

| 命令 | 作用 |
|---|---|
| `SSID 名字` | 保存网络名并重连。屏幕字库不含名字里的生僻字时会显示成问号，串口 `STATUS` 仍打印原文 |
| `PASS 密码` | 至少 8 位。`PASS` 单独一行表示开放网络 |
| `KEY sk-...` | 保存密钥并立刻查一次余额。日志只写 `key saved` |
| `FETCH` | 手动查询 |
| `STATUS` | 打印连接、IP、余额、赠送额、充值额和剩余堆。不打印密钥 |
| `FORGET` | 清掉 NVS 里的网络和密钥 |

时区固定为北京时间。TLS 证书校验要先完成 SNTP，没校时之前屏幕显示「等待校时」，不会发请求。

余额接口是 `GET https://api.deepseek.com/user/balance`。屏幕只显示 `total_balance` 和币种，优先人民币。`STATUS` 另附赠送额和充值额。401 显示「密钥无效」，其它状态码显示 `API n`。
