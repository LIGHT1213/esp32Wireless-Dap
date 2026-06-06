# 测试计划

## 阶段 0：环境与资料确认

- Windows 检查：确认当前系统为 Windows。
- ESP-IDF 检查：确认 `D:\esp-idf\export.ps1` 存在，使用进程级 `-ExecutionPolicy Bypass` 加载并执行 `idf.py --version`。
- Keil 检查：通过 Keil Skill `env` 检测 `C:\ARM-KEIL\UV4\UV4.exe`。
- 串口检查：确认 `COM3`、`COM5` 存在。
- MAC 读取：用 esptool 读取 COM3/COM5 芯片 MAC，并在首版固件启动日志中二次确认 STA MAC。
- Keil 工程检查：查找 `.uvprojx` 并解析 TargetName。
- DAPLink 检查：只读分析官方仓库，确认核心入口和许可证。

## 阶段 1：静态构建测试

- 执行 `tools\build_all.ps1`。
- 构建 `backend_b`：必须通过 `idf.py build`，目标为 `esp32s3`。
- 构建 `frontend_a`：必须通过 `idf.py build`，目标为 `esp32s3`。
- 构建失败时记录 CMake/编译/链接错误分类到 `docs\IMPLEMENTATION_LOG.md`。

## 阶段 2：烧写测试

- 先执行 `tools\flash_b.ps1`，使用 `COM5` 烧写 B 端。
- 再执行 `tools\flash_a.ps1`，使用 `COM3` 烧写 A 端。
- 烧写失败时检查端口占用、下载模式、USB 线、驱动和波特率。

## 阶段 3：启动日志测试

B 端 `COM5` monitor 应确认：

- Wi-Fi STA init ok。
- 打印 B 端 STA MAC，期望 `20:6e:f1:d6:02:d0`。
- ESP-NOW init ok。
- peer MAC 为 A 端 `20:6e:f1:d6:01:a8`。
- `esp_now_add_peer` ok。
- SWD GPIO init ok，GPIO5/GPIO4/GPIO6 配置正确。
- UART GPIO17/GPIO18 配置或预留正确。

A 端 `COM3` monitor 应确认：

- Wi-Fi STA init ok。
- 打印 A 端 STA MAC，期望 `20:6e:f1:d6:01:a8`。
- ESP-NOW init ok。
- peer MAC 为 B 端 `20:6e:f1:d6:02:d0`。
- `esp_now_add_peer` ok。
- USB TinyUSB mounted/configured。

## 阶段 4：ESP-NOW 链路测试

- A 端周期发送 `WDAP_PKT_PING`。
- B 端返回 `WDAP_PKT_PONG`。
- 连续运行至少 5 分钟。
- 统计发送、接收、ACK、NACK、timeout、retry、CRC error、duplicate、reassembly error。
- 最低验收：无持续 timeout，无 CRC/reassembly 连续错误，PING/PONG 稳定。

## 阶段 5：USB CMSIS-DAP 枚举测试

- 插入 A 端，Windows 设备管理器出现 CMSIS-DAP/WinUSB/HID 相关设备。
- Keil 能看到 CMSIS-DAP 设备。
- 如果失败，按 VID/PID、Interface string、MS OS descriptor、endpoint、DAP_Info 顺序排查。

## 阶段 6：DAP_Info 与基础命令测试

- PC 发送 `DAP_Info`，A 端经 ESP-NOW 转发到 B 端。
- B 端调用官方 `DAP_ProcessCommand()`。
- A 端返回 response。
- 验证 Vendor/Product/Serial、Packet Size、Packet Count、Capabilities。
- 验证 `DAP_Connect`、`DAP_Disconnect`、`DAP_SWJ_Clock`、`DAP_SWJ_Pins`、`DAP_SWJ_Sequence` 基础路径。

## 阶段 7：Keil 下载测试

- 执行 `tools\keil_flash_test.ps1`。
- 工程路径：`C:\Users\pan39\Desktop\workSpace\DapTestH7`。
- `.uvprojx`：`C:\Users\pan39\Desktop\workSpace\DapTestH7\MDK-ARM\DapTestH7.uvprojx`。
- TargetName：`DapTestH7`。
- 日志输出：`logs\keil_flash_test.log`。
- 成功判据：日志同时包含 `Erase Done.`、`Programming Done.`、`Verify OK.`。
- 失败分类：自动提取 `Cannot enter Debug Mode`、`No Cortex-M Device found`、`SWD/JTAG Communication Failure`、`Flash Download failed`、`CMSIS-DAP not found`、Access Port / Debug Port 错误。

## 完整验收测试

- 连续执行 Keil 下载至少 5 次。
- 每次日志均包含 `Erase Done.`、`Programming Done.`、`Verify OK.`。
- 下载过程中 A/B 不死机、不重启。
- ESP-NOW 统计中 timeout/retry 比例可接受。
- B 端 SWD GPIO 无方向切换错误或 nRESET 异常。
