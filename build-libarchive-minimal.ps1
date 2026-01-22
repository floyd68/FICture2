# libarchive Minimal Build Script for FICture2
# Builds static library with ZIP, 7-Zip, and RAR support only

$ErrorActionPreference = "Stop"

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$sourceDir = Join-Path $scriptDir "external\libarchive"
$buildDir = Join-Path $sourceDir "build-minimal"

Write-Host "=== libarchive Minimal Build ===" -ForegroundColor Cyan
Write-Host "Source: $sourceDir" -ForegroundColor Gray
Write-Host "Build:  $buildDir" -ForegroundColor Gray
Write-Host ""

# Create build directory
if (Test-Path $buildDir) {
    Write-Host "Cleaning existing build directory..." -ForegroundColor Yellow
    Remove-Item $buildDir -Recurse -Force
}
New-Item -ItemType Directory -Path $buildDir | Out-Null

# Configure with CMake
Write-Host "Configuring CMake..." -ForegroundColor Green
Push-Location $buildDir

try {
    # Note: FICture2 uses PlatformToolset v145 (Visual Studio 2026)
    # CMake generator "Visual Studio 18 2026" corresponds to v145 toolset
    
    # Use vcpkg toolchain file to find zlib and liblzma
    $vcpkgRoot = "D:\Works\vcpkg"
    $toolchainFile = Join-Path $vcpkgRoot "scripts\buildsystems\vcpkg.cmake"
    
    cmake .. `
        -G "Visual Studio 18 2026" `
        -A x64 `
        -DCMAKE_TOOLCHAIN_FILE="$toolchainFile" `
        -DVCPKG_TARGET_TRIPLET=x64-windows-static `
        -DCMAKE_BUILD_TYPE=Release `
        -DBUILD_SHARED_LIBS=OFF `
        -DENABLE_ZLIB=ON `
        -DENABLE_LZMA=ON `
        -DENABLE_BZip2=OFF `
        -DENABLE_ZSTD=OFF `
        -DENABLE_LZ4=OFF `
        -DENABLE_LZO=OFF `
        -DENABLE_OPENSSL=OFF `
        -DENABLE_TAR=OFF `
        -DENABLE_CPIO=OFF `
        -DENABLE_CAT=OFF `
        -DENABLE_TEST=OFF `
        -DENABLE_INSTALL=OFF `
        -DENABLE_ACL=OFF `
        -DENABLE_XATTR=OFF

    if ($LASTEXITCODE -ne 0) {
        throw "CMake configuration failed"
    }

    Write-Host ""
    Write-Host "Building Release configuration..." -ForegroundColor Green
    cmake --build . --config Release

    if ($LASTEXITCODE -ne 0) {
        throw "Release build failed"
    }

    Write-Host ""
    Write-Host "Building Debug configuration..." -ForegroundColor Green
    cmake --build . --config Debug

    if ($LASTEXITCODE -ne 0) {
        throw "Debug build failed"
    }

    Write-Host ""
    Write-Host "=== Build Successful ===" -ForegroundColor Green
    Write-Host ""

    # Show library sizes
    foreach ($config in @("Release", "Debug")) {
        $libPath = Join-Path $buildDir "libarchive\$config\archive.lib"
        if (Test-Path $libPath) {
            $libInfo = Get-Item $libPath
            $sizeKB = [math]::Round($libInfo.Length / 1KB, 0)
            $sizeMB = [math]::Round($libInfo.Length / 1MB, 2)
            Write-Host "$config Library: $libPath" -ForegroundColor Cyan
            Write-Host "Size:           $sizeKB KB ($sizeMB MB)" -ForegroundColor Cyan
        } else {
            Write-Host "Warning: $config archive.lib not found at expected location" -ForegroundColor Yellow
        }
    }
    
    Write-Host ""
    Write-Host "Searching for all library files..." -ForegroundColor Yellow
    Get-ChildItem -Path $buildDir -Filter "*.lib" -Recurse | ForEach-Object {
        Write-Host "  Found: $($_.FullName)" -ForegroundColor Gray
    }
}
finally {
    Pop-Location
}

Write-Host ""
Write-Host "Done!" -ForegroundColor Green
