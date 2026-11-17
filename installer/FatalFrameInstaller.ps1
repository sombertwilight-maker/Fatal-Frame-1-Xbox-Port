# =====================================================================
#  Fatal Frame - native Windows : installer front end
#
#  This runs before any build tool exists on the machine, so it leans on
#  nothing but what Windows already ships: PowerShell and WinForms. It
#  drives PlayFatalFrame.bat and reads that build's own output for
#  progress rather than inventing a number that creeps on a timer.
# =====================================================================
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
[System.Windows.Forms.Application]::EnableVisualStyles()

$Here = Split-Path -Parent $MyInvocation.MyCommand.Definition
$Root = Split-Path -Parent $Here
$Art  = Join-Path $Here 'assets'
$Snd  = Join-Path $Here 'sounds'
$Work = Join-Path $Root 'work'
$Bat  = Join-Path $Root 'PlayFatalFrame.bat'
$Gen  = Join-Path $Root 'project\src\recomp\gen'
# Where the finished game lives. Game\ is where the build installs it;
# the project\build paths are only a fallback for a tree built before
# that step existed.
$ExeG = Join-Path $Root 'Game\fatalframe.exe'
$Exe1 = Join-Path $Root 'project\build\Release\fatalframe.exe'
$Exe2 = Join-Path $Root 'project\build\fatalframe.exe'
$IsoMemo = Join-Path $Work 'iso.txt'
function Find-Exe {
  foreach ($e in @($ExeG,$Exe1,$Exe2)) { if (Test-Path $e) { return $e } }
  return $null
}
if (-not (Test-Path $Work)) { New-Item -ItemType Directory -Path $Work -Force | Out-Null }

if (-not (Test-Path $Bat)) {
  [Windows.Forms.MessageBox]::Show(
    "PlayFatalFrame.bat is not next to the installer folder.`r`n`r`nKeep this installer folder inside the unzipped release.",
    'Fatal Frame', 'OK', 'Error') | Out-Null
  return
}

function Load-Img([string]$n) {
  $p = Join-Path $Art $n
  if (Test-Path $p) { try { return [Drawing.Image]::FromFile($p) } catch { } }
  return $null
}
$imgBack = Load-Img 'background.png'
$imgGold = Load-Img 'orb_gold.png'
$imgAura = Load-Img 'aura.png'
$imgFill = Load-Img 'fill.png'

# ---- sound ----------------------------------------------------------
# Windows Media Player's COM control handles mp3 without extra assemblies.
# Absent (N editions, stripped installs) the whole thing simply stays quiet.
function New-Sound([string]$file, [int]$vol, [bool]$loop) {
  $p = Join-Path $Snd $file
  if (-not (Test-Path $p)) { return $null }
  try {
    $w = New-Object -ComObject WMPlayer.OCX
    $w.settings.volume = $vol
    $w.settings.autoStart = $false
    $w.URL = $p
    if ($loop) { try { $w.settings.setMode('loop', $true) } catch { } }
    $w.controls.stop()
    return $w
  } catch { return $null }
}
$sndBgm   = New-Sound 'bgm.mp3'       35 $true
$sndHover = New-Sound 'hover.mp3'     55 $false
$sndOrb   = New-Sound 'orb_hover.mp3' 60 $false
$sndClick = New-Sound 'click.mp3'     75 $false
$sndTick  = New-Sound 'tick.mp3'      65 $false
function Play($s) { if ($s) { try { $s.controls.currentPosition = 0; $s.controls.play() } catch { } } }
Play $sndBgm

# ---- geometry, measured off the layout art --------------------------
$R_LOAD = New-Object Drawing.Rectangle 642,10,145,40
$R_REIN = New-Object Drawing.Rectangle 12,390,145,41
$R_QUIT = New-Object Drawing.Rectangle 177,389,145,40
$R_BAR  = New-Object Drawing.Rectangle 23,512,549,54
$R_ORB  = New-Object Drawing.Rectangle 638,419,118,116
$ORB_CX = 697; $ORB_CY = 477; $ORB_R = 59

$script:Iso     = $null
$script:Phase   = 'Load your disc image to begin.'
$script:Pct     = 0.0
$script:Running = $false
$script:Done    = $false
$script:Failed  = $false
$script:Proc    = $null
$script:Hot     = ''
$script:Beat    = 0.0
# Once the game exists this stops being an installer. The build bar is for
# the build; leaving it sitting full and gold afterwards reads as work still
# happening, which is exactly backwards -- it means there is nothing to do
# but play. $Settled drives the quieter launcher face.
$script:Settled = $false
$script:DoneAt  = $null
$script:Leaving = $null

