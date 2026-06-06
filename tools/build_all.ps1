$ErrorActionPreference = 'Stop'
. 'D:\esp-idf\export.ps1'
$Root = Split-Path -Parent $PSScriptRoot
Push-Location (Join-Path $Root 'backend_b')
idf.py set-target esp32s3
idf.py build
Pop-Location
Push-Location (Join-Path $Root 'frontend_a')
idf.py set-target esp32s3
idf.py build
Pop-Location
