# ESP32 Wireless DAP Usage Guide

## 1. Overview

This project uses two ESP32-S3 boards:

- `frontend_a`
  - Connects to the PC over native USB
  - Exposes `CMSIS-DAP v2`
  - Connects to backend over Wi-Fi
- `backend_b`
  - Connects to the target board over SWD
  - Executes SWD operations
  - Works as Wi-Fi SoftAP server

Current default network configuration:

- SSID: `wireless-dap`
- Password: `wirelessdap`
- Backend IP: `192.168.4.1`
- UDP port: `3333`

## 2. Wiring

### 2.1 PC and A/B boards

- Connect `frontend_a` to the PC using the ESP32-S3 native USB port.
- In the current Windows environment, the A-side flash port is `COM3`.
- Connect `backend_b` to the PC for flashing or power.
- In the current Windows environment, the B-side flash port is `COM5`.
- During normal use, A and B communicate over Wi-Fi. No extra signal wires are required between A and B.

### 2.2 Backend to target board

`backend_b` drives the target SWD pins with the following GPIO mapping:

- `GPIO5` -> target `SWCLK`
- `GPIO4` -> target `SWDIO`
- `GPIO6` -> target `nRESET`
- `GND` -> target `GND`

Important notes:

- The target board and `backend_b` must share ground.
- The current implementation assumes `3.3V` SWD logic.
- If the target is not `3.3V`, use proper level shifting.
- `nRESET` is driven as open-drain on the backend side.
- Power the target board correctly. This project does not currently implement VTref sensing.

## 3. Toolchain

Windows paths used in this workspace:

- Keil: `C:\ARM-KEIL`
- ESP-IDF: `D:\esp-idf`
- Workspace: `C:\Users\pan39\Desktop\workSpace\esp32Wireless-Dap`

Recommended shell:

- `PowerShell`

## 4. Build

### 4.1 Build frontend A

```powershell
C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe -ExecutionPolicy Bypass -NoProfile -Command "& { . 'D:\esp-idf\export.ps1'; Set-Location 'C:\Users\pan39\Desktop\workSpace\esp32Wireless-Dap\frontend_a'; idf.py build }"
```

### 4.2 Build backend B

```powershell
C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe -ExecutionPolicy Bypass -NoProfile -Command "& { . 'D:\esp-idf\export.ps1'; Set-Location 'C:\Users\pan39\Desktop\workSpace\esp32Wireless-Dap\backend_b'; idf.py build }"
```

## 5. Flash

### 5.1 Flash backend B to COM5

```powershell
C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe -ExecutionPolicy Bypass -NoProfile -Command "& { . 'D:\esp-idf\export.ps1'; Set-Location 'C:\Users\pan39\Desktop\workSpace\esp32Wireless-Dap\backend_b'; idf.py -p COM5 flash }"
```

### 5.2 Flash frontend A to COM3

```powershell
C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe -ExecutionPolicy Bypass -NoProfile -Command "& { . 'D:\esp-idf\export.ps1'; Set-Location 'C:\Users\pan39\Desktop\workSpace\esp32Wireless-Dap\frontend_a'; idf.py -p COM3 flash }"
```

If your ports are different, replace `COM3` and `COM5` with the actual ports.

## 6. Keil CLI Verification

This project is intended to work without opening the Keil GUI.

Example Keil CLI download command:

```powershell
C:\ARM-KEIL\UV4\UV4.exe -j0 -f C:\Users\pan39\Desktop\workSpace\dapTest\MDK-ARM\dapTest.uvprojx -t dapTest -o C:\Users\pan39\Desktop\workSpace\dapTest\MDK-ARM\flash_cli.log
```

Expected successful output contains:

- `Erase Done.`
- `Programming Done.`
- `Verify OK.`

## 7. Optional OpenOCD Verification

You can also verify basic access with OpenOCD:

```powershell
D:\OpenOCD-20260302-0.12.0\bin\openocd.exe -s D:\OpenOCD-20260302-0.12.0\share\openocd\scripts -f interface/cmsis-dap.cfg -f target/stm32g0x.cfg -c "adapter serial 206EF1D601A8" -c "adapter speed 1000" -c "init" -c "halt" -c "mdw 0x08000000 12" -c "exit"
```

## 8. Current Performance-Relevant Configuration

The current checked-in build uses:

- `CMSIS-DAP v2`
- TinyUSB vendor RX/TX buffer size `512`
- default SWD clock `1000000 Hz`
- CPU frequency `240 MHz`
- release/performance optimization
- larger Wi-Fi and LWIP mailbox/buffer settings
- task pinning to reduce Wi-Fi/SWD interference
- low-overhead `gpio_ll` hot path in backend SWD PHY

## 9. Common Notes

- `DAP_Transfer` generic batching is not globally enabled because it breaks Keil compatibility in some sequences.
- A safe subset batching path is kept in the frontend for compatible traffic patterns.
- The Keil flash path is validated through CLI download, not through the GUI.
- If the probe can download but cannot enter debug mode, treat that as a separate debug-attach problem, not a flash problem.
