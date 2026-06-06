# ESP32-S3 Wireless CMSIS-DAP over ESP-NOW

Two ESP32-S3 boards implement a split CMSIS-DAP probe:

- `frontend_a`: USB device connected to PC, CMSIS-DAP v2 WinUSB vendor interface plus HID fallback.
- `backend_b`: ESP-NOW peer connected to the target MCU SWD pins, running the official DAPLink CMSIS-DAP command core.

## Fixed hardware

- A serial port: `COM3`
- B serial port: `COM5`
- A STA/peer MAC: `20:6e:f1:d6:01:a8`
- B STA/peer MAC: `20:6e:f1:d6:02:d0`
- ESP-NOW channel: `6`
- B GPIO5: SWCLK
- B GPIO4: SWDIO
- B GPIO6: nRESET open-drain/simulated open-drain
- B GPIO17: UART TX, GPIO18: UART RX, 115200 baud

## Build and flash

Run PowerShell with process-level execution policy if needed:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\tools\build_all.ps1
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\tools\flash_b.ps1
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\tools\flash_a.ps1
```

Monitor logs:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\tools\monitor_b.ps1
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\tools\monitor_a.ps1
```

Keil flash test:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\tools\keil_flash_test.ps1
```

Success requires `Erase Done.`, `Programming Done.`, and `Verify OK.` in `logs\keil_flash_test.log`.

## DAPLink port boundary

The backend compiles official DAPLink `DAP.c` and `SW_DP.c`. The ESP32-S3 port provides `DAP_config.h`, GPIO control, timestamp, strings, and board info. JTAG, SWO, and DAP UART commands are disabled in v1.
