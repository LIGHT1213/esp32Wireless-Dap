# ESP32-S3 Wireless CMSIS-DAP 实施计划

## 阶段 0 只读检查结论

- 系统环境：Windows，PowerShell 5.1。
- ESP-IDF：`D:\esp-idf\export.ps1` 存在；默认 PowerShell 执行策略会阻止加载脚本，使用进程级 `-ExecutionPolicy Bypass` 后可加载；`idf.py --version` 返回 `ESP-IDF v5.4.1`。
- Keil：`C:\ARM-KEIL\UV4\UV4.exe` 存在；Keil Skill `env` 检测通过，并发现 `uvision.com`。
- 串口：`COM3`、`COM5` 存在，PnP 名称均为 `USB-Enhanced-SERIAL CH343`；按用户约定 `COM3` 为 A 端，`COM5` 为 B 端。
- 测试 Keil 工程：`C:\Users\pan39\Desktop\workSpace\DapTestH7\MDK-ARM\DapTestH7.uvprojx`，解析到 TargetName 为 `DapTestH7`。
- A 端芯片 MAC：`20:6e:f1:d6:01:a8`，来自 `python -m esptool --chip esp32s3 -p COM3 read_mac`。
- B 端芯片 MAC：`20:6e:f1:d6:02:d0`，来自 `python -m esptool --chip esp32s3 -p COM5 read_mac`。
- STA MAC 判断：ESP-IDF `mac_addr.c` 中 `ESP_MAC_WIFI_STA` 由 base MAC 直接复制生成；默认 ESP32-S3 配置下 esptool 读出的 base MAC 可作为 Wi-Fi STA MAC。首版固件仍会启动后打印 `esp_wifi_get_mac(WIFI_IF_STA, mac)` 做实测确认。
- 官方 DAPLink：通过 SOCKS5 代理 `192.168.31.216:1080` 只读克隆到临时目录，仓库为 `https://github.com/ARMmbed/DAPLink`，分支 `main`，当前提交 `8f4e9e4`，许可证 `Apache-2.0`。

## 固定 MAC 配置

首版不做广播发现、HELLO 自动配对、动态绑定或 NVS 保存 peer。

- `frontend_a` 写死 peer 为 B 端 STA MAC：`20:6e:f1:d6:02:d0`。
- `backend_b` 写死 peer 为 A 端 STA MAC：`20:6e:f1:d6:01:a8`。
- Wi-Fi 模式固定为 `ESP_WIFI_MODE_STA`。
- ESP-NOW channel 初始固定为 `6`，写入 `sdkconfig.defaults`/Kconfig/board config。
- 启动日志必须打印本端 STA MAC、peer MAC、channel、`esp_now_add_peer` 结果。

## 目标工程结构

```text
wireless-dap-espnow-daplink/
  frontend_a/
    CMakeLists.txt
    main/
    sdkconfig.defaults
  backend_b/
    CMakeLists.txt
    main/
    sdkconfig.defaults
  components/
    wdap_transport/
      include/
      src/
    wdap_protocol/
      include/
      src/
    daplink_port/
      include/
      src/
    usb_dap_frontend/
      include/
      src/
    backend_swd_gpio/
      include/
      src/
    board_config/
      include/
  third_party/
    DAPLink/
  tools/
    build_all.ps1
    flash_a.ps1
    flash_b.ps1
    monitor_a.ps1
    monitor_b.ps1
    read_mac_a.ps1
    read_mac_b.ps1
    keil_flash_test.ps1
    parse_keil_log.py
  docs/
    PLAN.md
    RISK.md
    TEST_PLAN.md
    IMPLEMENTATION_LOG.md
  README.md
```

## DAPLink 官方源码分析

官方 CMSIS-DAP 核心位于 DAPLink：

