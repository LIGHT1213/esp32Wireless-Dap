$ErrorActionPreference = 'Stop'
. 'D:\esp-idf\export.ps1'
$Root = Split-Path -Parent $PSScriptRoot
Push-Location (Join-Path $Root 'backend_b')
idf.py -p COM5 monitor
Pop-Location
