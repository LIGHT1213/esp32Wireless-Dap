$ErrorActionPreference = 'Stop'
. 'D:\esp-idf\export.ps1'
$Root = Split-Path -Parent $PSScriptRoot
$LogDir = Join-Path $Root 'logs'
New-Item -ItemType Directory -Force -Path $LogDir | Out-Null
python -m esptool --chip esp32s3 -p COM5 read_mac *>&1 | Tee-Object -FilePath (Join-Path $LogDir 'mac_b.txt')
