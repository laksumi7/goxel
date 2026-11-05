param([int]$ProcId, [string]$Out)
# Capture one window by process id (no focus change, no foreground steal).
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public class Cap3 {
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out R r);
    [StructLayout(LayoutKind.Sequential)] public struct R { public int L, T, Rr, B; }
    public static int[] Get(IntPtr h) {
        SetProcessDPIAware();
        R r; GetWindowRect(h, out r);
        return new int[]{r.L, r.T, r.Rr - r.L, r.B - r.T};
    }
}
'@
$p = Get-Process -Id $ProcId -ErrorAction SilentlyContinue
if (-not $p -or $p.MainWindowHandle -eq [IntPtr]::Zero) { Write-Output "no window for pid $ProcId"; exit }
$r = [Cap3]::Get($p.MainWindowHandle)
$bmp = New-Object System.Drawing.Bitmap($r[2], $r[3])
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($r[0], $r[1], 0, 0, (New-Object System.Drawing.Size($r[2], $r[3])))
$bmp.Save($Out)
$g.Dispose(); $bmp.Dispose()
Write-Output "saved $Out ($($r[2])x$($r[3]))"
