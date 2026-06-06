$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
$LogDir = Join-Path $Root 'logs'
New-Item -ItemType Directory -Force -Path $LogDir | Out-Null
$ProjectRoot = 'C:\Users\pan39\Desktop\workSpace\DapTestH7'
$Uv4 = 'C:\ARM-KEIL\UV4\uvision.com'
if (!(Test-Path -LiteralPath $Uv4)) { $Uv4 = 'C:\ARM-KEIL\UV4\UV4.exe' }
$Projects = Get-ChildItem -LiteralPath $ProjectRoot -Recurse -Filter *.uvprojx
if ($Projects.Count -eq 0) { throw "No .uvprojx found under $ProjectRoot" }
$Project = $Projects | Select-Object -First 1
[xml]$Xml = Get-Content -LiteralPath $Project.FullName
$Target = ($Xml.Project.Targets.Target | Select-Object -First 1).TargetName
$Log = Join-Path $LogDir 'keil_flash_test.log'
& $Uv4 -f $Project.FullName -o $Log -t $Target
$Exit = $LASTEXITCODE
python (Join-Path $PSScriptRoot 'parse_keil_log.py') $Log
if ($LASTEXITCODE -ne 0 -or $Exit -ne 0) { exit 1 }