$form = New-Object Windows.Forms.Form
$form.Text = 'Fatal Frame - native Windows'
$form.ClientSize = New-Object Drawing.Size 800,600
$form.FormBorderStyle = 'None'
$form.StartPosition = 'CenterScreen'
$form.BackColor = [Drawing.Color]::FromArgb(10,14,12)
$form.KeyPreview = $true
try {
  $form.GetType().GetProperty('DoubleBuffered',
    [Reflection.BindingFlags]'Instance,NonPublic').SetValue($form,$true,$null)
} catch { }

function Rounded($rect,[int]$radius) {
  $p = New-Object Drawing.Drawing2D.GraphicsPath
  $d = $radius * 2
  $p.AddArc($rect.X, $rect.Y, $d, $d, 180, 90)
  $p.AddArc($rect.Right - $d, $rect.Y, $d, $d, 270, 90)
  $p.AddArc($rect.Right - $d, $rect.Bottom - $d, $d, $d, 0, 90)
  $p.AddArc($rect.X, $rect.Bottom - $d, $d, $d, 90, 90)
  $p.CloseFigure()
  return $p
}

function Draw-Button($g,$rect,[string]$text,[bool]$on,[bool]$hot) {
  $path = Rounded $rect 17
  if (-not $on)  { $c1=[Drawing.Color]::FromArgb(150,126,126,120); $c2=[Drawing.Color]::FromArgb(150,74,74,70); $tc=[Drawing.Color]::FromArgb(190,186,180) }
  elseif ($hot)  { $c1=[Drawing.Color]::FromArgb(255,243,206);     $c2=[Drawing.Color]::FromArgb(206,158,52);  $tc=[Drawing.Color]::FromArgb(46,30,8) }
  else           { $c1=[Drawing.Color]::FromArgb(243,240,232);     $c2=[Drawing.Color]::FromArgb(176,172,164); $tc=[Drawing.Color]::FromArgb(40,38,34) }
  $br = New-Object Drawing.Drawing2D.LinearGradientBrush $rect,$c1,$c2,90.0
  $g.FillPath($br,$path)
  $pen = New-Object Drawing.Pen ([Drawing.Color]::FromArgb(170,58,46,24)),1.6
  $g.DrawPath($pen,$path)
  $f = New-Object Drawing.Font 'Georgia',11.0,([Drawing.FontStyle]::Bold)
  $sf = New-Object Drawing.StringFormat
  $sf.Alignment='Center'; $sf.LineAlignment='Center'
  $g.DrawString($text,$f,(New-Object Drawing.SolidBrush $tc),([Drawing.RectangleF]$rect),$sf)
  $br.Dispose(); $pen.Dispose(); $path.Dispose(); $f.Dispose(); $sf.Dispose()
}

function Draw-Alpha($g,$img,$rect,[double]$alpha) {
  $cm = New-Object Drawing.Imaging.ColorMatrix
  $cm.Matrix33 = $alpha
  $ia = New-Object Drawing.Imaging.ImageAttributes
  $ia.SetColorMatrix($cm)
  $g.DrawImage($img,$rect,0,0,$img.Width,$img.Height,[Drawing.GraphicsUnit]::Pixel,$ia)
  $ia.Dispose()
}

