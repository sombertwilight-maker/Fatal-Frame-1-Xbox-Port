' Fatal Frame - native Windows : opens the installer window.
' A plain shortcut to PowerShell would flash a console first; this does not.
Set fso = CreateObject("Scripting.FileSystemObject")
here = fso.GetParentFolderName(WScript.ScriptFullName)
ps   = here & "\installer\FatalFrameInstaller.ps1"
If Not fso.FileExists(ps) Then
  MsgBox "The installer folder is missing." & vbCrLf & vbCrLf & _
         "Unzip the whole release and keep these files together.", 16, "Fatal Frame"
  WScript.Quit 1
End If
CreateObject("WScript.Shell").Run _
  "powershell -NoProfile -ExecutionPolicy Bypass -STA -File """ & ps & """", 0, False
