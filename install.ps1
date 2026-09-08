$ErrorActionPreference = "Stop"

$installDir = if ($env:NUSA_INSTALL_DIR) { $env:NUSA_INSTALL_DIR } else { "$env:LOCALAPPDATA\nusa" }
New-Item -ItemType Directory -Force -Path $installDir | Out-Null

Write-Host "Nusantara -- Windows (PowerShell)"
Write-Host "Ambil binary jadi ..."
Invoke-WebRequest -Uri "https://github.com/NusaLang/NSL/releases/latest/download/nusa-windows-x86_64.exe" `
    -OutFile "$installDir\nusa.exe"

$userPath = [Environment]::GetEnvironmentVariable("Path", "User")
if ($userPath -notlike "*$installDir*") {
    [Environment]::SetEnvironmentVariable("Path", "$userPath;$installDir", "User")
    Write-Host "  PATH ditambahin -- buka terminal baru biar kepakai."
}

Write-Host ""
Write-Host "Terpasang di $installDir\nusa.exe."
Write-Host "Buka terminal baru, terus:"
Write-Host "  nusa run examples/hello.ns"