$form.Add_Paint({
  $g = $_.Graphics
  $g.SmoothingMode = 'AntiAlias'
  $g.InterpolationMode = 'HighQualityBicubic'
  $g.PixelOffsetMode = 'HighQuality'
  if ($imgBack) { $g.DrawImage($imgBack,0,0,800,600) }

  $armed = ($null -ne $script:Iso) -or $script:Done

  # ---- the orb. The idle silver one is already part of the artwork, so
  # ---- arming it means painting the gold one over the top.
  if ($armed) {
    if ($imgAura) {
      # a slow breath while the build runs, steady otherwise
      $a = if ($script:Running) { 0.16 + 0.17 * (1 + [Math]::Sin($script:Beat)) / 2 } else { 0.14 }
      if ($script:Hot -eq 'orb') { $a += 0.12 }
      $s = 1.62
      $aw = [int]($R_ORB.Width*$s); $ah = [int]($R_ORB.Height*$s)
      $ar = New-Object Drawing.Rectangle ($ORB_CX-[int]($aw/2)),($ORB_CY-[int]($ah/2)),$aw,$ah
      Draw-Alpha $g $imgAura $ar $a
    }
    if ($imgGold) {
      if ($script:Hot -eq 'orb' -and -not $script:Running) {
        $gr = New-Object Drawing.Rectangle ($R_ORB.X-3),($R_ORB.Y-3),($R_ORB.Width+6),($R_ORB.Height+6)
        $g.DrawImage($imgGold,$gr)
      } else {
        $g.DrawImage($imgGold,$R_ORB)
      }
    }
  }

  $cap = if ($script:Running) { 'BUILDING' }
         elseif ($script:Done)  { 'PLAY' }
         elseif ($script:Failed){ 'TRY AGAIN' }
         elseif ($armed)        { 'INSTALL GAME' }
         else { '' }
  if ($cap) {
    $sf = New-Object Drawing.StringFormat
    $sf.Alignment='Center'; $sf.LineAlignment='Center'
    $f = New-Object Drawing.Font 'Georgia',10.5,([Drawing.FontStyle]::Bold)
    $cr = New-Object Drawing.RectangleF ($ORB_CX-100),($ORB_CY+$ORB_R+6),200,22
    $g.DrawString($cap,$f,(New-Object Drawing.SolidBrush ([Drawing.Color]::FromArgb(70,30,14))),
                  (New-Object Drawing.RectangleF ($ORB_CX-99),($ORB_CY+$ORB_R+7),200,22),$sf)
    $g.DrawString($cap,$f,(New-Object Drawing.SolidBrush ([Drawing.Color]::FromArgb(248,236,200))),$cr,$sf)
    $f.Dispose(); $sf.Dispose()
  }

  # ---- the progress bar ----
  $bp = Rounded $R_BAR 24
  $g.FillPath((New-Object Drawing.SolidBrush ([Drawing.Color]::FromArgb(210,16,13,11))),$bp)
  $frac = if ($script:Settled -and -not $script:Running) { 0.0 }
          else { [Math]::Max(0.0,[Math]::Min(1.0,$script:Pct)) }
  if ($frac -gt 0.002) {
    $w = [int]($R_BAR.Width*$frac); if ($w -lt 8) { $w = 8 }
    $old = $g.Clip
    $g.SetClip($bp)
    $g.IntersectClip((New-Object Drawing.Rectangle $R_BAR.X,$R_BAR.Y,$w,$R_BAR.Height))
    if ($imgFill) { $g.DrawImage($imgFill,$R_BAR) }
    else {
      $b2 = New-Object Drawing.Drawing2D.LinearGradientBrush $R_BAR,
            ([Drawing.Color]::FromArgb(255,230,150)),([Drawing.Color]::FromArgb(198,142,36)),90.0
      $g.FillRectangle($b2,$R_BAR); $b2.Dispose()
    }
    # a brighter lip at the leading edge, so the bar reads as advancing
    if ($frac -lt 0.999) {
      $lip = New-Object Drawing.Rectangle ($R_BAR.X+$w-14),$R_BAR.Y,14,$R_BAR.Height
      $lb = New-Object Drawing.Drawing2D.LinearGradientBrush $lip,
            ([Drawing.Color]::FromArgb(0,255,246,214)),([Drawing.Color]::FromArgb(205,255,246,214)),0.0
      $g.FillRectangle($lb,$lip); $lb.Dispose()
    }
    $g.Clip = $old
  }
  $g.DrawPath((New-Object Drawing.Pen ([Drawing.Color]::FromArgb(200,224,198,138)),2.0),$bp)
  $bp.Dispose()

  $sf2 = New-Object Drawing.StringFormat
  $sf2.Alignment='Center'; $sf2.LineAlignment='Center'
  $f2 = New-Object Drawing.Font 'Georgia',10.5,([Drawing.FontStyle]::Bold)
  $txt = $script:Phase
  if ($script:Running) { $txt = ('{0}    {1:0}%' -f $script:Phase,($frac*100)) }
  $g.DrawString($txt,$f2,(New-Object Drawing.SolidBrush ([Drawing.Color]::FromArgb(190,8,6,5))),
                (New-Object Drawing.RectangleF ($R_BAR.X+1),($R_BAR.Y+1),$R_BAR.Width,$R_BAR.Height),$sf2)
  $g.DrawString($txt,$f2,(New-Object Drawing.SolidBrush ([Drawing.Color]::FromArgb(242,236,220))),
                ([Drawing.RectangleF]$R_BAR),$sf2)
  $f2.Dispose()

  # ---- buttons ----
  Draw-Button $g $R_LOAD 'LOAD ISO'  (-not $script:Running) ($script:Hot -eq 'load')
  Draw-Button $g $R_REIN 'REINSTALL' (-not $script:Running) ($script:Hot -eq 'rein')
  Draw-Button $g $R_QUIT 'QUIT'      $true                  ($script:Hot -eq 'quit')

  if ($script:Iso) {
    $f3 = New-Object Drawing.Font 'Georgia',8.5
    $sf3 = New-Object Drawing.StringFormat
    $sf3.Alignment='Far'; $sf3.LineAlignment='Center'
    $sf3.Trimming = 'EllipsisCharacter'
    $nm = [IO.Path]::GetFileName($script:Iso)
    $g.DrawString($nm,$f3,(New-Object Drawing.SolidBrush ([Drawing.Color]::FromArgb(150,6,8,6))),
                  (New-Object Drawing.RectangleF 331,53,458,20),$sf3)
    $g.DrawString($nm,$f3,(New-Object Drawing.SolidBrush ([Drawing.Color]::FromArgb(236,230,212))),
                  (New-Object Drawing.RectangleF 330,52,458,20),$sf3)
    $f3.Dispose(); $sf3.Dispose()
  }
  $sf2.Dispose()
})

