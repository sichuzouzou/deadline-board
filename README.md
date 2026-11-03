# deadline-board 桌面墨水屏待办提示板

放办公桌的 4.2 寸墨水屏待办板：显示 3 条待办的截止日与剩余天数，USB 有线同步，断电靠 DS3231 + 纽扣电池保持时间。

## 硬件

| 件 | 型号/参数 |
|---|---|
| 主控 | ESP32-C3，Flash 4MB（**必须 DIO 模式**，QIO 会 bootloop） |
| 屏 | 4.2 寸 400×300 黑白（面板实际由 SSD1619A 三色控制器驱动，**不支持局刷**，全刷约 4.2s） |
| 时钟 | DS3231，SDA=GPIO1 / SCL=GPIO8，地址 0x68 |
| 按键 | GPIO4 / 5 / 13 |

## 编译烧录

```
arduino-cli compile --fqbn "esp32:esp32:esp32c3:CDCOnBoot=cdc,FlashMode=dio,FlashFreq=80,FlashSize=4M,PartitionScheme=no_ota" .
arduino-cli upload  -p COM4 --fqbn "…" .
```

依赖：U8g2、GxEPD2（驱动为库内 GxEPD2_420c_Z96 的本地改版）。
数字字体 DSEG7 Classic Bold（SIL OFL 1.1，授权见 DSEG-LICENSE.txt）。

## 串口协议（115200，打开串口前置低 DTR/RTS，否则 C3 复位）

```
PING                          → OK
TIME|2026-11-05 09:00:00      → OK TIME（六字段全验，缺一拒绝）
ITEM|n|标题|2026-11-05        → OK ITEM n（标题≤10字，含|拒绝）
CLR|n / CLEARALL / SHOW       → OK …
STATUS / RTC / DUMP           → 状态回读
```

## 版本记录

- **v3.11**（2026-11-03）：①「还要吗?」待确认改为只盘逾期条目，未到期永远显示天数；②内置 2027 年法定节假日表，工作日计算跳过假日；③TIME 命令六字段全验。
- v3.10：STATUS 加 up=（开机秒数）。v3.9：上午/下午心跳 + 串口链路加固。v3.8：区分「RTC 能读」与「时间可信」，不可信时屏显「未对时·插电脑」。v3.7：DSEG7 Bold 字模全量上屏。
