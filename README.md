# ESP32-S3 Wi-Fi / UART 双向透传

基于 ESP-IDF 6.0 SoftAP 例程实现。数据路径为：

```text
TCP client <-- Wi-Fi / TCP --> ESP32-S3 <-- UART0 --> 外部串口设备
```

AP 模式创建热点，STA 模式加入已有热点；**两种模式均由 ESP32-S3 运行 TCP server**，外部设备作为 TCP client 连接。默认只转发一个 TCP 会话，后续连接等待当前会话释放。STA 模式不会主动连接远程 TCP server，因此不需要配置远程服务器 IP。

透传按字节处理，支持 `00`、`FF` 等二进制内容，不追加换行或协议头。TCP 是字节流，收发分块不会保留应用消息边界。

## 1. 配置模式、热点与 TCP 端口

编辑 [main/wifi_uart_config.h](main/wifi_uart_config.h)。默认值：

| 宏 | 默认值 | 作用 |
| --- | --- | --- |
| `TRANSP_WIFI_MODE` | `TRANSP_WIFI_MODE_AP` | AP 创建热点，STA 连接热点 |
| `TRANSP_WIFI_SSID` | `CONFIG_ESP_WIFI_SSID` | AP 热点名称 / STA 要连接的热点名称 |
| `TRANSP_WIFI_PASSWORD` | `CONFIG_ESP_WIFI_PASSWORD` | 对应热点密码 |
| `TRANSP_WIFI_CHANNEL` | `CONFIG_ESP_WIFI_CHANNEL` | AP 信道，STA 自动跟随路由器 |
| `TRANSP_MAX_STA_CONN` | `CONFIG_ESP_MAX_STA_CONN` | 可加入 AP 的 Wi-Fi 设备数；TCP 仍只有一个活动会话 |
| `TRANSP_TCP_PORT` | `3333` | ESP32-S3 监听端口，范围 1～65535 |

SSID 长度为 1～32 字节；密码为空表示开放热点，否则为 8～63 字节。STA 的有密码网络要求 WPA2/WPA3。ESP32-S3 使用 2.4 GHz Wi-Fi。

可以保留 SSID/密码宏对 `CONFIG_...` 的引用，在 `idf.py menuconfig → Example Configuration` 中修改；也可以直接把这两个宏改成字符串。直接赋值后，以头文件内容为准。下面的配置片段均用于**替换头文件中对应的宏**。

### AP 示例：创建热点

```c
#define TRANSP_WIFI_MODE TRANSP_WIFI_MODE_AP
#define TRANSP_WIFI_SSID "ESP32S3_UART"
#define TRANSP_WIFI_PASSWORD "uart12345"
#define TRANSP_TCP_PORT 3333
```

电脑/手机加入 `ESP32S3_UART`，再用 TCP client 连接 `192.168.4.1:3333`。实际地址以启动日志 `AP SSID: ...; connect TCP client to ...` 为准。默认仍使用原例程 menuconfig 的热点配置（`myssid` / `mypassword`）。

### STA 示例：加入路由器

```c
#define TRANSP_WIFI_MODE TRANSP_WIFI_MODE_STA
#define TRANSP_WIFI_SSID "MyRouter"
#define TRANSP_WIFI_PASSWORD "router12345"
#define TRANSP_TCP_PORT 5000
```

启动后等待日志：

```text
STA ready: connect TCP client to 192.168.1.123:5000
TCP server listening on port 5000 (one active client)
```

电脑连接同一路由器，TCP client 连接日志中的 IP 和端口。IP 由 DHCP 分配，可能变化；如果连接失败，检查路由器是否启用了客户端隔离。Wi-Fi 断线会结束当前会话并自动重连；网络恢复后，TCP client 需要重新连接。

## 2. 设置 UART 引脚和波特率

同样编辑 [main/wifi_uart_config.h](main/wifi_uart_config.h)：

```c
#define TRANSP_UART_PORT UART_NUM_0
#define TRANSP_UART_TX_PIN U0TXD_GPIO_NUM  // ESP32-S3: GPIO43
#define TRANSP_UART_RX_PIN U0RXD_GPIO_NUM  // ESP32-S3: GPIO44
#define TRANSP_UART_BAUD_RATE 115200
```

默认使用 UART0 的原生 U0TXD/U0RXD 引脚、8 数据位、无校验、1 停止位（115200 / 8N1），无硬件流控。`U0TXD_GPIO_NUM` / `U0RXD_GPIO_NUM` 来自 `soc/uart_pins.h`；在 ESP32-S3 上分别为 **GPIO43 / GPIO44**。驱动通过 `uart_set_pin()` 配置对应引脚。

与外部 MCU 或 USB-TTL 模块连接：

| ESP32-S3 | 外部设备 |
| --- | --- |
| GPIO43（U0TXD） | RX |
| GPIO44（U0RXD） | TX |
| GND | GND |

使用 **3.3 V TTL 电平**，不能直接连接 RS-232 电平。USB-TTL 模块接到电脑后，在串口工具中选择该模块的 COM 口和 115200 / 8N1。

### UART0 与日志、下载接口的冲突

