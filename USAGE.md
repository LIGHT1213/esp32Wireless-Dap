# ESP32 Wireless DAP 使用说明

本文档面向实际使用和调试，覆盖：

- 双端接线
- Windows 下构建与烧写
- Keil CLI / OpenOCD 验证
- Web OTA 使用方法

如果你想先快速了解项目结构，请看 [readme.md](./readme.md)。

## 1. 设备角色

项目由两块 ESP32-S3 组成：

- `frontend_a`
  - 连接 PC
  - 暴露 `CMSIS-DAP v2`
  - 暴露一个 USB CDC 串口用于 UART 透传
  - 通过 Wi-Fi 与后端通信
- `backend_b`
  - 连接目标板 SWD
  - 执行实际 SWD 操作
  - 开启热点
  - 托管 Web OTA 页面

整体链路：

`PC <-> frontend_a <-> Wi-Fi <-> backend_b <-> Target`

## 2. 默认网络参数

`backend_b` 默认热点参数如下：

| 项目 | 值 |
| --- | --- |
| SSID | `wireless-dap` |
| Password | `wirelessdap` |
| Backend IP | `192.168.4.1` |
| UDP Port | `3333` |
| OTA 页面 | `http://192.168.4.1/` |

说明：

- `frontend_a` 默认会自动连接这个热点
- 做 Web OTA 时，PC 也必须连接到该热点

## 3. 接线

### 3.1 A / B 与 PC

- `frontend_a` 使用 ESP32-S3 原生 USB 连接 PC
- 当前环境中，A 端烧写口为 `COM3`
- `backend_b` 用于烧写或供电
- 当前环境中，B 端烧写口为 `COM5`

### 3.2 backend_b 到目标板 SWD

默认 SWD GPIO 映射：

| backend_b GPIO | 目标板信号 |
| --- | --- |
| `GPIO5` | `SWCLK` |
| `GPIO4` | `SWDIO` |
| `GPIO6` | `nRESET` |
| `GND` | `GND` |

注意事项：

- 目标板必须与 `backend_b` 共地
- 当前实现假定目标电平为 `3.3V`
- `nRESET` 为开漏驱动

### 3.3 backend_b UART 透传

默认 UART bridge 引脚：

| backend_b GPIO | 功能 |
| --- | --- |
| `GPIO17` | UART TX |
| `GPIO18` | UART RX |

默认波特率：`115200`

如果要做回环测试，可临时短接：

- `GPIO17 <-> GPIO18`

## 4. 工具链与路径

当前工作区使用的 Windows 路径：

| 项目 | 路径 |
| --- | --- |
| Workspace | `C:\Users\pan39\Desktop\workSpace\esp32Wireless-Dap` |
| ESP-IDF | `D:\esp-idf` |
| Keil | `C:\ARM-KEIL` |

推荐使用：

- `PowerShell`

## 5. 构建

### 5.1 构建 frontend_a

```powershell
C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe -ExecutionPolicy Bypass -NoProfile -Command "& { . 'D:\esp-idf\export.ps1'; Set-Location 'C:\Users\pan39\Desktop\workSpace\esp32Wireless-Dap\frontend_a'; idf.py build }"
```

### 5.2 构建 backend_b

```powershell
C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe -ExecutionPolicy Bypass -NoProfile -Command "& { . 'D:\esp-idf\export.ps1'; Set-Location 'C:\Users\pan39\Desktop\workSpace\esp32Wireless-Dap\backend_b'; idf.py build }"
```

## 6. 首次烧写

第一次启用 OTA 前，必须先通过串口把支持 OTA 的基线固件刷进去。

### 6.1 烧写 backend_b 到 COM5

```powershell
C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe -ExecutionPolicy Bypass -NoProfile -Command "& { . 'D:\esp-idf\export.ps1'; Set-Location 'C:\Users\pan39\Desktop\workSpace\esp32Wireless-Dap\backend_b'; idf.py -p COM5 flash }"
```

### 6.2 烧写 frontend_a 到 COM3

```powershell
C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe -ExecutionPolicy Bypass -NoProfile -Command "& { . 'D:\esp-idf\export.ps1'; Set-Location 'C:\Users\pan39\Desktop\workSpace\esp32Wireless-Dap\frontend_a'; idf.py -p COM3 flash }"
```

如果端口不同，请替换成你的实际 `COMx`。

## 7. Keil CLI 验证

项目默认以 Keil CLI 为主要验证路径，不依赖 GUI。

示例命令：

```powershell
C:\ARM-KEIL\UV4\UV4.exe -j0 -f C:\Users\pan39\Desktop\workSpace\dapTest\MDK-ARM\dapTest.uvprojx -t dapTest -o C:\Users\pan39\Desktop\workSpace\dapTest\MDK-ARM\flash_cli.log
```