function HitOrb($p) {
  $dx = $p.X-$ORB_CX; $dy = $p.Y-$ORB_CY
  return (($dx*$dx+$dy*$dy) -le ($ORB_R*$ORB_R))
}
function HotAt($p) {
  if ($R_LOAD.Contains($p)) { return 'load' }
  if ($R_REIN.Contains($p)) { return 'rein' }
  if ($R_QUIT.Contains($p)) { return 'quit' }
  if (HitOrb $p)            { return 'orb'  }
  return ''
}

$script:Drag = $false
$script:DragPt = New-Object Drawing.Point 0,0

$form.Add_MouseDown({
  if ($_.Button -eq 'Left' -and (HotAt $_.Location) -eq '') {
    $script:Drag = $true
    $script:DragPt = $_.Location
  }
})
$form.Add_MouseUp({ $script:Drag = $false })

$form.Add_MouseMove({
  if ($script:Drag) {
    $form.Left += $_.X - $script:DragPt.X
    $form.Top  += $_.Y - $script:DragPt.Y
    return
  }
  $h = HotAt $_.Location
  if ($h -ne $script:Hot) {
    if ($h -eq 'orb' -and (($null -ne $script:Iso) -or $script:Done)) { Play $sndOrb }
    elseif ($h -ne '') { Play $sndHover }
    $script:Hot = $h
    $form.Invalidate()
  }
})

function Ask-Iso {
  $d = New-Object Windows.Forms.OpenFileDialog
  $d.Title = 'Choose your Fatal Frame (USA) disc image'
  $d.Filter = 'Disc image (*.iso;*.xiso)|*.iso;*.xiso|All files (*.*)|*.*'
  $d.InitialDirectory = $Root
  if ($d.ShowDialog($form) -ne [Windows.Forms.DialogResult]::OK) { return $false }
  $script:Iso = $d.FileName
  $script:Failed = $false
  try { Set-Content -LiteralPath $IsoMemo -Value $script:Iso -Encoding ASCII -Force } catch { }
  Play $sndTick
  $form.Invalidate()
  return $true
}

