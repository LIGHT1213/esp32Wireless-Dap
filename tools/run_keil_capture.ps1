$pythonExe = "C:\Users\pan39\.espressif\python_env\idf5.4_py3.14_env\Scripts\python.exe"
$captureScript = "C:\Users\pan39\Desktop\workSpace\esp32Wireless-Dap\tools\capture_serial.py"
$outputDir = "C:\Users\pan39\Desktop\workSpace\esp32Wireless-Dap\logs"
$keilProject = "C:\Users\pan39\Desktop\workSpace\dapTest\MDK-ARM\dapTest.uvprojx"
$keilLog = "C:\Users\pan39\Desktop\workSpace\dapTest\MDK-ARM\flash_cli_current.log"

New-Item -ItemType Directory -Force -Path $outputDir | Out-Null

$aLog = Join-Path $outputDir "keil_a_com3.log"
$bLog = Join-Path $outputDir "keil_b_com5.log"
Remove-Item -Force -ErrorAction SilentlyContinue $aLog, $bLog

$capA = Start-Process -FilePath $pythonExe -ArgumentList @($captureScript, "COM3", "300", $aLog) -PassThru -WindowStyle Hidden
$capB = Start-Process -FilePath $pythonExe -ArgumentList @($captureScript, "COM5", "300", $bLog) -PassThru -WindowStyle Hidden

Start-Sleep -Seconds 1

try {
    & "C:\ARM-KEIL\UV4\UV4.exe" -j0 -f $keilProject -t "dapTest" -o $keilLog
    $keilExit = $LASTEXITCODE
    Start-Sleep -Seconds 1
} finally {
    foreach ($proc in @($capA, $capB)) {
        if ($null -ne $proc -and -not $proc.HasExited) {
            Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
        }
    }
}

exit $keilExit
