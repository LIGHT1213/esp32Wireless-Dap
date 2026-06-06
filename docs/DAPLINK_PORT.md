# DAPLink 移植说明

- 官方源码来源：`third_party/DAPLink`，Apache-2.0，当前引入提交 `8f4e9e4`。
- B 端编译官方 `source/daplink/cmsis-dap/DAP.c` 和 `SW_DP.c`。
- DAP 命令处理入口为官方 `DAP_ExecuteCommand()` / `DAP_ProcessCommand()`。
- ESP32-S3 只实现 `DAP_config.h` 端口层：SWCLK/SWDIO/nRESET、timestamp、DAP strings、LED 空实现和 reset hook。
- 首版 `DAP_SWD=1`，`DAP_JTAG=0`，`SWO_UART=0`，`SWO_MANCHESTER=0`，`DAP_UART=0`。