function Start-Build {
  if ($script:Running -or $null -eq $script:Iso) { return }
  foreach ($f in 'installer.log','cmake-build.log','cmake-configure.log') {
    $p = Join-Path $Work $f
    if (Test-Path $p) { Remove-Item $p -Force -ErrorAction SilentlyContinue }
  }
  $script:Running = $true
  $script:Failed = $false
  $script:Settled = $false
  $script:DoneAt = $null
  $script:Phase = 'Starting the build'
  $script:Pct = 0.012
  $form.Invalidate()
  $form.Update()
  $log = Join-Path $Work 'installer.log'
  # FF_BUILD_ONLY keeps the batch file from launching the game behind us;
  # this window owns that, through the orb.
  #
  # Written to a wrapper rather than passed on a cmd /c command line: cmd
  # strips quotes by rules that depend on what follows them, and this line
  # has a quoted path, a quoted argument and a redirect. One file, one
  # level of quoting, nothing to misparse.
  $run = Join-Path $Work 'gui-build.cmd'
  $lines = @(
    '@echo off',
    'set FF_BUILD_ONLY=1',
    'set FF_REBUILD=1',
    ('cd /d "{0}"' -f $Root),
    ('call "{0}" "{1}" > "{2}" 2>&1' -f $Bat,$script:Iso,$log),
    'exit /b %errorlevel%'
  )
  try { Set-Content -LiteralPath $run -Value $lines -Encoding ASCII -Force }
  catch {
    $script:Running = $false; $script:Failed = $true
    $script:Phase = 'Could not write to the work folder.'
    $form.Invalidate(); return
  }
  $psi = New-Object Diagnostics.ProcessStartInfo
  $psi.FileName = $run
  $psi.WorkingDirectory = $Root
  $psi.UseShellExecute = $false
  $psi.CreateNoWindow = $true
  try { $script:Proc = [Diagnostics.Process]::Start($psi) }
  catch {
    $script:Running = $false; $script:Failed = $true
    $script:Phase = 'Could not start the build.'
  }
  $form.Invalidate()
}

