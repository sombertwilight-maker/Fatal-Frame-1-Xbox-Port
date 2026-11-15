# Fetch the build tools this port needs, into .\tools, once.
# Nothing here is redistributed: each piece is downloaded from its own
# upstream on the player's machine, so the release stays small and no
# third-party licence travels with it.
#
#   powershell -ExecutionPolicy Bypass -File tools\bootstrap.ps1
param([string]$Root = (Split-Path -Parent $PSScriptRoot))

try {
  if ($Root) { $Root = $Root.Trim('"').TrimEnd('\') }
  if (-not $Root) { $Root = Split-Path -Parent $PSScriptRoot }
  $ErrorActionPreference = 'Stop'
  $ProgressPreference    = 'SilentlyContinue'   # the progress bar makes downloads ~10x slower
  try { [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12 } catch {}
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

  function Latest-Asset($Repo, $Pattern, $Exclude, [switch]$Smallest) {
    $api = "https://api.github.com/repos/$Repo/releases/latest"
    $rel = Invoke-RestMethod -Uri $api -UseBasicParsing -Headers @{ 'User-Agent' = 'fatalframe-bootstrap' }
    $m = $rel.assets | Where-Object { $_.name -match $Pattern }
    if ($Exclude) { $m = $m | Where-Object { $_.name -notmatch $Exclude } }
    if ($Smallest) { $m = $m | Sort-Object size }
    $a = $m | Select-Object -First 1
    if (-not $a) { throw "no asset matching $Pattern in $Repo $($rel.tag_name)" }
    Say ("{0} ({1:N0} MB)" -f $a.name, ($a.size / 1MB))
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
    Say "ok"
  } else { Write-Host "`n[1/4] Python - already here" }

  # The embeddable build is deliberately isolated: a ._pth file replaces
  # the normal sys.path, drops the working directory and ignores
  # PYTHONPATH. Left alone, "python -m tools.xiso" cannot see the
  # toolkit's tools package even when run from inside it, and
  # site-packages is off so capstone is invisible too. Write the path
  # file we actually need.
  New-Item -ItemType Directory -Force -Path (Join-Path $PyDir 'Lib\site-packages') | Out-Null
  $toolkit = Join-Path $Root 'toolkit'
  foreach ($pth in @(Get-ChildItem $PyDir -Filter 'python*._pth')) {
    $f = $pth.FullName
    $stdlib = @(Get-Content $f | Where-Object { $_ -match '\.zip$' })
    $want = $stdlib + @('.', 'Lib\site-packages', $toolkit, 'import site')
    Set-Content -Path $f -Value $want -Encoding ASCII
  }

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
    # a wheel is a zip, but Expand-Archive in Windows PowerShell refuses
    # anything not named .zip, so fetch it under that name
    $w = Join-Path $env:TEMP 'capstone-wheel.zip'
    Get-File $whl.url $w
    Expand-Archive -Path $w -DestinationPath $SitePkgs -Force
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
    Write-Host "`n[4/4] C compiler (MinGW-w64 GCC) - this is the big one"
    $a = Latest-Asset 'brechtsanders/winlibs_mingw' 'x86_64.*ucrt.*\.zip$' 'llvm|mcf' -Smallest
    $z = Join-Path $env:TEMP $a.name
    Get-File $a.browser_download_url $z
    Expand-Any $z $GccDir
    Remove-Item $z -Force
    Say "ok"
  } else { Write-Host "`n[4/4] C compiler - already here" }

  Write-Host "`n   Build tools ready in tools\.`n"
} catch {
  Write-Host "`n   FAILED: $($_.Exception.Message)`n" -ForegroundColor Red
  exit 1
}
