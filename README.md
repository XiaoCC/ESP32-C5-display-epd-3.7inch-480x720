# HueLeaf Base 社区固件 · ESP32‑C5 六色墨水屏

[English](README.en.md) · [发行说明](RELEASE.md) · [第三方声明](NOTICE.md)

将鱼鹰光电（OSPTEK）的彩叶 HueLeaf Base 改成可离线使用的桌面信息屏。板子用原有的 3.7 英寸、480×720 六色 EPD037A02，手机或电脑可通过设备自带的网页后台管理。平时 Wi‑Fi 关闭，屏幕保留上次画面。需要传图、修改待办或更新资料时，长按 BOOT 开启临时 Wi‑Fi。

这是社区二次开发版本，基于 OSPTEK 的 `examples/3.7einkDvb`，上游提交为 `a34b55b168e0e6e12fc6215c6706e7a850e2d6a4`。源码使用 ESP‑IDF **v5.5.4**。本发布包把原开发版的小字点阵换成 Noto Sans CJK SC Bold，固件和随包源码来自同一次构建。

- [原厂 HueLeaf Base 项目、原理图及出厂固件](https://gitee.com/osptek/devkit-hueleaf_base)
- [EPD037A02 屏幕资料](https://gitee.com/osptek/display-epd-3.7inch-480x720/tree/main/versions/EPD037A02)
- [ESP‑IDF v5.5.4](https://github.com/espressif/esp-idf/tree/v5.5.4)

## 适配范围

| 项目 | 要求 |
| --- | --- |
| 开发板 | OSPTEK 彩叶 HueLeaf Base，ESP32‑C5，4 MB Flash |
| 屏幕 | EPD037A02，3.7 英寸，480×720，黑、白、红、黄、蓝、绿 |
| 板载器件 | PCF8563 RTC、AXP2101 电源管理、microSD/TF 卡槽 |
| 刷机 | USB‑C 数据线、Windows 或 macOS、Python 3 和 esptool |
| SD 卡 | 已备份的 FAT32 卡。行程、天气缓存、待办、相册、日签和历史资料依赖 SD 卡 |
| 网络 | 手机或电脑可连接设备的 5 GHz 热点。下载天气和历史资料时，设备还需连上可以访问相应数据源的路由器 |

本包使用原厂 4 MB 分区布局。准备刷写其他屏幕、其他板卡版本或不同 Flash 容量的设备时，请先核查硬件和分区表。

## 功能

| 页面 | 可以做什么 |
| --- | --- |
| 文字 | 从网页编辑、预览并发送文字画面。可在刷新时使用该页单独设置的黑白或彩色模式。 |
| 照片 | 浏览器端裁剪、旋转、调整颜色和做六色抖动。支持单张直接上屏、批量保存原图到 SD、从缩略图选择已存照片上屏，以及多选删除。相册轮播可更换照片。SD 原图与用于墨水屏显示的转换帧分开保存。 |
| 桌面日历 | 显示本月日历、当前日期、星期和设备时间。北京时间由联网校时和板载 RTC 支持。 |
| 天气时钟 | 默认扬州。显示实况、天气数据时间和从当天起七天的预报。设备连上路由器后通常每天自动尝试更新一次；可以手动刷新，也可以把自动更新设为只用缓存。断网时保留最近一次有效结果。 |
| 日历日签 | 大日期、星期、农历、节气、每日宜忌与带出处的摘句。保留原句库，可添加、下载 TSV 模板、批量导入或指定一句上屏。首次初始化写入 2026、2027 全年资料，后台可重新生成一整年的资料。 |
| 待办事项 | 最多 5 项。可编辑、勾选、删除，选填截止时间，选择标准、大字或特大字，并在网页实时预览后保存并显示。 |
| 我的行程 | 最多 12 条高铁／火车或航班记录。可手动填写车次或航班号、站点或机场、出发和抵达的北京时间、座位、闸口和接驳备注。支持逐条上屏或自动选择最近行程。 |
| 历史上的今天 | 读取中文维基百科同月同日的事件，联网时每个自然日尝试更新一次，缓存从当天起七天。首次初始化带有 11 月 22 日至 28 日的七条可离线浏览的种子资料。 |

### 行程和目的地天气

1. 打开「行程 → 添加行程」，选票种并填写信息。跨天行程单独填写抵达日期。
2. 在「目的地天气地点」输入前缀。例如输入「扬」，从候选里选择扬州市、邗江区或广陵区。设备内置约 2,800 条中国地区索引，连着设备热点也能查询本地索引。需要时也可以填经纬度。
3. 点击「保存到 SD 卡」。设备能访问 Open‑Meteo 时，会缓存从当天起七天范围内的抵达日预报。稍后点「重新读取」，查看「已缓存」或「待缓存」。
4. 点击某条记录的「显示这条」，或选择「自动显示最近行程」。自动模式在出发当天优先选择当天尚未结束的行程。若已停留在行程页，跨日后会重绘。已经缓存的目的地天气可离线显示。
5. 可以手动删除行程。设备会在抵达日期过去七天后自动从行程列表移除该记录。行程信息由用户录入，登机口、检票口和时刻以实际承运方通知为准。

### 轮播、配色和按键

在「设置 → 自动轮播」勾选页面，并设置分钟间隔。网页顶部的「立即切换并刷新」可以切换页面。屏幕正在刷新时也可以提交切换，队列里只保留最近一次选择。文字和照片需要先发送或保存可显示的内容。

在「设置 → 页面显示模式」可为各页独立选黑白或彩色。配置自动保存，下次刷屏生效。黑白配色与彩色使用相同的整屏刷新流程，通常需要约 30 秒。时间显示在页面重绘时更新。本版包含的显示能力为整屏刷新，局部快刷、每分钟时钟局刷和微信小程序蓝牙传输仍待实现。

| 操作 | 行为 |
| --- | --- |
| BOOT 单击 | 切到下一项已启用的页面 |
| BOOT 双击 | 旋转 90° |
| BOOT 长按 | 开启或关闭临时 Wi‑Fi；开启时显示连接二维码 |
| 电源键单击 | 打开在「设置 → 电源键单击」配置的快捷页面，并暂停轮播 |
| 在快捷页面再单击电源键 | 返回原页面，重新开始该页的轮播计时 |
| 电源键双击 | 切到白屏或恢复 |
| 电源键长按 | 由原板电源管理芯片处理开关机 |

蓝灯用于充电与主动联网提示。充电时蓝灯常亮，充满和普通待机时蓝灯熄灭。电池供电时，主动开启 Wi‑Fi 后首次确认外网可用，蓝灯短亮约两秒。原板绿灯由电源线路控制。电池低于 10% 时，后台及下次刷新的日历、天气、日签、待办等信息页提示低电量。

## 下载后的文件

| 路径 | 用途 |
| --- | --- |
| `firmware/hueleaf-app-0x10000.bin` | 适配原厂分区的应用镜像 |
| `firmware/bootloader-0x2000.bin` | 完整刷机所需的引导程序 |
| `firmware/partition-table-0x8000.bin` | 4 MB 分区表 |
| `firmware/SHA256SUMS.txt` | 三个二进制文件的 SHA‑256 |
| `firmware/flash-layout.json` | 可供脚本读取的地址和镜像信息 |
| `source/` | 可独立构建的 ESP‑IDF 项目 |
| `RELEASE.md` | 建议仓库名、Topics 和可粘贴的 GitHub Release 文本 |

## 刷机前

1. 备份 SD 卡文件。接好可传数据的 USB‑C 线，关闭串口监视器和正在占用端口的刷机软件。
2. 确认设备是 HueLeaf Base ESP32‑C5、Flash 容量是 4 MB。应用升级推荐用原厂分区表。
3. 在下文找到对应系统的端口和命令。首次刷机或分区未知时，使用完整写入。正常升级只写应用即可。
4. 如需保留整块板子的本地备份，可先用下文的 `read_flash` 命令读出 4 MB 镜像。备份可能含 Wi‑Fi 配置和自用数据，请仅本地保存。

## Windows · PowerShell

在解压后的仓库根目录打开 PowerShell。可在「设备管理器 → 端口」查看 `COM` 编号，或对比插拔前后的列表。

```powershell
[System.IO.Ports.SerialPort]::GetPortNames()
py -3 -m venv .venv
$py = ".\.venv\Scripts\python.exe"
& $py -m pip install "esptool==4.13.0"
& $py -m esptool version
$port = "COM4"  # 替换为你看到的端口
& $py -m esptool --chip esp32c5 --port $port flash_id
Get-FileHash .\firmware\*.bin -Algorithm SHA256
```

可选的整板备份，以及对比分区表。两行哈希一致时，当前分区表与本包相同。

```powershell
& $py -m esptool --chip esp32c5 --port $port --baud 460800 read_flash 0x0 0x400000 hueleaf-local-backup.bin
& $py -m esptool --chip esp32c5 --port $port read_flash 0x8000 0xC00 current-partitions.bin
Get-FileHash current-partitions.bin, .\firmware\partition-table-0x8000.bin -Algorithm SHA256
```

**已有原厂分区的设备 · 只升级应用**

```powershell
& $py -m esptool --chip esp32c5 --port $port --baud 460800 write_flash 0x10000 .\firmware\hueleaf-app-0x10000.bin
```

**空白板或需要重写分区 · 完整写入**

```powershell
& $py -m esptool --chip esp32c5 --port $port --baud 460800 write_flash --flash_mode dio --flash_freq 80m --flash_size 4MB 0x2000 .\firmware\bootloader-0x2000.bin 0x8000 .\firmware\partition-table-0x8000.bin 0x10000 .\firmware\hueleaf-app-0x10000.bin
```

## macOS · Terminal

在解压后的仓库根目录运行。端口常类似 `/dev/cu.usbmodemXXXX` 或 `/dev/cu.usbserial-XXXX`，以你实际看到的为准。

```bash
ls /dev/cu.*
python3 -m venv .venv
. .venv/bin/activate
python -m pip install "esptool==4.13.0"
python -m esptool version
PORT=/dev/cu.usbmodemXXXX  # 替换为实际端口
python -m esptool --chip esp32c5 --port "$PORT" flash_id
(cd firmware && shasum -a 256 -c SHA256SUMS.txt)
```

可选的本地备份和分区表对比。

```bash
python -m esptool --chip esp32c5 --port "$PORT" --baud 460800 read_flash 0x0 0x400000 hueleaf-local-backup.bin
python -m esptool --chip esp32c5 --port "$PORT" read_flash 0x8000 0xC00 current-partitions.bin
shasum -a 256 current-partitions.bin firmware/partition-table-0x8000.bin
```

**已有原厂分区的设备 · 只升级应用**

```bash
python -m esptool --chip esp32c5 --port "$PORT" --baud 460800 \
  write_flash 0x10000 firmware/hueleaf-app-0x10000.bin
```

**空白板或需要重写分区 · 完整写入**

```bash
python -m esptool --chip esp32c5 --port "$PORT" --baud 460800 \
  write_flash --flash_mode dio --flash_freq 80m --flash_size 4MB \
  0x2000 firmware/bootloader-0x2000.bin \
  0x8000 firmware/partition-table-0x8000.bin \
  0x10000 firmware/hueleaf-app-0x10000.bin
```

看到 `Hash of data verified` 后，等待设备复位和第一次刷屏。若停在 `Connecting...`，按住 BOOT，短按 RESET，再在 esptool 识别芯片后松开 BOOT。刷写不稳定时，可把速率降到 `115200`。应用升级保留现有分区和 SD 内容；跨分区的完整刷写前请备份。这里使用 esptool 4.13.0 的下划线命令格式。[esptool 文档](https://docs.espressif.com/projects/esptool/en/latest/esp32c5/esptool/basic-commands.html)

## 首次进入后台

1. 正常启动后，长按 BOOT，等设备显示 Wi‑Fi 二维码。
2. 手机或电脑连接屏幕上的 **Eink E6 3.7**，浏览器打开 `http://192.168.4.1`。手机提示热点无法上网时，选择保持连接。
3. 在「网络」页点「扫描 Wi‑Fi」，选择自己的路由器并输入密码。设备拿到局域网地址后，也可以从同一局域网访问该地址。
4. 先用「文字」或「待办」发送一页，确认设备上屏，再配置行程、相册和轮播。
5. 无操作约 3 分钟后，设备关闭临时 Wi‑Fi。也可长按 BOOT 或用后台网络页的滑块关闭。下次要访问，重新用 BOOT 开启。

热点为开放网络，后台用于可信的近距离环境或局域网。请勿将后台端口暴露到公网。默认热点使用 5 GHz 信道 36，源码中的地区配置为 CN。其他地区使用时，请按当地频道配置核对 `wifi_mgr.c` 和 `wifi_mgr.h`。

### 常用操作

- **文字** · 在「文本」输入内容，调整样式，在预览核对画面后发送。要把文字加入轮播，先成功传输一张文字画面。
- **照片** · 在「图片」选择一张或多张照片。要保留到相册，选择保存到 SD。要临时上屏，在编辑器确认构图后直接传输。已存照片可以从缩略图重新选取，或多选删除。照片原文件留在卡上，旋转和抖动用于生成上屏画面。
- **天气** · 连接路由器后打开「天气」，检查地点，再手动刷新一次。确认屏幕有数据后可关闭 Wi-Fi，随后重新切到天气页检查离线缓存。地点改动后建议在线刷新一次，为新地点建立缓存。
- **待办** · 输入事项并按需填写截止时间。调整字号，在预览里检查文字是否完整，点击「保存并显示」。
- **日签** · 在「日历日签」点击「下载导入模板」，按模板填写 UTF-8 TSV 再导入。可显示当天摘句或指定一条。需要补充年份时，选择 2025 至 2099 之间的年份，再点「更新全年农历到 SD」。
- **历史** · 路由器有外网时让设备完成当日更新，然后在后台检查各日缓存。离线时设备按当前日期匹配卡上的记录。


## SD 卡数据

| 文件或目录 | 内容 |
| --- | --- |
| `photos/` | 已保存的照片原图和显示转换帧 |
| `WEATHER.BIN` | 主天气页最近一次有效的七天缓存 |
| `TRIPS.JSN` · `TRIPSEL.CFG` | 行程和手动选择的显示项 |
| `Wxxxxxxx.TSV` | 按目的地和抵达日期保存的行程天气 |
| `TODO.JSN` | 待办文字、完成状态、截止时间、字号及字形数据 |
| `QUOTES.TSV` | 摘句与出处 |
| `L2026.TSV`、`L2027.TSV` 等 | 各年份的全年农历、宜忌和节气 |
| `HMMDD.JSN` · `HUPDATE.DAT` | 历史事件和最近自动更新日期 |
| `PWRKEY.CFG` | 电源键单击快捷页面 |

设备首次发现缺少的年度资料或历史种子文件时会写入 SD 卡。旧版待办在首次读取时可迁移。拔卡前完成后台保存和屏幕刷新。固件遇到挂载失败会保留卡上资料，卡的格式化由电脑侧完成。

## 从源码构建

安装并启用 **ESP‑IDF v5.5.4**。Windows 使用对应版本的 ESP‑IDF 终端；macOS 可以用以下命令安装目标工具链。

```bash
git clone --branch v5.5.4 --recursive https://github.com/espressif/esp-idf.git
cd esp-idf
./install.sh esp32c5
. ./export.sh
```

返回本仓库根目录，每个系统都可运行下面的构建命令。随包 `sdkconfig` 已设置目标和分区布局。串口监视可用 `idf.py -p 实际端口 monitor`，按 Ctrl+] 退出。

```text
cd source
idf.py build
idf.py -p COM4 app-flash
```

macOS 把 `COM4` 换成实际的 `/dev/cu.*` 端口。需要编译并写入引导程序和分区表时，把 `app-flash` 改为 `flash`。Windows 和 macOS 安装细节见官方文档。[ESP‑IDF 安装指南](https://docs.espressif.com/projects/esp-idf/en/v5.5.4/esp32c5/get-started/index.html)

## 常见问题

| 现象 | 排查 |
| --- | --- |
| 没有串口 | 换一条数据线或 USB 口，对比插拔前后的设备列表。关闭占用端口的软件。 |
| 后台打不开 | 长按 BOOT 确认临时 Wi‑Fi 已开。连接设备热点后访问 `192.168.4.1`。回到局域网时使用路由器分配的实际地址。 |
| 搜索有候选，但天气仍待缓存 | 地点候选可以离线用。下载预报需要设备连上有外网的路由器，且抵达日位于预报范围内。后台「重新读取」可查看缓存状态。 |
| 历史今天没有新内容 | 需要设备能访问中文维基百科的数据接口。联网后可查看七天缓存。SD 卡上已有的日期资料仍可读。 |
| 不显示 SD 相册或待办保存失败 | 确认 SD 卡为 FAT32，关机重新插卡后启动。可以通过串口查看挂载结果。 |
| 黑白仍然慢 | 当前仍使用整屏刷新，和页面颜色设置无关。刷屏过程中可排队选择下一页。 |
| 需要恢复原厂固件 | 从原厂仓库获取适配 HueLeaf Base 的完整镜像，并按原厂说明写回。先备份设备及 SD 卡资料。 |

## 贡献和许可证

欢迎在 Issues 分享上游尚未实现的功能方案、同型号面板已验证的刷新指令、兼容性记录，或你自己开发的固件。提交 PR 时，请说明板卡和屏幕型号、ESP‑IDF 版本、复现步骤，以及实机验证结果。涉及刷屏可附 BUSY 耗时、残影和耗电测量。

基于上游的代码按 [Apache‑2.0](LICENSE) 发布。原厂硬件资料和文档适用 [CC BY‑NC‑SA 4.0](LICENSE-CC-BY-NC-SA-4.0)。本包保留第三方字体、库、数据来源和各自许可证，详见 [NOTICE.md](NOTICE.md)。
