# engine build into web/: mural.js, mural.wasm, mural.data
# usage: powershell -NoProfile -ExecutionPolicy Bypass -File engine/build.ps1
# emsdk: the folder in EMSDK, else emcc already on PATH, else emsdk in the home folder

# block textures are not in the repo -- they are the game files (client jar)
# copy them into engine/data/block_textures before building, the csvs name the files the engine looks for
$ErrorActionPreference = 'Continue'
$textures = Join-Path $PSScriptRoot 'data\block_textures'
if (-not (Get-ChildItem -Path $textures -Filter *.png -ErrorAction SilentlyContinue | Select-Object -First 1)) {
    Write-Output "No block textures in $textures -- they are not in the repo, copy them in from the game first."
    exit 1
}

# inside web/, so mural.js asks for mural.data by its bare name
Set-Location (Join-Path $PSScriptRoot '..\web')
$emsdk = if ($env:EMSDK) { $env:EMSDK } else { Join-Path $HOME 'emsdk' }
if (-not (Get-Command emcc -ErrorAction SilentlyContinue)) {
    if (-not (Test-Path (Join-Path $emsdk 'emsdk_env.ps1'))) {
        Write-Output "No emsdk found -- set EMSDK to its folder or put emcc on PATH."
        exit 1
    }
    & (Join-Path $emsdk 'emsdk_env.ps1') | Out-Null
}

# the prefix map keeps the miniz path baked into the wasm as it was, the data keeps the resources/ paths the engine opens
$src = '../engine/src'
emcc "$src/main.cpp" "$src/engine.cpp" "$src/palette.cpp" "$src/layout.cpp" "$src/solver.cpp" "$src/exports.cpp" "$src/bridge.cpp" "$src/enhance.cpp" "$src/railbuild.cpp" ../engine/third_party/miniz.c -o mural.js `
  '-ffile-prefix-map=../engine/=' -O3 -pthread -msimd128 `
  '-sPTHREAD_POOL_SIZE=17' `
  '-sWASM=1' `
  '-sALLOW_MEMORY_GROWTH=1' `
  '-sINITIAL_MEMORY=536870912' `
  '-sMAXIMUM_MEMORY=4294967296' `
  '-sEXIT_RUNTIME=0' `
  '-sMODULARIZE=1' `
  '-sEXPORT_NAME=createMuralModule' `
  '-sEXPORTED_FUNCTIONS=[_main,_malloc,_free]' `
  '-sEXPORTED_RUNTIME_METHODS=[ccall,cwrap,UTF8ToString,HEAPU8,HEAP32,PThread]' `
  --preload-file ../engine/data/block_textures@resources/block_textures `
  --preload-file ../engine/data/allowed_ids.csv@resources/allowed_ids.csv `
  --preload-file ../engine/data/overlay_ids.csv@resources/overlay_ids.csv `
  --preload-file ../engine/data/overlay_states.csv@resources/overlay_states.csv `
  --preload-file ../engine/data/version_renames.csv@resources/version_renames.csv `
  --preload-file ../engine/data/block_faces.csv@resources/block_faces.csv
Write-Output "emcc exit code: $LASTEXITCODE"
Get-Item mural.js, mural.wasm, mural.data | Select-Object Name, Length, LastWriteTime | Format-Table -AutoSize
