$ErrorActionPreference = 'Stop'
. 'D:\esp-idf\export.ps1'
$Root = Split-Path -Parent $PSScriptRoot
Push-Location (Join-Path $Root 'frontend_a')
idf.py -p COM3 flash
Pop-Location
