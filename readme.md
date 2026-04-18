# ESP32 Wireless DAP

## 简介

这是一个双端 ESP32-S3 的无线 CMSIS-DAP 工程。

- `frontend_a`
  - 通过原生 USB 连接 PC
  - 对 PC 暴露 `CMSIS-DAP v2`
  - 通过 Wi-Fi 把调试请求转发给后端
- `backend_b`
  - 通过 SWD 连接目标板
  - 执行实际的 SWD 读写、复位、下载相关操作
  - 通过 Wi-Fi 向前端返回结果

整体链路：

`PC <-> frontend_a <-> Wi-Fi <-> backend_b <-> Target`

## 当前状态

当前仓库已经可以在 Windows 环境下完成以下工作：

- A 端以 `CMSIS-DAP v2` 方式工作
- B 端通过 SWD 驱动目标板
- 使用 `Keil CLI` 进行原生下载，不依赖 Keil GUI
- 使用 `OpenOCD` 做基础连通性验证
- 默认使用双 ESP32-S3 + Wi-Fi + SWD 的工作模式

已验证下载链路可以得到以下 Keil CLI 结果：

- `Erase Done.`
- `Programming Done.`
- `Verify OK.`

## 工程结构

- [`frontend_a`](./frontend_a)
  - PC 侧 USB CMSIS-DAP 前端
- [`backend_b`](./backend_b)
  - 目标侧 SWD 执行后端
- [`common`](./common)
  - 前后端共用协议和工具代码
- [`tools`](./tools)
  - Windows 下的辅助脚本
- [`USAGE.md`](./USAGE.md)
  - 详细使用文档

## 默认网络配置

当前默认配置如下：

- SSID: `wireless-dap`
- Password: `wirelessdap`
- Backend IP: `192.168.4.1`
- UDP port: `3333`

## 默认 SWD 接线

`backend_b` 到目标板的默认 GPIO 映射：

- `GPIO5` -> `SWCLK`
- `GPIO4` -> `SWDIO`
- `GPIO6` -> `nRESET`
- `GND` -> `GND`

注意：

- 目标板必须与 `backend_b` 共地
- 当前实现按 `3.3V` 逻辑电平使用
- `nRESET` 为开漏驱动

## 构建与烧写

详细命令请看 [`USAGE.md`](./USAGE.md)。

Windows 下典型流程：

1. 编译 `frontend_a`
2. 编译 `backend_b`
3. 烧写 `backend_b` 到 `COM5`
4. 烧写 `frontend_a` 到 `COM3`
5. 用 `Keil CLI` 或 `OpenOCD` 验证

## GitHub Actions 与 Release

仓库现在包含自动构建与发布 workflow：

- Push 到 `main` 时，GitHub Actions 会编译 `frontend_a` 和 `backend_b`，并上传 workflow artifact
- 提交 Pull Request 时，会执行同样的编译检查
- Push `v*` 或 `V*` 标签时，例如 `v1.0.0` 或 `V0.1.0`，会自动创建或更新 GitHub Release，并上传 A/B 两端固件包
- 手动触发 workflow 时，如果填写 `release_tag`，也会创建或更新对应 Release

相关页面：

- [Actions](https://github.com/LIGHT1213/esp32Wireless-Dap/actions)
- [Releases](https://github.com/LIGHT1213/esp32Wireless-Dap/releases)
- [Latest Release](https://github.com/LIGHT1213/esp32Wireless-Dap/releases/latest)

## 如何下载 BIN

如果你不想本地编译，可以直接从 GitHub Release 下载现成固件：

1. 打开 [Latest Release](https://github.com/LIGHT1213/esp32Wireless-Dap/releases/latest)
2. 下载 `esp32-wireless-dap-frontend_a-<版本>.zip`
3. 下载 `esp32-wireless-dap-backend_b-<版本>.zip`
4. 解压后使用压缩包内的 `FLASH_COMMAND.txt` 或 `flash_args.txt` 烧写

每个压缩包都包含：

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

当前测试环境中：

- A 端通常是 `COM3`
- B 端通常是 `COM5`

## 性能相关

当前仓库中的性能优化已包含：

- TinyUSB vendor FIFO `512/512`
- 默认 SWD 时钟 `1 MHz`
- CPU 主频 `240 MHz`
- Release/Performance 编译优化
- 更大的 Wi-Fi / LWIP buffer 配置
- 任务绑核，减少 Wi-Fi 与 SWD 互相干扰
- backend SWD PHY 热路径使用 `gpio_ll + IRAM_ATTR`

## 已知说明

- 通用 `DAP_Transfer` batching 不能全开，否则会破坏 Keil 兼容性
- 当前只保留了安全子集的 batching
- 当前下载链路已验证正常
- `Cannot enter Debug Mode` 如果出现，应视为单独的 debug attach 问题，不等同于下载失败

## 文档

- 使用文档：[`USAGE.md`](./USAGE.md)
- 原始原理图：[`ESP32-S3-SCH-V1.4.pdf`](./ESP32-S3-SCH-V1.4.pdf)
