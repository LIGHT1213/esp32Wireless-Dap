# ESP32 Wireless DAP

双端 ESP32-S3 无线 CMSIS-DAP 工程。

它把传统 USB 调试器拆成了两个节点：

- `frontend_a`
  - 通过原生 USB 连接 PC
  - 对 PC 暴露 `CMSIS-DAP v2`
  - 额外提供一个 USB CDC 串口用于 UART 透传
  - 通过 Wi-Fi 与后端通信
- `backend_b`
  - 通过 SWD 连接目标板
  - 执行实际的 SWD 读写、复位、下载
  - 作为 Wi-Fi 热点和 Web OTA 页面宿主

整体链路如下：

`PC <-> frontend_a <-> Wi-Fi <-> backend_b <-> Target`

## 功能概览

当前仓库已经实现并验证：

- `CMSIS-DAP v2`
- `Keil CLI` 下载
- `OpenOCD` 基础访问
- A/B 双端无线链路
- A 端 USB CDC <-> B 端 UART 透传
- A/B 双端 Web OTA

已验证的下载结果包含：

- `Erase Done.`
- `Programming Done.`
- `Verify OK.`

## 工程结构

- [`frontend_a`](./frontend_a)
  - PC 侧 USB 前端
- [`backend_b`](./backend_b)
  - 目标侧 SWD 后端
- [`common`](./common)
  - 共用协议、运行时和 OTA 组件
- [`tools`](./tools)
  - Windows 下的辅助脚本
- [`USAGE.md`](./USAGE.md)
  - 更详细的构建和使用说明

## 默认网络配置

`backend_b` 默认开启热点，`frontend_a` 默认自动连接该热点。

| 项目 | 值 |
| --- | --- |
| SSID | `wireless-dap` |
| Password | `wirelessdap` |
| Backend IP | `192.168.4.1` |
| UDP Port | `3333` |
| OTA Portal | `http://192.168.4.1/` |

注意：

- 做 Web OTA 时，PC 也必须连接到这个热点。
- 如果没有密码，PC 无法连上热点，也就打不开 OTA 页面。

## 默认接线

### A / B 与 PC

- `frontend_a` 通过 ESP32-S3 原生 USB 连接 PC
- 当前测试环境下，A 端烧写口是 `COM3`
- `backend_b` 用于烧写或供电
- 当前测试环境下，B 端烧写口是 `COM5`

### B 端到目标板 SWD

`backend_b` 默认 GPIO 映射如下：

| backend_b GPIO | 目标板信号 |
| --- | --- |
| `GPIO5` | `SWCLK` |
| `GPIO4` | `SWDIO` |
| `GPIO6` | `nRESET` |
| `GND` | `GND` |

说明：

- 目标板必须与 `backend_b` 共地
- 当前实现按 `3.3V` 逻辑电平使用
- `nRESET` 为开漏驱动

### B 端 UART Bridge

`backend_b` 默认 UART 透传引脚如下：

| backend_b GPIO | 功能 |
| --- | --- |
| `GPIO17` | UART TX |
| `GPIO18` | UART RX |

默认波特率：`115200`

## 快速开始

### 1. 本地编译

Windows + PowerShell + ESP-IDF：

```powershell
C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe -ExecutionPolicy Bypass -NoProfile -Command "& { . 'D:\esp-idf\export.ps1'; Set-Location 'C:\Users\pan39\Desktop\workSpace\esp32Wireless-Dap\frontend_a'; idf.py build }"
```

```powershell
C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe -ExecutionPolicy Bypass -NoProfile -Command "& { . 'D:\esp-idf\export.ps1'; Set-Location 'C:\Users\pan39\Desktop\workSpace\esp32Wireless-Dap\backend_b'; idf.py build }"
```

### 2. 首次基线烧写

第一次使用 Web OTA 之前，必须先把带 OTA 能力的基线固件通过串口烧进去。

先烧 `backend_b`：

```powershell
C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe -ExecutionPolicy Bypass -NoProfile -Command "& { . 'D:\esp-idf\export.ps1'; Set-Location 'C:\Users\pan39\Desktop\workSpace\esp32Wireless-Dap\backend_b'; idf.py -p COM5 flash }"
```

再烧 `frontend_a`：

```powershell
C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe -ExecutionPolicy Bypass -NoProfile -Command "& { . 'D:\esp-idf\export.ps1'; Set-Location 'C:\Users\pan39\Desktop\workSpace\esp32Wireless-Dap\frontend_a'; idf.py -p COM3 flash }"
```

