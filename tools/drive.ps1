# Test helper for driving the game's own menus from a script while testing the mod: a picture of a screen, a click, or a key.
# The game's menus take a click only once the pointer is already there, so click twice, about a second apart.
#   drive.ps1 shot  [-Out file.png]            picture of the primary screen at 1280x720
#   drive.ps1 click -X 960 -Y 540              left click, in real screen pixels
#   drive.ps1 key   -Keys m,k -HoldMs 200      keys held the way a keyboard holds them
#   drive.ps1 focus                            brings the game's window to the front
param([Parameter(Mandatory)][string]$Action, [int]$X = 0, [int]$Y = 0, [string]$Out = "", [string[]]$Keys = @(), [int]$HoldMs = 200, [int]$Monitor = 0)

Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms
Add-Type -Name U -Namespace T8D -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
[DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
[DllImport("user32.dll")] public static extern void mouse_event(uint flags, uint dx, uint dy, uint data, System.UIntPtr extra);
[DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, System.UIntPtr extra);
[DllImport("user32.dll")] public static extern uint MapVirtualKeyW(uint code, uint type);
[DllImport("user32.dll")] public static extern bool SetForegroundWindow(System.IntPtr window);
[DllImport("user32.dll")] public static extern bool ShowWindow(System.IntPtr window, int command);
[DllImport("user32.dll")] public static extern System.IntPtr GetForegroundWindow();
[DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(System.IntPtr window, out uint process);
[DllImport("user32.dll")] public static extern bool GetWindowRect(System.IntPtr window, out RECT rect);
[DllImport("user32.dll")] public static extern bool GetClientRect(System.IntPtr window, out RECT rect);
public struct RECT { public int Left, Top, Right, Bottom; }
'@
[T8D.U]::SetProcessDPIAware() | Out-Null

function Game { Get-Process Polaris-Win64-Shipping -ErrorAction SilentlyContinue | Select-Object -First 1 }

switch ($Action) {
    "shot" {
        $screen = [System.Windows.Forms.Screen]::AllScreens[$Monitor].Bounds
        $full = New-Object System.Drawing.Bitmap $screen.Width, $screen.Height
        $g = [System.Drawing.Graphics]::FromImage($full)
        $g.CopyFromScreen($screen.X, $screen.Y, 0, 0, $full.Size)
        $small = New-Object System.Drawing.Bitmap 1280, 720
        $g2 = [System.Drawing.Graphics]::FromImage($small)
        $g2.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBilinear
        $g2.DrawImage($full, 0, 0, 1280, 720)
        $small.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
        $g.Dispose(); $g2.Dispose(); $full.Dispose(); $small.Dispose()
        "saved $Out ($($screen.Width)x$($screen.Height) at $($screen.X),$($screen.Y))"
    }
    "click" {
        [T8D.U]::SetCursorPos($X, $Y) | Out-Null
        Start-Sleep -Milliseconds 120
        [T8D.U]::mouse_event(2, 0, 0, 0, [System.UIntPtr]::Zero)
        Start-Sleep -Milliseconds 90
        [T8D.U]::mouse_event(4, 0, 0, 0, [System.UIntPtr]::Zero)
        "clicked $X,$Y"
    }
    "key" {
        $map = @{ insert = 0x2D; up = 0x26; down = 0x28; left = 0x25; right = 0x27; space = 0x20; esc = 0x1B; enter = 0x0D; home = 0x24; f5 = 0x74; f7 = 0x76; f9 = 0x78 }
        foreach ($key in ($Keys -split ',')) {
            $vk = if ($map.ContainsKey($key)) { $map[$key] } else { [int][char]$key.ToUpper() }
            $extended = if (@('insert', 'home', 'up', 'down', 'left', 'right') -contains $key) { 1 } else { 0 }
            $scan = [T8D.U]::MapVirtualKeyW($vk, 0)
            [T8D.U]::keybd_event([byte]$vk, [byte]$scan, [uint32]$extended, [System.UIntPtr]::Zero)
            Start-Sleep -Milliseconds $HoldMs
            [T8D.U]::keybd_event([byte]$vk, [byte]$scan, [uint32]($extended -bor 2), [System.UIntPtr]::Zero)
            Start-Sleep -Milliseconds 300
        }
        "pressed $($Keys -join ',')"
    }
    "focus" {
        $p = Game
        if (-not $p) { "the game isn't running"; return }
        [T8D.U]::ShowWindow($p.MainWindowHandle, 9) | Out-Null
        [T8D.U]::SetForegroundWindow($p.MainWindowHandle) | Out-Null
        (New-Object -ComObject WScript.Shell).AppActivate($p.Id) | Out-Null
        Start-Sleep -Milliseconds 400
        $front = 0; [T8D.U]::GetWindowThreadProcessId([T8D.U]::GetForegroundWindow(), [ref]$front) | Out-Null
        $rect = New-Object T8D.U+RECT; [T8D.U]::GetWindowRect($p.MainWindowHandle, [ref]$rect) | Out-Null
        $client = New-Object T8D.U+RECT; [T8D.U]::GetClientRect($p.MainWindowHandle, [ref]$client) | Out-Null
        "game $($p.Id) in front: $($front -eq $p.Id); window $($rect.Left),$($rect.Top) to $($rect.Right),$($rect.Bottom); picture $($client.Right)x$($client.Bottom)"
    }
}
