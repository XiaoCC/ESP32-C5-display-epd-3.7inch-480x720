# FrameFilm / ForFilm 与 HueLeaf 六色显示对照

参考 FrameFilm 提交 d5e2a6b。仓库 README 与 ForFilm README、convert.js、sz_enhanced.js、Ark 测试及发布工作流是本次核对的主要依据。

## 项目与架构

FrameFilm 包含彩色电子纸终端、ESP-IDF 固件、ForFilm 浏览器工具、手机端和 Film-hub。固件以服务、硬件抽象和系统组件分层。ForFilm 在浏览器完成照片转换和预览，使用 .Film 文件及该项目的 BLE/Wi-Fi/USB 通路。发布工作流构建固件并部署 GitHub Pages，测试覆盖转换及浏览器交互。

ForFilm 的增强路径使用另一款面板的扫描色板，并把基础抖动结果作为种子，在后处理中结合色相、边缘或区域信息。照片转换与显示预览分离，使参数可以迭代而无需反复刷新实体屏。

## 落地取舍

| 顺序 | 做法 | 成本和风险 | 协议影响 |
| --- | --- | --- | --- |
| 1 | 借鉴先量化、后约束异色点的流程。HueLeaf 用独立实现，仅清理平坦区里被同色墨点包围且替换后局部线性颜色误差没有增加的彩点 | 小。可能改变局部墨色占比，保留为可选 | 无 |
| 2 | 沿用设备校准色板、Worker 和同一预览输出 | 小。实屏效果依赖校准 | 无 |
| 3 | 暂缓照搬其它面板的实测色板、照片边框检测和整套增强预设 | 面板、测量条件和图像目标不同 | 无 |
| 4 | 不迁入 .Film、BLE/USB OTA、Film-hub 和设备驱动 | 涉及传输、内存布局和硬件支持，超出六色优化 | 有 |

FrameFilm 根目录为 GPL-3.0，HueLeaf 上游为 Apache-2.0。这里只作技术参考，新增实现未复制其代码、实测色值或资源。

## HueLeaf 联网核对

现有 net_sync 已有 NTP 通知、后台 Wi-Fi 租约、成功后记账、逐日历史缓存和独立退避，以及每日联网与天气页更新事件。2.4 GHz 路由器和热点同信道是 AP+STA 约束，现有后台提示和局域网二维码用于切换访问地址。此次没有重复改动这些流程；DHCP 超时和异常状态仍需分别实测。

[FrameFilm](https://github.com/kirmisaki/FrameFilm) · [ForFilm](https://github.com/kirmisaki/FrameFilm/tree/main/tools/ForFilm)
