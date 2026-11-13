# 用 ESP32-S3 把 AirPlay 音乐投到 Sonos

[English](README.md) · [固件下载](https://github.com/yanhuicsdn/airplay-sonos-esp32/releases)

用一块 USB 供电的 ESP32-S3，把 iPhone 上的音乐投到 Sonos Play:3。ESP32 接收 AirPlay 音频，编码成立体声 AAC，再让 Sonos 通过局域网拉取音频流。电脑只在首次 USB 刷写时用到，无需外接 DAC 或音频线，也无需修改 Sonos 固件。

```text
iPhone -- Wi-Fi 上的 AirPlay --> ESP32-S3 -- HTTP AAC 流 --> Sonos Play:3
```

## 硬件

- **ESP32-S3-WROOM-1-N16R8** 开发板，16 MB Flash、8 MB 八线 PSRAM。请核对模组上的完整型号。
- **Sonos Play:3**，已在 Sonos S1 系统中以单台音箱验证播放。
- ESP32 连接 2.4 GHz Wi-Fi。iPhone、ESP32 和 Play:3 要在同一局域网内。Play:3 可以用网线接入路由器。

## 刷写和配置

1. 从 [Releases](https://github.com/yanhuicsdn/airplay-sonos-esp32/releases) 下载 `airplay-sonos-esp32s3-n16r8.bin`。用 USB 把合并镜像写入 `0x0`，这会覆盖板上的原有固件。把 `PORT` 换成实际串口，例如 `/dev/cu.usbmodem…`、`/dev/ttyACM0` 或 `COM5`。

   ```sh
   python -m pip install "esptool>=5,<6"
   python -m esptool --chip esp32s3 --port PORT write-flash 0x0 airplay-sonos-esp32s3-n16r8.bin
   ```

   如果无法连接，按住 **BOOT**，点按 **RESET**，松开 **BOOT** 后重试。

2. 新刷写且尚未配网的板子会创建开放热点 **ESP32-AirPlay-Setup**。连上热点，在浏览器打开 **http://192.168.4.1/**，点 **Set up Wi-Fi**，让板子接入 2.4 GHz Wi-Fi。
3. 手机切回家里的 Wi-Fi，在路由器客户端列表找到 ESP32 的 IP，用浏览器打开。在 **Sonos** 区域填入 Play:3 的 IPv4 地址，点 **Save**。音箱地址可在 Sonos App 或路由器里查看。可以用路由器的 DHCP 地址保留功能固定音箱地址。
4. 在 iPhone 播放音乐，从控制中心的音频 AirPlay 菜单选择 **ESP32 AirPlay**。播放过程中如果改了 Sonos 地址，断开后重新连接一次 AirPlay。

发布镜像包含引导程序、分区表、应用和配置网页。请用 USB 刷写，使应用和网页一起更新。

## 播放表现

- AirPlay 启动和 Play:3 的网络预缓冲可能需要几秒。建议用于听音乐，视频口型同步和 Sonos 分组播放尚未验证。
- 能选到 AirPlay 设备但没有声音时，检查 Play:3 的 IP、房间音量，以及路由器是否开启了无线客户端隔离。音箱 IP 变化后，更新设置并重新连接 AirPlay。
- 这是预览版。最初的实机测试中曾出现卡顿。发布版把 PCM 调度和 AAC 编码分到不同核心，并记录 PCM 丢块、AAC 丢帧和编码错误，便于排查中断。

## 从源码构建

使用 [ESP-IDF v6.1](https://github.com/espressif/esp-idf/tree/v6.1)，安装 ESP32-S3 工具链并加载环境。

```sh
git clone --recurse-submodules --branch v0.1.0 https://github.com/yanhuicsdn/airplay-sonos-esp32.git
cd airplay-sonos-esp32
. /path/to/esp-idf/export.sh
./scripts/build-sonos.sh
```

合并镜像位于 `build-sonos/airplay-sonos-esp32s3-n16r8.bin`。Release 的对应源码包包含固定版本的子模块，构建依赖由 ESP-IDF Component Manager 下载。

## 上游与许可证

基于 [rbouteiller/airplay-esp32 的 `35be6fc`](https://github.com/rbouteiller/airplay-esp32/tree/35be6fcb58f7224a50b9f8500482fa8a2a989307)。项目按 [GPL-3.0-or-later](LICENSE) 发布，并保留[针对 Espressif 二进制组件的附加许可](LICENSE-EXCEPTION)。依赖许可证和协议参考见[第三方声明](THIRD-PARTY-NOTICES.md)。
