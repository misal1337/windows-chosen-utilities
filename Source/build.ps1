param([switch]$Run)
$ErrorActionPreference = 'Stop'
# portable Zig in ~\wcu-tools if it's there, otherwise zig on PATH
$zig = "$HOME\wcu-tools\zig\zig.exe"
if (-not (Test-Path $zig)) { $zig = (Get-Command zig -ErrorAction Stop).Source }
Set-Location $PSScriptRoot
& "$PSScriptRoot\lang.ps1"
$out = 'Windows Chosen Utilities.exe'
# baseline: without it Zig builds for the CPU doing the build (AVX2 and newer), and the exe crashes at start on PCs without those
$args = @('cc','app.c','app.rc','-o',$out,'-O2','-municode','-mcpu=baseline',
  '-ldwmapi','-lgdi32','-luser32','-lshell32','-lole32','-lshlwapi','-lcomdlg32','-ladvapi32','-loleaut32','-luuid','-lvirtdisk','-ld2d1','-ldwrite','-lwinhttp','-lwintrust','-lcrypt32',
  '-Wl,/subsystem:windows')
& $zig @args 2>&1 | ForEach-Object { "$_" }
if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE)" }
Copy-Item $out .. -Force      # the folder above holds the copy to run
"built: $out  $([math]::Round((Get-Item $out).Length/1KB,1)) KB"
if ($Run) { Start-Process ".\$out" }
