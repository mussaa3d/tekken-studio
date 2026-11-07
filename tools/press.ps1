# Test helper: presses keys the way a keyboard does, for what the desktop tools can't send, like Insert. Names: insert, space, esc, home, the arrows, f4..f9, period, comma, the brackets, or a character.
param([string[]]$Keys, [int]$HoldMs = 80)
Add-Type -Name K -Namespace T8 -MemberDefinition '[DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, System.UIntPtr extra); [DllImport("user32.dll")] public static extern uint MapVirtualKeyW(uint code, uint type);'
$map = @{ insert = 0x2D; f4 = 0x73; up = 0x26; down = 0x28; left = 0x25; right = 0x27; f5 = 0x74; f6 = 0x75; f7 = 0x76; f8 = 0x77; f9 = 0x78; period = 0xBE; lbracket = 0xDB; rbracket = 0xDD; space = 0x20; comma = 0xBC; home = 0x24; esc = 0x1B }
foreach ($key in ($Keys -split ',')) {
    $vk = if ($map.ContainsKey($key)) { $map[$key] } else { [int][char]$key.ToUpper() }
    $extended = if (@('insert', 'home', 'up', 'down', 'left', 'right') -contains $key) { 1 } else { 0 }
    $scan = [T8.K]::MapVirtualKeyW($vk, 0)
    [T8.K]::keybd_event([byte]$vk, [byte]$scan, [uint32]$extended, [System.UIntPtr]::Zero)
    Start-Sleep -Milliseconds $HoldMs
    [T8.K]::keybd_event([byte]$vk, [byte]$scan, [uint32]($extended -bor 2), [System.UIntPtr]::Zero)
    Start-Sleep -Milliseconds 250
}