- `source/daplink/cmsis-dap/DAP.c`：CMSIS-DAP 命令分发和协议处理核心。
- `source/daplink/cmsis-dap/DAP.h`：命令 ID、响应码、DAP 数据结构、`DAP_ProcessCommand()` 声明。
- `source/daplink/cmsis-dap/SW_DP.c`：SWD 传输实现，被 `DAP_Transfer` / `DAP_TransferBlock` 调用。
- `source/daplink/cmsis-dap/JTAG_DP.c`：JTAG 传输实现，首版禁用。
- `source/daplink/cmsis-dap/dap_strings.h`：DAP_Info 字符串接口默认实现。
- `source/daplink/cmsis-dap/DAP_queue.c` / `DAP_queue.h`：DAP 队列辅助，按移植需求选择是否复用。
- `source/hic_hal/*/DAP_config.h`：不同 HIC 芯片的 CMSIS-DAP 硬件适配模板。
- `source/usb/winusb/usbd_core_winusb.c`、`source/usb/hid/usbd_core_hid.c`：官方 USB 侧参考，但 ESP-IDF 下不直接移植其 USB device stack，优先用 TinyUSB 重建描述符和端点。

关键入口与必须复用的官方实现：

- `DAP_ProcessCommand(const uint8_t *request, uint8_t *response)`：B 端 DAP 处理主入口。
- `DAP_Info()`：由官方 `DAP.c` 内部处理，移植层提供字符串/能力/包大小配置。
- `DAP_SWJ_Pins()`、`DAP_SWJ_Clock()`、`DAP_SWJ_Sequence()`：保留官方命令解析，底层映射到 ESP32-S3 GPIO。
- `DAP_Transfer()`、`DAP_TransferBlock()`：保留官方协议解析和 SWD/JTAG 分发，首版只启用 SWD。
- `DAP_ResetTarget()`：保留官方命令处理，底层 `RESET_TARGET()` 和 nRESET GPIO 由 ESP32-S3 适配。

## DAPLink 移植边界

### 直接引入或编译的官方源码

- `DAP.c`
- `DAP.h`
- `SW_DP.c`
- `dap_strings.h`
- 必要时引入 `DAP_vendor.c`、`daplink_vendor_commands.h`，但首版可以先缩小范围，仅保证 Keil 下载路径。

### 首版不启用或显式禁用

- JTAG：`DAP_JTAG=0`。
- SWO UART / Manchester：`SWO_UART=0`、`SWO_MANCHESTER=0`。
- DAP UART 命令：`DAP_UART=0`；CDC UART bridge 走独立 ESP-NOW packet，不混入 DAPLink UART 命令路径。
- DAPLink drag-and-drop / MSC / WebUSB / Bootloader / target flash algorithm 框架：不移植。

### ESP32-S3 端口层需要实现

- `DAP_config.h`：放在 `components/daplink_port/include`，定义 `DAP_SWD=1`、`DAP_JTAG=0`、`DAP_PACKET_SIZE`、`DAP_PACKET_COUNT`、`DAP_DEFAULT_SWJ_CLOCK`、`CPU_CLOCK`、`IO_PORT_WRITE_CYCLES` 等。
- `PIN_SWCLK_TCK_SET/CLR/IN` 等 SWCLK/TCK 宏或 inline 函数。
- `PIN_SWDIO_TMS_SET/CLR/IN`、`PIN_SWDIO_OUT_ENABLE()`、`PIN_SWDIO_OUT_DISABLE()`。
- `PIN_nRESET_OUT()`，按开漏或模拟开漏实现。
- `DAP_SETUP()`，初始化 GPIO5/GPIO4/GPIO6。
- `RESET_TARGET()`，首版返回无自定义序列，并确保 nRESET 可通过 `DAP_SWJ_Pins` 控制。
- `TIMESTAMP_GET()`，可基于 `esp_timer_get_time()` 或禁用 timestamp。
- `LED_CONNECTED_OUT()`、`LED_RUNNING_OUT()` 空实现或日志安全实现。
- `DAP_GetVendorString()` / Product / Serial 等，可使用 `dap_strings.h` 默认宏覆盖。

## B 端 SWD GPIO 方案

- GPIO5：SWCLK，普通 GPIO 输出，热路径后续可优化为寄存器/`gpio_ll`。
- GPIO4：SWDIO，支持输出和输入方向切换。
- GPIO6：nRESET，使用开漏 GPIO 或输出低/输入高阻模拟开漏；释放态不得强推高压到目标板。
- GPIO17：UART TX，预留 CDC bridge。
- GPIO18：UART RX，预留 CDC bridge。
- UART 默认波特率：115200。
- 初始 SWD clock：保守设置为 500 kHz 或 1 MHz；`DAP_SWJ_Clock` 做上限限幅。
- SWD bit-bang 热路径禁止刷日志，优先 `IRAM_ATTR`。

