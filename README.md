# AirPlay to Sonos on ESP32-S3

[简体中文](README.zh-CN.md) · [Firmware releases](https://github.com/yanhuicsdn/airplay-sonos-esp32/releases)

Play music from an iPhone on a Sonos Play:3 using a USB-powered ESP32-S3. The ESP32 receives AirPlay audio, encodes it as stereo AAC, and tells the Sonos to pull the stream over the local network. A computer is needed for initial USB flashing. No DAC, analog cable, or Sonos firmware modification is required.

```text
iPhone -- AirPlay over Wi-Fi --> ESP32-S3 -- AAC over HTTP --> Sonos Play:3
```

## Hardware

- **ESP32-S3-WROOM-1-N16R8** development board with 16 MB of flash and 8 MB of octal PSRAM. Check the full module marking.
- **Sonos Play:3**, tested as a standalone player in a Sonos S1 system.
- A 2.4 GHz Wi-Fi network for the ESP32. The iPhone and Play:3 must be on the same local network as the bridge. The Play:3 can use Ethernet.

## Flash and set up

1. Download `airplay-sonos-esp32s3-n16r8.bin` from [Releases](https://github.com/yanhuicsdn/airplay-sonos-esp32/releases). Flash the merged image to address `0x0` over USB. This overwrites existing firmware. Replace `PORT` with your board’s serial port, such as `/dev/cu.usbmodem…`, `/dev/ttyACM0`, or `COM5`.

   ```sh
   python -m pip install "esptool>=5,<6"
   python -m esptool --chip esp32s3 --port PORT write-flash 0x0 airplay-sonos-esp32s3-n16r8.bin
   ```

   If esptool cannot connect, hold **BOOT**, tap **RESET**, then release **BOOT** and retry.

2. On a fresh board, join the open Wi-Fi network **ESP32-AirPlay-Setup**. Open **http://192.168.4.1/**, choose **Set up Wi-Fi**, and connect the board to your 2.4 GHz network.
3. Rejoin your normal Wi-Fi and open the ESP32’s assigned IP address in a browser. You can find it in your router’s client list. Under **Sonos**, enter your Play:3’s IPv4 address from the Sonos app or router and click **Save**. A DHCP reservation keeps that address stable.
4. Start music on your iPhone, open the audio AirPlay picker in Control Center, and choose **ESP32 AirPlay**. If you change the Sonos address while playing, disconnect and start a new AirPlay session.

The release image includes the bootloader, partition table, application, and setup pages. Flash it over USB so the app and web pages are updated together.

## Playback behavior

- Startup can take several seconds while AirPlay starts and the Play:3 buffers the network stream. Use the bridge for music. Video lip sync and Sonos grouping are outside the tested configuration.
- If the bridge appears but stays silent, check the Play:3 address, room volume, and whether the router isolates Wi-Fi clients. If the Sonos address changed, update it and reconnect AirPlay.
- This is a preview. Playback stalls were observed during the original hardware test. The release separates PCM scheduling from AAC encoding and logs PCM drops, AAC drops, and encoder errors to help diagnose interruptions.

## Build from source

Use [ESP-IDF v6.1](https://github.com/espressif/esp-idf/tree/v6.1) with its environment activated and the ESP32-S3 tools installed.

```sh
git clone --recurse-submodules --branch v0.1.0 https://github.com/yanhuicsdn/airplay-sonos-esp32.git
cd airplay-sonos-esp32
. /path/to/esp-idf/export.sh
./scripts/build-sonos.sh
```

The merged image is written to `build-sonos/airplay-sonos-esp32s3-n16r8.bin`. The release’s corresponding source archive includes the pinned submodule. ESP-IDF Component Manager fetches the build dependencies.

## Credits and license

Based on [rbouteiller/airplay-esp32 at `35be6fc`](https://github.com/rbouteiller/airplay-esp32/tree/35be6fcb58f7224a50b9f8500482fa8a2a989307). The project is distributed under [GPL-3.0-or-later](LICENSE) with the [additional permission for Espressif binary components](LICENSE-EXCEPTION). See [third-party notices](THIRD-PARTY-NOTICES.md) for dependency licenses and protocol credits.