- 工程已将二级 bootloader、应用日志和 panic 输出切换到 **USB Serial/JTAG**，UART0 用于透传。日志使用芯片原生 USB（GPIO19=D−、GPIO20=D+）连接电脑后出现的 COM 口，板上通常标为 USB/OTG；接到 CP210x/CH340 等桥接芯片的 UART USB 口不能显示这些应用日志。具体接口以原理图为准。
- 新建配置时 `sdkconfig.defaults` 会自动选择 USB 控制台；已有 `sdkconfig` 不会被 defaults 覆盖。若迁移旧配置，在 `idf.py menuconfig → Component config → ESP-STDIO` 中将 `Channel for console output` 设为 `USB Serial/JTAG Controller`，并将 secondary output 设为 `No secondary console`。当前工作区已同步修改；编译期检查仍会拒绝透传与日志共用同一个 UART。
- 若板载 USB-UART 桥的 TX 已连接 GPIO44，它与外部设备的 TX 会同时驱动 U0RXD。需要通过跳线/电阻等断开桥接芯片对应连接，或只使用该桥作为唯一串口对端；仅关闭电脑串口工具不能保证电气隔离。烧录时也应避免外部设备驱动 UART0，推荐通过原生 USB 烧录并查看日志。
- **上电/复位最早阶段的 ROM 启动信息仍可能从 U0TXD 输出**，外部设备应忽略启动阶段字节。本改动没有设置永久关闭 ROM 日志的选项，因为该选项会在启动时烧写 eFuse，见 [IDF ROM 日志配置说明](https://docs.espressif.com/projects/esp-idf/en/release-v6.0/esp32s3/api-reference/kconfig-reference.html#config-boot-rom-log-scheme)。若要求从上电起完全无杂字节，需要另行确认 ROM 日志屏蔽或外部硬件隔离方案。

例如，若开发板原理图确认 GPIO4、GPIO5 空闲，可改成：

```c
#define TRANSP_UART_PORT UART_NUM_0
#define TRANSP_UART_TX_PIN 4
#define TRANSP_UART_RX_PIN 5
#define TRANSP_UART_BAUD_RATE 9600
```

接线随之改为 GPIO4→对端 RX、GPIO5←对端 TX，对端也设置为 9600 / 8N1。

选脚必须对照实际板卡原理图：TX 必须支持输出，RX 必须支持输入，两个脚不可相同；GPIO19/20 已用于原生 USB 日志，应保留，GPIO26～32 通常连接 Flash/PSRAM，部分 Octal 模组还占用 GPIO33～37；GPIO0/3/45/46 是启动配置脚。GPIO43/44 需要确认是否连接了板载 USB-UART 桥。芯片和驱动限制参考 [Espressif GPIO 文档](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/peripherals/gpio.html)和 [UART 文档](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/peripherals/uart.html)。

## 3. 编译和烧录

在已导出 ESP-IDF 6.0 环境的终端中运行：

```powershell
idf.py set-target esp32s3
idf.py menuconfig
idf.py build
idf.py -p COM7 flash monitor
```

`COM7` 替换为开发板原生 USB Serial/JTAG 的 COM 口；已配置 ESP32-S3 的工程可跳过 `set-target`。修改头文件后重新编译、烧录。控制台变更还会更新 bootloader，应使用 `flash` 烧录完整构建产物，不能只烧录应用。按 `Ctrl+]` 退出 monitor。

## 4. 验证双向透传

### 两个调试工具

1. 按上表连接 USB-TTL 模块，打开串口工具。
2. 按 AP 或 STA 模式连接网络，TCP 调试工具使用 **TCP client** 连接开发板地址和端口。
3. TCP 工具发送 `hello\r\n`，串口工具应收到相同字节。
4. 串口工具发送 `world\r\n`，TCP 工具应收到相同字节。
5. 使用 HEX 发送 `00 01 7F 80 FF`，确认另一侧逐字节相同。
6. 断开 TCP 后重新连接，再重复双向收发。STA 模式还可关闭并恢复路由器，确认日志重新获得 IP，再重新连接 TCP。

### 自动回环检查

断开外部串口设备并隔离板载 USB-UART 桥的 TX，**直接短接 ESP32-S3 的 TX 和 RX 引脚**（默认 GPIO43↔GPIO44），使 TCP→UART TX 的内容通过 UART RX 返回 TCP。电脑连接对应网络后运行：

```powershell
python tests/tcp_loopback_test.py 192.168.4.1 --port 3333
```

STA 模式替换为日志中的 IP 和配置端口。脚本只使用 Python 标准库，每轮并行发送和接收 16384 字节（覆盖所有字节值），连续建立两次连接。成功时输出两行 `PASS connection ...`，内容错误或超时返回非零退出码。测试完成后移除短接线，再接回外部设备。

脚本默认期限为 10 秒，适用于默认 115200 波特率；如果改成 9600，将脚本中的 `TIMEOUT` 改为 30 秒或更长，避免 16384 字节的串口传输时间触发超时。

当前 UART0 配置已完成默认 AP 固件及 bootloader 的编译和链接；回环脚本通过了本机模拟 TCP 服务的正常回传、数据损坏及无响应检查。尚未烧录开发板，实际 Wi-Fi/UART 双向通信和断线恢复需要按上述步骤进行实机验收。

## 5. 运行边界

- 每次只服务一个 TCP client。正常关闭后可重新连接；终端突然掉电或离开 AP 时，旧会话释放可能需要等待 TCP keepalive 检测。当前参数为空闲 5 秒、间隔 5 秒、3 次探测。
- 未连接 TCP 时不保存串口数据；新会话会清空此前 UART RX 缓冲。建立会话前最多等待 1 秒让旧 UART TX 排空，超时会拒绝该连接，客户端可稍后重试。外部设备的延迟回复没有会话标记，仍可能被新连接接收。
- UART RX/TX 缓冲各 4096 字节。无流控时，持续串口输入超过网络处理速度可能溢出；115200 / 8N1 的理论单向上限约 11520 字节/秒。网络发送阻塞约 1 秒后会结束会话，避免失联客户端长期占用串口。需要持续高速无损传输时，应另行设计硬件流控或端到端确认协议。
- 当前连接结束时，正在传输的数据可能被截断；没有离线补发、消息分帧或交付确认。TCP 服务使用明文传输，没有额外登录验证，应在受信任的网络使用。