期望成功日志包含：

- `Erase Done.`
- `Programming Done.`
- `Verify OK.`

说明：

- 如果下载成功但 Keil 报 `Cannot enter Debug Mode`，这通常是单独的 debug attach 问题，不等同于烧录失败

## 8. OpenOCD 验证

如果需要，也可以用 OpenOCD 做基础访问验证：

```powershell
D:\OpenOCD-20260302-0.12.0\bin\openocd.exe -s D:\OpenOCD-20260302-0.12.0\share\openocd\scripts -f interface/cmsis-dap.cfg -f target/stm32g0x.cfg -c "adapter serial 206EF1D601A8" -c "adapter speed 1000" -c "init" -c "halt" -c "mdw 0x08000000 12" -c "exit"
```

## 9. Web OTA

Web OTA 是当前推荐的升级方式。

### 9.1 前提条件

- `frontend_a` 与 `backend_b` 都已经刷入支持 OTA 的基线固件
- `backend_b` 已正常上电并开启热点
- `frontend_a` 已自动连接到 `backend_b`
- PC 已连接热点：
  - SSID：`wireless-dap`
  - Password：`wirelessdap`

### 9.2 页面入口

浏览器打开：

```text
http://192.168.4.1/
```

页面会自动探测：

- `backend_b`
- `frontend_a`

其中：

- `backend_b` 固定为 `192.168.4.1`
- `frontend_a` 由 B 端通过现有无线链路自动发现并显示 IP

### 9.3 升级步骤

1. PC 连接 `wireless-dap`
2. 打开 `http://192.168.4.1/`
3. 确认页面已经识别到要升级的设备
4. 选择本地 `.bin`
   - A 端使用 `wdap_frontend_a.bin`
   - B 端使用 `wdap_backend_b.bin`
5. 点击上传
6. 等待设备写入、切换 OTA 分区并重启

### 9.4 升级行为

- 当前仅支持“单端手动升级”
- 不支持网页一键同时升级 A/B
- 仅支持上传原始 app `.bin`
- 不支持通过网页升级：
  - `bootloader`
  - `partition table`
  - `NVS`

### 9.5 升级后的现象

#### 升级 frontend_a

- 页面本身不会掉
- A 重启后会重新连回 B
- 页面会重新识别 A

#### 升级 backend_b

- 热点会断开
- 这是正常现象
- `backend_b` 重启后会恢复热点
- PC 需要重新连接：
  - SSID：`wireless-dap`
  - Password：`wirelessdap`
- 然后重新打开 `http://192.168.4.1/`

### 9.6 成功判定

升级成功后，设备会切换到新的 OTA 分区。

可以访问：

- `http://192.168.4.1/api/info`
- `http://<frontend_ip>/api/info`

查看：

- `running_partition`
- `version`
- `busy`

## 10. Release 固件下载

如果不想本地编译，可以从 GitHub Release 下载固件包：

1. 打开 [Latest Release](https://github.com/LIGHT1213/esp32Wireless-Dap/releases/latest)
2. 下载：
   - `esp32-wireless-dap-frontend_a-<版本>.zip`
   - `esp32-wireless-dap-backend_b-<版本>.zip`
3. 解压后使用其中的 `FLASH_COMMAND.txt` 或 `flash_args.txt` 烧写

每个压缩包通常包含：

- `bootloader.bin`
- `partition-table.bin`
- `wdap_frontend_a.bin` 或 `wdap_backend_b.bin`
- `flash_args.txt`
- `FLASH_COMMAND.txt`
- `manifest.json`
- `SHA256SUMS.txt`

示例烧写命令：

```powershell
python -m esptool --chip esp32s3 --port COM3 --baud 460800 --before default_reset --after hard_reset write_flash @flash_args.txt
```

```powershell
python -m esptool --chip esp32s3 --port COM5 --baud 460800 --before default_reset --after hard_reset write_flash @flash_args.txt
```

## 11. 当前默认配置

当前默认启用：

- `CMSIS-DAP v2`
- USB CDC UART bridge
- 双 OTA 分区
- OTA rollback
- TinyUSB vendor FIFO `512 / 512`
- 默认 SWD 时钟 `1 MHz`
- SWD 最大时钟上限 `10 MHz`
- CPU 主频 `240 MHz`
- 更大的 Wi-Fi / LWIP buffer
- 任务绑核优化
- backend SWD PHY 热路径 `gpio_ll + IRAM_ATTR`

## 12. 备注

- 通用 `DAP_Transfer` batching 不能全开，否则会破坏 Keil 兼容性
- 当前只保留安全子集 batching
- B 端 OTA 后，热点通常会先恢复，HTTP 页面可能会稍晚几秒到几十秒恢复