如果你的端口不是 `COM3` / `COM5`，请替换成实际端口。

### 3. 验证下载链路

Keil CLI 示例：

```powershell
C:\ARM-KEIL\UV4\UV4.exe -j0 -f C:\Users\pan39\Desktop\workSpace\dapTest\MDK-ARM\dapTest.uvprojx -t dapTest -o C:\Users\pan39\Desktop\workSpace\dapTest\MDK-ARM\flash_cli.log
```

成功日志通常包含：

- `Erase Done.`
- `Programming Done.`
- `Verify OK.`

OpenOCD 示例：

```powershell
D:\OpenOCD-20260302-0.12.0\bin\openocd.exe -s D:\OpenOCD-20260302-0.12.0\share\openocd\scripts -f interface/cmsis-dap.cfg -f target/stm32g0x.cfg -c "adapter serial 206EF1D601A8" -c "adapter speed 1000" -c "init" -c "halt" -c "mdw 0x08000000 12" -c "exit"
```

## Web OTA 用法

这是当前推荐的升级方式。

### 前提

- A、B 两端都已经刷入带 OTA 支持的基线固件
- `backend_b` 已正常上电并开启热点
- `frontend_a` 已自动连上 `backend_b`
- PC 已连接到热点：
  - SSID: `wireless-dap`
  - Password: `wirelessdap`

### 操作步骤

1. 让 PC 连接到 `wireless-dap`
2. 在浏览器打开 `http://192.168.4.1/`
3. 页面会自动探测：
   - `backend_b`，固定为 `192.168.4.1`
   - `frontend_a`，由 B 端自动发现并显示当前 IP
4. 对应选择本地 `.bin` 文件：
   - A 端选择 `wdap_frontend_a.bin`
   - B 端选择 `wdap_backend_b.bin`
5. 点击上传，等待设备写入、切换分区并重启

### OTA 行为说明

- 当前只支持“单端手动升级”，不做双端一键同时升级
- 上传文件格式固定为原始 app `.bin`
- 不通过网页升级 `bootloader`、`partition table` 或 `NVS`
- A 端升级后，页面不会掉
- B 端升级后，热点会短暂断开
  - 这是正常行为
  - 等 `backend_b` 重启完成后，重新连接 `wireless-dap`
  - 再次打开 `http://192.168.4.1/`

### OTA 成功后的预期

- 设备会从当前运行分区切换到新的 OTA 分区
- `frontend_a` / `backend_b` 的 `/api/info` 中 `running_partition` 会变化
- 设备版本和页面状态会自动更新

## GitHub Release 下载

如果不想本地编译，可以直接下载 Release 中的固件包：

1. 打开 [Latest Release](https://github.com/LIGHT1213/esp32Wireless-Dap/releases/latest)
2. 下载：
   - `esp32-wireless-dap-frontend_a-<版本>.zip`
   - `esp32-wireless-dap-backend_b-<版本>.zip`
3. 解压后使用压缩包内的 `FLASH_COMMAND.txt` 或 `flash_args.txt` 烧写

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

## 当前默认配置

当前仓库默认启用：

- `CMSIS-DAP v2`
- USB CDC UART Bridge
- 双 OTA 分区
- OTA rollback
- TinyUSB vendor FIFO `512 / 512`
- 默认 SWD 时钟 `1 MHz`
- SWD 最大时钟上限 `10 MHz`
- CPU 主频 `240 MHz`
- 更大的 Wi-Fi / LWIP buffer 配置
- 任务绑核，减少 Wi-Fi 与 SWD 的互相干扰
- backend SWD PHY 热路径 `gpio_ll + IRAM_ATTR`

## 已知说明

- 通用 `DAP_Transfer` batching 不能全开，否则会破坏 Keil 兼容性
- 当前仅保留安全子集 batching
- `Cannot enter Debug Mode` 如果出现，应视为单独的 debug attach 问题，不等同于下载失败
- B 端 OTA 后，热点通常会先恢复，HTTP 页面可能比热点稍晚可用几秒到几十秒

## 相关文档

- 使用说明：[`USAGE.md`](./USAGE.md)
- 原理图：[`ESP32-S3-SCH-V1.4.pdf`](./ESP32-S3-SCH-V1.4.pdf)
- GitHub Actions：[`/.github/workflows/firmware-release.yml`](./.github/workflows/firmware-release.yml)
