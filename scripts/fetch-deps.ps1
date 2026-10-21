# Downloads the pinned build dependencies into .deps/ and verifies their hashes.
# Nothing is installed system-wide.

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$deps = Join-Path $root '.deps'
$downloads = Join-Path $deps 'dl'
New-Item -ItemType Directory -Force $downloads | Out-Null

$sdkVersion = '10.0.28000.2526'
$packages = @(
    @{ Name = "microsoft.windows.sdk.cpp.$sdkVersion"; Kind = 'zip'
       Url = "https://api.nuget.org/v3-flatcontainer/microsoft.windows.sdk.cpp/$sdkVersion/microsoft.windows.sdk.cpp.$sdkVersion.nupkg"
       Sha256 = 'be1b419491607eae6f7c57844ebab39face9643c51e2af1d9176a3ba0d0b23fc' },
    @{ Name = "microsoft.windows.sdk.cpp.x64.$sdkVersion"; Kind = 'zip'
       Url = "https://api.nuget.org/v3-flatcontainer/microsoft.windows.sdk.cpp.x64/$sdkVersion/microsoft.windows.sdk.cpp.x64.$sdkVersion.nupkg"
       Sha256 = 'a9cae2a8c5da7f5dc5838ae6a76d06d0d2e2fdc3d8cfc69ca6c184e4b9193a00' },
    @{ Name = "microsoft.windows.sdk.buildtools.$sdkVersion"; Kind = 'zip'
       Url = "https://api.nuget.org/v3-flatcontainer/microsoft.windows.sdk.buildtools/$sdkVersion/microsoft.windows.sdk.buildtools.$sdkVersion.nupkg"
       Sha256 = 'a09a4c9d68160ced4765137a9a7444ea560ea86c45d6a77093dea58c2f7563a0' },
    @{ Name = 'libwebp-1.6.0'; Kind = 'tar'
       Url = 'https://github.com/webmproject/libwebp/archive/refs/tags/v1.6.0.tar.gz'
       Sha256 = '93a852c2b3efafee3723efd4636de855b46f9fe1efddd607e1f42f60fc8f2136' }
)

foreach ($package in $packages) {
    $target = Join-Path $deps $package.Name
    if (Test-Path $target) { continue }

    $archive = Join-Path $downloads ([IO.Path]::GetFileName($package.Url))
    if (-not (Test-Path $archive)) {
        Write-Host "Downloading $($package.Name)"
        $ProgressPreference = 'SilentlyContinue'
        Invoke-WebRequest -Uri $package.Url -OutFile $archive -UseBasicParsing
    }
    $hash = (Get-FileHash -Algorithm SHA256 $archive).Hash.ToLowerInvariant()
    if ($hash -ne $package.Sha256) {
        Remove-Item $archive
        throw "Hash mismatch for $($package.Name): $hash"
    }

    if ($package.Kind -eq 'zip') {
        Add-Type -AssemblyName System.IO.Compression.FileSystem
        [IO.Compression.ZipFile]::ExtractToDirectory($archive, $target)
    } else {
        tar -xzf $archive -C $deps
        if ($LASTEXITCODE -ne 0) { throw "Could not extract $archive" }
    }
}
