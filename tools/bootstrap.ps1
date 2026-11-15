# Fetch the build tools this port needs, into .\tools, once.
# Nothing here is redistributed: each piece is downloaded from its own
# upstream on the player's machine, so the release stays small and no
# third-party licence travels with it.
#
#   powershell -ExecutionPolicy Bypass -File tools\bootstrap.ps1
param([string]$Root = (Split-Path -Parent $PSScriptRoot))
if ($Root) { $Root = $Root.Trim('"').TrimEnd('\') }
if (-not $Root) { $Root = Split-Path -Parent $PSScriptRoot }
$ErrorActionPreference = 'Stop'
$ProgressPreference    = 'SilentlyContinue'   # the progress bar makes downloads ~10x slower
$Tools = Join-Path $Root 'tools'
New-Item -ItemType Directory -Force -Path $Tools | Out-Null

function Say($m) { Write-Host "   $m" }

function Get-File($Url, $Dest) {
  Say "downloading $(Split-Path $Dest -Leaf)"
  try { Invoke-WebRequest -Uri $Url -OutFile $Dest -UseBasicParsing }
  catch { throw "could not download $Url`n   $($_.Exception.Message)" }
}

function Expand-Any($Archive, $Dest) {
  New-Item -ItemType Directory -Force -Path $Dest | Out-Null
  if ($Archive -match '\.zip$') { Expand-Archive -Path $Archive -DestinationPath $Dest -Force }
  else { & tar -xf $Archive -C $Dest; if ($LASTEXITCODE) { throw "tar failed on $Archive" } }
}

function Latest-Asset($Repo, $Pattern) {
  $api = "https://api.github.com/repos/$Repo/releases/latest"
  $rel = Invoke-RestMethod -Uri $api -UseBasicParsing -Headers @{ 'User-Agent' = 'fatalframe-bootstrap' }
  $a = $rel.assets | Where-Object { $_.name -match $Pattern } | Select-Object -First 1
  if (-not $a) { throw "no asset matching $Pattern in $Repo $($rel.tag_name)" }
  return $a
}

function Need($Exe) { return -not (Get-Command $Exe -ErrorAction SilentlyContinue) }

# ---------------------------------------------------------------- Python
$PyDir = Join-Path $Tools 'python'
$PyExe = Join-Path $PyDir 'python.exe'
if (-not (Test-Path $PyExe)) {
  Write-Host "`n[1/4] Python"
  $v = '3.12.8'
  $zip = Join-Path $env:TEMP "python-$v-embed.zip"
  Get-File "https://www.python.org/ftp/python/$v/python-$v-embed-amd64.zip" $zip
  Expand-Any $zip $PyDir
  Remove-Item $zip -Force
  # the embeddable build ships with site-packages switched off
  Get-ChildItem $PyDir -Filter 'python*._pth' | ForEach-Object {
    (Get-Content $_.FullName) -replace '^#\s*import site', 'import site' | Set-Content $_.FullName
  }
  New-Item -ItemType Directory -Force -Path (Join-Path $PyDir 'Lib\site-packages') | Out-Null
  Say "ok"
} else { Write-Host "`n[1/4] Python - already here" }

# -------------------------------------------------------------- capstone
$SitePkgs = Join-Path $PyDir 'Lib\site-packages'
if (-not (Test-Path (Join-Path $SitePkgs 'capstone'))) {
  Write-Host "`n[2/4] capstone (disassembler)"
  $meta = Invoke-RestMethod -Uri 'https://pypi.org/pypi/capstone/json' -UseBasicParsing
  $whl = $meta.urls | Where-Object { $_.filename -match 'win_amd64\.whl$' } | Select-Object -First 1
  if (-not $whl) {
    $all = $meta.releases.PSObject.Properties | Sort-Object { [version]($_.Name -replace '[^\d.].*$','0') } -Descending
    foreach ($r in $all) {
      $whl = $r.Value | Where-Object { $_.filename -match 'win_amd64\.whl$' } | Select-Object -First 1
      if ($whl) { break }
    }
  }
  if (-not $whl) { throw "no capstone win_amd64 wheel found on PyPI" }
  $w = Join-Path $env:TEMP $whl.filename
  Get-File $whl.url $w
  Expand-Archive -Path $w -DestinationPath $SitePkgs -Force   # a wheel is a zip
  Remove-Item $w -Force
  Say "ok"
} else { Write-Host "`n[2/4] capstone - already here" }

# ----------------------------------------------------------------- CMake
$CMakeDir = Join-Path $Tools 'cmake'
if ((Need 'cmake') -and -not (Get-ChildItem $CMakeDir -Filter 'cmake.exe' -Recurse -ErrorAction SilentlyContinue)) {
  Write-Host "`n[3/4] CMake"
  $a = Latest-Asset 'Kitware/CMake' 'windows-x86_64\.zip$'
  $z = Join-Path $env:TEMP $a.name
  Get-File $a.browser_download_url $z
  Expand-Any $z $CMakeDir
  Remove-Item $z -Force
  Say "ok"
} else { Write-Host "`n[3/4] CMake - already here" }

# -------------------------------------------------------------- compiler
$GccDir = Join-Path $Tools 'mingw'
if ((Need 'cl') -and (Need 'gcc') -and -not (Get-ChildItem $GccDir -Filter 'gcc.exe' -Recurse -ErrorAction SilentlyContinue)) {
  Write-Host "`n[4/4] C compiler (MinGW-w64 GCC) - this is the big one, ~150 MB"
  $a = Latest-Asset 'brechtsanders/winlibs_mingw' 'x86_64.*ucrt.*\.zip$'
  $z = Join-Path $env:TEMP $a.name
  Get-File $a.browser_download_url $z
  Expand-Any $z $GccDir
  Remove-Item $z -Force
  Say "ok"
} else { Write-Host "`n[4/4] C compiler - already here" }

Write-Host "`n   Build tools ready in tools\.`n"
