# test_flashread_debug.ps1
# FLASH_READ (0x07) diagnostic. Sends FLASH_ON, then FLASH_READ size=8,
# then FLASH_READ size=256 (same as LOGLSMW normally requests).
# Prints and logs to file everything received, accumulated per command
# (not printed line-by-line every 30ms) so nothing gets lost on a small
# console window.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File test_flashread_debug.ps1 -Port COM4

param(
    [string]$Port = "COM4",
    [int]$Baud = 921600
)

function ToHex {
    param([byte[]]$Bytes)
    if (-not $Bytes -or $Bytes.Length -eq 0) { return "" }
    return (($Bytes | ForEach-Object { $_.ToString("X2") }) -join " ")
}

$LogFile = Join-Path $PSScriptRoot "flashread_debug_log.txt"
Remove-Item $LogFile -ErrorAction SilentlyContinue
function Log {
    param([string]$Text, [string]$Color = "White")
    Write-Host $Text -ForegroundColor $Color
    Add-Content -Path $LogFile -Value $Text
}

$port_obj = New-Object System.IO.Ports.SerialPort $Port, $Baud, ([System.IO.Ports.Parity]::None), 8, ([System.IO.Ports.StopBits]::One)
$port_obj.Handshake = [System.IO.Ports.Handshake]::None
$port_obj.Open()

Log "=== Port $Port @ $Baud opened ===" "Cyan"

# HARDCODED bytes (the PowerShell CRC function computed CRC wrong earlier;
# verified independently in Python against the same algorithm and against
# a real known-good ACK vector from project notes: "C0 8C 01 01 00 00 00 00 E7 AD").
[byte[]]$pktOn      = 0xC0,0x8D,0x0E,0x00,0x01,0x00,0x00,0x00,0x46,0x2A
[byte[]]$pktRead8   = 0xC0,0x8D,0x07,0x00,0x02,0x00,0x08,0x00,0x00,0x01,0x00,0x00,0x08,0x00,0x00,0x00,0x09,0xDF
[byte[]]$pktRead256 = 0xC0,0x8D,0x07,0x00,0x03,0x00,0x08,0x00,0x00,0x01,0x00,0x00,0x00,0x01,0x00,0x00,0xB8,0x59

Log ("PKT ON      : " + (ToHex $pktOn))
Log ("PKT READ 8  : " + (ToHex $pktRead8))
Log ("PKT READ 256: " + (ToHex $pktRead256))

function Send-AndDrain {
    param([byte[]]$Packet, [string]$Label, [int]$Seconds)

    Log ""
    Log (">>> " + $Label) "Yellow"
    $port_obj.Write($Packet, 0, $Packet.Length)

    $all = New-Object System.Collections.Generic.List[byte]
    $sw = [Diagnostics.Stopwatch]::StartNew()
    while ($sw.Elapsed.TotalSeconds -lt $Seconds) {
        $n = $port_obj.BytesToRead
        if ($n -gt 0) {
            $buf = New-Object byte[] $n
            $read = $port_obj.Read($buf, 0, $n)
            for ($i = 0; $i -lt $read; $i++) { $all.Add($buf[$i]) }
        }
        Start-Sleep -Milliseconds 30
    }

    if ($all.Count -eq 0) {
        Log "RX: nothing received (0 bytes)" "Red"
    } else {
        Log ("RX bytes total: " + $all.Count) "Green"
        Log ("RX HEX: " + (ToHex $all.ToArray())) "Green"
        $ascii = -join ($all.ToArray() | ForEach-Object { if ($_ -ge 32 -and $_ -le 126) { [char]$_ } else { "." } })
        Log ("RX TXT: " + $ascii) "DarkGreen"
    }
}

Send-AndDrain -Packet $pktOn      -Label "CMD_FLASH_ON (0x0E)"                                     -Seconds 1.5
Send-AndDrain -Packet $pktRead8   -Label "CMD_FLASH_READ (0x07, addr=0x000100, size=8)"             -Seconds 3

# Bisection: find the exact size where it breaks
$sizes = @(
    @{Size=224; Pkt=[byte[]](0xC0,0x8D,0x07,0x00,0x14,0x00,0x08,0x00,0x00,0x01,0x00,0x00,0xE0,0x00,0x00,0x00,0x34,0x98)},
    @{Size=240; Pkt=[byte[]](0xC0,0x8D,0x07,0x00,0x15,0x00,0x08,0x00,0x00,0x01,0x00,0x00,0xF0,0x00,0x00,0x00,0x2C,0x4A)},
    @{Size=250; Pkt=[byte[]](0xC0,0x8D,0x07,0x00,0x16,0x00,0x08,0x00,0x00,0x01,0x00,0x00,0xFA,0x00,0x00,0x00,0x41,0x7E)},
    @{Size=254; Pkt=[byte[]](0xC0,0x8D,0x07,0x00,0x17,0x00,0x08,0x00,0x00,0x01,0x00,0x00,0xFE,0x00,0x00,0x00,0x88,0xFA)},
    @{Size=255; Pkt=[byte[]](0xC0,0x8D,0x07,0x00,0x18,0x00,0x08,0x00,0x00,0x01,0x00,0x00,0xFF,0x00,0x00,0x00,0xED,0xAD)}
)
foreach ($s in $sizes) {
    Send-AndDrain -Packet $s.Pkt -Label ("CMD_FLASH_READ (0x07, addr=0x000100, size=" + $s.Size + ")") -Seconds 3
}

Send-AndDrain -Packet $pktRead256 -Label "CMD_FLASH_READ (0x07, addr=0x000100, size=256, LOGLSMW-style)" -Seconds 5

Log ""
Log "=== Done, closing port ===" "Cyan"
Log ("Full log saved to: " + $LogFile)
$port_obj.Close()
