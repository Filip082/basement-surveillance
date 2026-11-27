# Build the firmware in the official ESP-IDF image from PowerShell (Docker Desktop).
# Usage: .\scripts\docker-build.ps1 [idf.py args...]     default: build
param([Parameter(ValueFromRemainingArguments = $true)] [string[]] $IdfArgs)

$image = if ($env:IDF_IMAGE) { $env:IDF_IMAGE } else { "espressif/idf:v6.1" }
if (-not $IdfArgs -or $IdfArgs.Count -eq 0) { $IdfArgs = @("build") }
$project = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path

docker run --rm -it -e HOME=/tmp -v "${project}:/project" -w /project $image idf.py @IdfArgs
