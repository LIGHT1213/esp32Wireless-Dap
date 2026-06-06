# 风险与分层诊断

## 高风险点

- USB 识别风险：CMSIS-DAP v2 WinUSB 在 Windows/Keil 下依赖 USB 描述符、Interface string、MS OS descriptor、端点大小；Keil 可能能枚举 USB 设备但不认 CMSIS-DAP。
- DAPLink 端口层风险：官方 `DAP.c`/`SW_DP.c` 假设底层 GPIO inline 操作足够快，ESP-IDF GPIO API 可能过慢；首版需保守降 SWD 频率。
- SWD 时序风险：ESP-NOW 往返延迟不影响单次 B 端本地 SWD bit-bang，但 PC 侧 DAP request 超时会受无线重传和分片影响。
- ESP-NOW 可靠性风险：ESP-NOW 非可靠流，必须正确实现 ACK/NACK、timeout、retry、CRC 和重复包处理；否则 DAP packet 损坏会导致 Keil 误判为 DAP/SWD 故障。
- MAC 风险：esptool 读到的是 base MAC；ESP-IDF 默认生成 `ESP_MAC_WIFI_STA` 时直接复制 base MAC，但仍需要固件启动后打印 `esp_wifi_get_mac(WIFI_IF_STA)` 复核。
- Channel 风险：A/B channel 不一致会导致 peer 添加成功但通信失败或回调异常。
- nRESET 风险：GPIO6 必须开漏或模拟开漏，禁止释放态强推与目标板电平冲突。
- 目标板物理风险：B 端与目标板必须共地，目标板必须自行供电或供电方案明确；本项目未规划给目标板供电。
- Keil 工程风险：测试工程必须配置 CMSIS-DAP、正确芯片、正确 Flash Download algorithm，否则即便 DAP 工作也会下载失败。
- PowerShell 策略风险：当前系统默认禁止加载 `D:\esp-idf\export.ps1`，脚本需使用进程级 `-ExecutionPolicy Bypass` 或用户在 ESP-IDF Shell 中运行。

## 分层诊断顺序

### 1. USB 层

- Windows 设备管理器是否出现 CMSIS-DAP/WinUSB/HID 设备。
- USB VID/PID、Product string、Interface string 是否含 `CMSIS-DAP`。
- TinyUSB 是否 mounted/configured。
- OUT endpoint 是否收到 PC 请求，IN endpoint 是否返回响应。
- `DAP_Info` 的 Vendor/Product/Serial/Packet Size/Packet Count/Capabilities 是否符合 Keil 预期。

### 2. ESP-NOW 层

- A/B 固件启动日志中的 STA MAC 是否分别为 `20:6e:f1:d6:01:a8` 和 `20:6e:f1:d6:02:d0`。
- A 端 peer 是否写为 B 端 MAC，B 端 peer 是否写为 A 端 MAC。
- channel 是否同为 `6`。
- `esp_now_init()` 和 `esp_now_add_peer()` 是否成功，失败时记录 `esp_err_t`。
- PING/PONG 是否稳定，重传率是否异常。
- 分片 `frag_idx/frag_cnt`、CRC32、ACK/NACK、timeout 是否有错误统计。

### 3. DAPLink 层

- B 端是否收到完整 `WDAP_PKT_DAP_REQ`。
- B 端是否调用官方 `DAP_ProcessCommand()`。
- `DAP_Connect` 是否返回 SWD 成功。
- `DAP_SWJ_Clock` 是否被限幅到可实现范围。
- `DAP_Transfer` / `DAP_TransferBlock` 返回的 ACK/FAULT/WAIT 是否符合目标状态。

### 4. SWD 物理层

- GPIO5 SWCLK 是否有波形。
- GPIO4 SWDIO 是否在写/读之间正确切换方向。
- GPIO6 nRESET 拉低和释放是否符合开漏语义。
- 目标 MCU 是否上电、未被外部复位保持。
- B 端 GND 与目标板 GND 是否共地。

### 5. Keil 层

- `C:\ARM-KEIL\UV4\UV4.exe` 是否可执行。
- `.uvprojx` 是否为 `C:\Users\pan39\Desktop\workSpace\DapTestH7\MDK-ARM\DapTestH7.uvprojx`。
- TargetName 是否为 `DapTestH7`。
- Debug Adapter 是否配置为 CMSIS-DAP。
- Flash Download algorithm 是否匹配目标 MCU。
- 日志是否包含 `CMSIS-DAP not found`、`Cannot enter Debug Mode`、`No Cortex-M Device found`、`SWD/JTAG Communication Failure`、`Flash Download failed`、Access Port / Debug Port 错误。

## 缓解策略

- USB 首版优先实现 v2 WinUSB；若 Keil 不识别，添加 HID CMSIS-DAP v1 fallback 验证上层 DAP 与无线链路。
- SWD 初始频率降到 500 kHz 或 1 MHz，下载稳定后再优化 GPIO 热路径。
- ESP-NOW 首版采用 stop-and-wait，先保证每个 DAP packet 完整可靠。
- 所有 callback 只投递 queue，避免 Wi-Fi task 阻塞。
- 所有 timeout、retry、CRC 错误都计数并低频打印。
- Keil 下载测试必须保存真实日志，不以主观现象判断成功。
