# Test helper for driving the studio's copy of TTT2 from a script: a picture of its window, keys, or a command to the studio.
#   drive-ttt2.ps1 shot [-Out file.png]        picture of the game's window
#   drive-ttt2.ps1 key -Keys o,o,return -HoldMs 120   keys, one after the other, with the window in front (names or letters)
#   drive-ttt2.ps1 hold -Keys u -HoldMs 3000   keys held together
#   drive-ttt2.ps1 cmd -Text "peek 82DE69A0 16"   a command for the studio's test channel; prints its answer
param([Parameter(Mandatory)][string]$Action, [string]$Out = "", [string[]]$Keys = @(), [int]$HoldMs = 120, [int]$GapMs = 350, [string]$Text = "",
      [string]$Playtest = "C:\Users\ms3ody\Downloads\Compressed\ttt2-playtest")

Add-Type -AssemblyName System.Drawing
Add-Type -Name U -Namespace T2D -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
[DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, System.UIntPtr extra);
[DllImport("user32.dll")] public static extern uint MapVirtualKeyW(uint code, uint type);
[DllImport("user32.dll")] public static extern bool SetForegroundWindow(System.IntPtr window);
[DllImport("user32.dll")] public static extern bool ShowWindow(System.IntPtr window, int command);
[DllImport("user32.dll")] public static extern System.IntPtr GetForegroundWindow();
[DllImport("user32.dll")] public static extern bool GetClientRect(System.IntPtr window, out RECT rect);
[DllImport("user32.dll")] public static extern bool ClientToScreen(System.IntPtr window, ref POINT point);
public struct RECT { public int Left, Top, Right, Bottom; }
public struct POINT { public int X, Y; }
'@
[T2D.U]::SetProcessDPIAware() | Out-Null
$data = Join-Path $Playtest 'rex\out\build\tekken-studio\studio'

function Game { Get-Process ttt2-studio -ErrorAction SilentlyContinue | Select-Object -First 1 }
function Front {
    $p = Game
    if (-not $p) { throw "the studio isn't running" }
    if ([T2D.U]::GetForegroundWindow() -ne $p.MainWindowHandle) {
        [T2D.U]::ShowWindow($p.MainWindowHandle, 9) | Out-Null
        [T2D.U]::SetForegroundWindow($p.MainWindowHandle) | Out-Null
        (New-Object -ComObject WScript.Shell).AppActivate($p.Id) | Out-Null
        Start-Sleep -Milliseconds 400
    }
    $p
}
$names = @{ insert = 0x2D; up = 0x26; down = 0x28; left = 0x25; right = 0x27; space = 0x20; esc = 0x1B; return = 0x0D; tab = 0x09; semicolon = 0xBA;
            f1 = 0x70; f2 = 0x71; f3 = 0x72; f4 = 0x73; f5 = 0x74; f6 = 0x75; f7 = 0x76; f8 = 0x77; f9 = 0x78; f10 = 0x79; home = 0x24; end = 0x23; shift = 0x10; ctrl = 0x11 }
function Vk([string]$key) { if ($names.ContainsKey($key)) { $names[$key] } else { [int][char]$key.ToUpper() } }
function Down([string]$key) { $vk = Vk $key; $ext = if (@('insert', 'home', 'end', 'up', 'down', 'left', 'right') -contains $key) { 1 } else { 0 }
    [T2D.U]::keybd_event([byte]$vk, [byte][T2D.U]::MapVirtualKeyW($vk, 0), [uint32]$ext, [System.UIntPtr]::Zero) }
function Up([string]$key) { $vk = Vk $key; $ext = if (@('insert', 'home', 'end', 'up', 'down', 'left', 'right') -contains $key) { 1 } else { 0 }
    [T2D.U]::keybd_event([byte]$vk, [byte][T2D.U]::MapVirtualKeyW($vk, 0), [uint32]($ext -bor 2), [System.UIntPtr]::Zero) }

switch ($Action) {
    "shot" {
        $p = Front
        $rect = New-Object T2D.U+RECT; [T2D.U]::GetClientRect($p.MainWindowHandle, [ref]$rect) | Out-Null
        $origin = New-Object T2D.U+POINT; [T2D.U]::ClientToScreen($p.MainWindowHandle, [ref]$origin) | Out-Null
        $bitmap = New-Object System.Drawing.Bitmap $rect.Right, $rect.Bottom
        $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
        $graphics.CopyFromScreen($origin.X, $origin.Y, 0, 0, $bitmap.Size)
        if (-not $Out) { $Out = Join-Path $env:TEMP 'ttt2-shot.png' }
        $bitmap.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
        "saved $Out ($($rect.Right)x$($rect.Bottom))"
    }
    "key" {
        Front | Out-Null
        foreach ($key in ($Keys -split ',')) { Down $key; Start-Sleep -Milliseconds $HoldMs; Up $key; Start-Sleep -Milliseconds $GapMs }
        "pressed $($Keys -join ',')"
    }
    "hold" {
        Front | Out-Null
        $all = @($Keys -split ',')
        foreach ($key in $all) { Down $key }
        Start-Sleep -Milliseconds $HoldMs
        foreach ($key in $all) { Up $key }
        "held $($all -join '+') for $HoldMs ms"
    }
    "cmd" {
        Remove-Item (Join-Path $data 'state.txt') -ErrorAction SilentlyContinue
        for ($i = 0; $i -lt 100 -and (Test-Path (Join-Path $data 'commands.txt')); $i++) { Start-Sleep -Milliseconds 20 }
        Set-Content (Join-Path $data 'commands.txt') -Value ($Text -split ';' | ForEach-Object { $_.Trim() }) -Encoding ascii
        for ($i = 0; $i -lt 150 -and -not (Test-Path (Join-Path $data 'state.txt')); $i++) { Start-Sleep -Milliseconds 20 }
        if (Test-Path (Join-Path $data 'state.txt')) { Get-Content (Join-Path $data 'state.txt') } else { "(no answer)" }
    }
}