## ESP-NOW 可靠传输方案

首版采用 stop-and-wait，优先稳定而非吞吐。

- 公共组件：`wdap_protocol` 定义 packet header、CRC、状态码；`wdap_transport` 封装 ESP-NOW、队列、重传、重组。
- Packet header 使用用户给定结构，包含 `magic`、`version`、`type`、`session_id`、`msg_id`、`seq`、`ack_seq`、`frag_idx`、`frag_cnt`、`payload_len`、`flags`、`crc32`。
- 默认 payload MTU：先取 200~240 bytes，避免触碰 ESP-NOW v1/v2 边界和 vendor action frame 额外开销。
- CRC：首版使用 CRC32。
- ACK/NACK：每片确认；超时重发；超出重试次数返回错误。
- 重复包：按 `session_id + msg_id + seq + frag_idx` 去重。
- 乱序包：首版同一消息只接受按序重组，乱序 NACK 或丢弃并等待重传。
- ESP-NOW callback：只复制数据并投递 FreeRTOS queue，不做 DAP 处理、不刷屏。

## A 端 USB CMSIS-DAP 方案

- 使用 ESP32-S3 原生 USB + ESP-IDF TinyUSB。
- 最终目标：CMSIS-DAP v2 WinUSB bulk，被 Keil 识别。
- 阶段性 fallback：如果 WinUSB / MS OS descriptor 影响 Windows 识别，临时增加 CMSIS-DAP v1 HID 进行链路验证，但不替代最终目标。
- USB DAP task 从 bulk OUT/HID OUT 读取完整 CMSIS-DAP request，投递到 ESP-NOW 请求队列；等待 B 端 response 后从 bulk IN/HID IN 返回。
- USB task 不直接阻塞在 ESP-NOW callback；必须有请求超时，超时向 PC 返回 DAP failure 或明确错误响应。
- USB string/product/interface 需包含 `CMSIS-DAP`，DAP_Info Vendor/Product/Serial 与 USB 描述符保持一致。

## 执行顺序

1. 创建工程骨架和公共组件目录。
2. 将官方 DAPLink 引入 `third_party/DAPLink`，保留 `LICENSE` 和提交记录说明。
3. 编写 `board_config.h`，固化 A/B MAC、channel、GPIO、UART、SWD 默认频率。
4. 实现 `daplink_port` 最小移植层，优先让 B 端能编译 `DAP.c + SW_DP.c`。
5. 实现 `backend_swd_gpio`，完成 GPIO 初始化和 DAP_config 宏绑定。
6. 实现 `wdap_protocol` 的 header、CRC32、分片/重组数据结构。
7. 实现 `wdap_transport` 的 ESP-NOW 固定 peer、ACK/NACK、timeout、retry、PING/PONG。
8. 实现 `backend_b` DAP worker：接收 DAP_REQ、调用 `DAP_ProcessCommand()`、返回 DAP_RSP。
9. 实现 `frontend_a` TinyUSB CMSIS-DAP v2 WinUSB bulk，必要时加 HID fallback。
10. 提供 PowerShell 工具脚本和 README 命令说明。
11. 先构建 `backend_b`，再构建 `frontend_a`。
12. 先烧写 B 端 COM5，再烧写 A 端 COM3。
13. monitor 两端日志确认 Wi-Fi、ESP-NOW、peer、PING/PONG、SWD init、USB mounted。
14. Windows 设备管理器确认 CMSIS-DAP/WinUSB/HID 设备。
15. 运行 Keil CLI 下载测试，依据真实日志判断成功/失败。

## 第一阶段完成判据

- 文档输出完整。
- 官方 DAPLink 结构、许可证、关键入口、移植边界明确。
- A/B STA MAC 和 Keil 工程 TargetName 已记录。
- 后续实现不会自写 CMSIS-DAP 协议栈，而是复用官方 `DAP_ProcessCommand()` 和相关命令处理。