function Start-Game {
  $exe = Find-Exe
  if (-not $exe) { return }
  if ($script:Iso) {
    try { Set-Content -LiteralPath $IsoMemo -Value $script:Iso -Encoding ASCII -Force } catch { }
  }
  if ($sndBgm) { try { $sndBgm.controls.stop() } catch { } }
  # Keep the game's own diagnostics. It reports costume redirects and
  # missing files on stderr, and a detached launch throws that away -- so
  # when something does not load there is nothing to look at. If the
  # redirect cannot be set up, start it plainly rather than not at all.
  $glog = Join-Path $Work 'game.log'
  try {
    Start-Process -FilePath $exe -ArgumentList ('"{0}"' -f $script:Iso) `
      -WorkingDirectory $Root -RedirectStandardError $glog
  } catch {
    try { Start-Process -FilePath $exe -ArgumentList ('"{0}"' -f $script:Iso) -WorkingDirectory $Root } catch { }
  }
  $script:Phase = 'Starting Fatal Frame...'
  $script:Leaving = Get-Date
  $form.Invalidate()
}

$form.Add_MouseClick({
  $h = HotAt $_.Location
  switch ($h) {
    'quit' { Play $sndClick; $form.Close() }
    'load' {
      if ($script:Running) { return }
      Play $sndClick
      if (Ask-Iso) {
        $script:Phase = if ($script:Done) { 'Ready. Touch the orb to play.' }
                        else { 'Ready. Touch the orb to install.' }
        $form.Invalidate()
      }
    }
    'rein' {
      if ($script:Running) { return }
      Play $sndClick
      # $form as owner: without it this dialog can open behind a borderless
      # window, and the button looks dead.
      $ans = [Windows.Forms.MessageBox]::Show($form,
        "Build the game again from your disc image?`r`n`r`nThe game you have now keeps working until the new build finishes.",
        'Reinstall', 'YesNo', 'Question')
      if ($ans -ne [Windows.Forms.DialogResult]::Yes) { return }
      if ($null -eq $script:Iso) { if (-not (Ask-Iso)) { return } }
      Start-Build
    }
    'orb'  {
      if ($script:Running) { return }
      Play $sndClick
      if ($script:Done) {
        # Installed means this is a launcher. The orb plays, and never
        # rebuilds -- that is what REINSTALL is for.
        if ($null -eq $script:Iso) { if (-not (Ask-Iso)) { return } }
        Start-Game
      } else {
        if ($null -eq $script:Iso) { if (-not (Ask-Iso)) { return } }
        Start-Build
      }
    }
  }
})

# ---- progress, read from the build's own output ---------------------
# The log is held open by the running batch file, so it has to be opened
# share-all; ReadAllText would trip over the writer's handle.
function Read-Shared([string]$path) {
  if (-not (Test-Path $path)) { return '' }
  try {
    $fs = New-Object IO.FileStream $path,([IO.FileMode]::Open),([IO.FileAccess]::Read),
          ([IO.FileShare]'ReadWrite,Delete')
    $sr = New-Object IO.StreamReader $fs
    $t = $sr.ReadToEnd()
    $sr.Close(); $fs.Close()
    return $t
  } catch { return '' }
}

$timer = New-Object Windows.Forms.Timer
$timer.Interval = 200
$timer.Add_Tick({
  $script:Beat += 0.09

  # Let a finished build show its full bar for a breath, then become a
  # launcher: bar emptied, wording changed, nothing implying activity.
  if ($script:Done -and -not $script:Running -and -not $script:Settled) {
    if ($null -eq $script:DoneAt -or
        ((Get-Date) - $script:DoneAt).TotalSeconds -ge 2.5) {
      $script:Settled = $true
      $script:Pct = 0.0
      $script:Phase = 'Ready. Touch the orb to play.'
      $form.Invalidate()
    }
  }

  # The game has the screen now; sitting on top of it looks like a hang.
  if ($null -ne $script:Leaving -and
      ((Get-Date) - $script:Leaving).TotalSeconds -ge 1.2) {
    $script:Leaving = $null
    $script:Phase = 'Ready. Touch the orb to play.'
    $form.WindowState = 'Minimized'
    $form.Invalidate()
  }

  if (-not $script:Running) {
    if ($null -ne $script:Iso -and $imgAura) { $form.Invalidate() }
    return
  }

  $t = Read-Shared (Join-Path $Work 'installer.log')
  $phase = $script:Phase
  $pct = $script:Pct

  if ($t -match 'Fetching the build tools') { $phase='Fetching the build tools'; $pct=[Math]::Max($pct,0.04) }
  if ($t -match 'Python\s*:')               { $phase='Build tools ready';        $pct=[Math]::Max($pct,0.14) }
  if ($t -match '\[1/2\] Translating') {
    $phase = 'Translating the game from your disc'
    # a real signal: the translator writes its C files as it goes
    $n = 0
    if (Test-Path $Gen) {
      try { $n = @([IO.Directory]::GetFiles($Gen,'*.c')).Count } catch { $n = 0 }
    }
    $pct = [Math]::Max($pct, 0.18 + 0.34*[Math]::Min(1.0,($n/20.0)))
  }
  if ($t -match 'Translated code already present') { $pct=[Math]::Max($pct,0.52) }
  if ($t -match 'path contains a space')           { $phase='Staging the compiler' }
  if ($t -match '\[2/2\] Building') {
    $phase = 'Compiling'
    $pct = [Math]::Max($pct,0.55)
    $b = Read-Shared (Join-Path $Work 'cmake-build.log')
    if ($b) {
      $ms = [regex]::Matches($b,'\[\s*(\d+)%\]')
      if ($ms.Count -gt 0) {
        $last = [int]$ms[$ms.Count-1].Groups[1].Value
        $pct = [Math]::Max($pct, 0.56 + 0.43*($last/100.0))
      }
    }
  }
  $script:Phase = $phase
  $script:Pct = $pct

  if ($script:Proc -and $script:Proc.HasExited) {
    $script:Running = $false
    if (Find-Exe) {
      $script:Done = $true; $script:Pct = 1.0
      $script:DoneAt = Get-Date
      $script:Phase = 'Installed. Touch the orb to play.'
    } else {
      $script:Failed = $true; $script:Pct = 0.0
      $script:Phase =
        if     ($t -match 'Configure failed')         { 'Configure failed - see work\cmake-configure.log' }
        elseif ($t -match 'Build failed')             { 'Build failed - see work\cmake-build.log' }
        elseif ($t -match 'Translation failed')       { 'Translation failed - is that the right disc image?' }
        elseif ($t -match 'Could not fetch the build tools') { 'Could not fetch the build tools - check the connection' }
        else { 'Install failed - see work\installer.log' }
    }
  }
  $form.Invalidate()
})
$timer.Start()

# already built by an earlier run? Then recall the disc image it was built
# from, so the orb plays straight away instead of asking again.
if (Find-Exe) {
  $script:Done = $true; $script:Pct = 1.0
  if (Test-Path $IsoMemo) {
    try {
      $remembered = (Get-Content -LiteralPath $IsoMemo -TotalCount 1).Trim()
      if ($remembered -and (Test-Path $remembered)) { $script:Iso = $remembered }
    } catch { }
  }
  $script:Settled = $true
  $script:Pct = 0.0
  $script:Phase = 'Ready. Touch the orb to play.'
}

$form.Add_KeyDown({ if ($_.KeyCode -eq 'Escape') { $form.Close() } })
$form.Add_FormClosed({
  $timer.Stop()
  foreach ($s in @($sndBgm,$sndHover,$sndOrb,$sndClick,$sndTick)) {
    if ($s) { try { $s.controls.stop(); $s.close() } catch { } }
  }
})

[void]$form.ShowDialog()
