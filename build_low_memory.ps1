$env:CMAKE_BUILD_PARALLEL_LEVEL = "1"
$env:NINJAFLAGS = "-j1"
$tmpDir = Join-Path $PSScriptRoot ".tmp"
New-Item -ItemType Directory -Force -Path $tmpDir | Out-Null
$env:TMP = $tmpDir
$env:TEMP = $tmpDir
idf.py build
