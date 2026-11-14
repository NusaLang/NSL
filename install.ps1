$ErrorActionPreference = "Stop"

$installDir = if ($env:NusaInstallDir) { $env:NusaInstallDir } else { "$env:LOCALAPPDATA\nusa" }
New-Item -ItemType Directory -Force -Path $installDir | Out-Null

Write-Host "Nusantara -- Windows (PowerShell)"
Write-Host "Ambil bundle rilis terbaru ..."

$tarGz = "$env:TEMP\nusa-windows-x86_64.tar.gz"
$url = "https://github.com/NusaLang/NSL/releases/latest/download/nusa-windows-x86_64.tar.gz"
Invoke-WebRequest -Uri $url -OutFile $tarGz

Write-Host "Ekstrak bundle ..."
tar -xzf $tarGz -C $installDir
Remove-Item -Force $tarGz 2>$null

$userPath = [Environment]::GetEnvironmentVariable("Path", "User")
if ($userPath -notlike "*$installDir*") {
    [Environment]::SetEnvironmentVariable("Path", "$userPath;$installDir", "User")
    Write-Host "  PATH ditambahkan ke environment User."
}

Write-Host ""
Write-Host "Terpasang di $installDir."
Write-Host "Buka terminal baru (PowerShell / Command Prompt), terus:"
Write-Host "  nusa run examples/hello.ns"
