# GitHub 发布资料

推荐仓库 `tankey33/ESP32-C5-display-epd-3.7inch-480x720`

推荐标题 **HueLeaf Base 社区固件 · ESP32-C5 3.7 英寸六色墨水屏**

推荐标签 `v2026.11.25.1`

推荐 Topics `esp32-c5` `esp-idf` `e-paper` `eink` `epd037a02` `hueleaf`

## 仓库简介

ESP32-C5 六色墨水屏社区固件，支持离线日历、天气缓存、SD 相册、待办、行程与历史今天，附 Windows 和 macOS 刷机指南。

Community firmware for the OSPTEK HueLeaf Base with offline calendar, cached weather, SD gallery, to-dos, itineraries and On This Day.

## 可粘贴到 GitHub Release 的正文

### HueLeaf Base 社区固件 v2026.11.25.1

适配 ESP32-C5、4 MB Flash、3.7 英寸 480×720 EPD037A02 六色屏。包含 SD 相册、农历日签、待办、七天天气缓存、机票与高铁行程、目的地预报和历史今天。可按分钟轮播，也可设置电源键单击直达某页。

本次更新包括独立联网同步任务、Wi-Fi 租约、逐日历史缓存回补、同步诊断、可校准六色处理和后台资源拆分。旧版缓存和原厂 4 MB 分区布局保留。实机的断网回补、配网切换、色板与耗电有待复测。

下载 ZIP 后先读 README。原厂分区设备可只刷应用到 `0x10000`。完整安装所需的引导程序和分区表也已附上。刷机前请核对设备型号和各镜像的 SHA-256。

Built with ESP-IDF v5.5.4. The ZIP includes source, flash images, checksums and instructions for Windows and macOS.

基于鱼鹰光电 HueLeaf Base。欢迎分享上游尚未实现的功能方案、实机测试结果或自行开发的固件。 [原项目](https://gitee.com/osptek/devkit-hueleaf_base)

## 上传流程

1. 解压后，把内层目录中的内容放在 GitHub 仓库根目录。保留许可证和第三方声明。
2. 使用标签 `v2026.11.25.1` 创建 GitHub Release，将完整 ZIP 作为 Release 附件上传。
3. 核查仓库首页的中英文 README、固件路径，以及 Release 附件的下载链接。


### v2026.11.25.1 预发布

加入实验性 OKLab 量化、色板取样和 SD 校验存储。合成基准下经典保持默认。


### v2026.11.26.1

在「状态与网络 → 高级同步设置与诊断」设置每日联网时间。定时联网立即同步，约 15 分钟后关闭。手动联网可选 15 分钟自动关闭。照片上的 Wi-Fi 图标不再绘制底色。
