$code = @"
using System;
using System.Runtime.InteropServices;
public static class D {
  [DllImport("C:\\ARM-KEIL\\ARM\\BIN\\CMSIS_DAP.dll", CallingConvention=CallingConvention.Cdecl)]
  public static extern int CMSIS_DAP_GetNumberOfDevices();

  [DllImport("C:\\ARM-KEIL\\ARM\\BIN\\CMSIS_DAP.dll", CallingConvention=CallingConvention.Cdecl)]
  public static extern int CMSIS_DAP_DetectNumberOfDevices();
}
"@

Add-Type -TypeDefinition $code

Write-Output ("Get=" + [D]::CMSIS_DAP_GetNumberOfDevices())
Write-Output ("Detect=" + [D]::CMSIS_DAP_DetectNumberOfDevices())
