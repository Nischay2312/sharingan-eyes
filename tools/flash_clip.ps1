# Flash a .eyv animation clip to the board's 'anim' partition (offset 0x410000).
# Uses the ESP-IDF venv Python directly (same one idf.py/monitor use), so esptool
# is available without activating anything.
#
#   .\tools\flash_clip.ps1 -Port COM5
#   .\tools\flash_clip.ps1 -Port COM5 -Clip .\anim.eyv
#
# NOTE: flash the app first (idf.py ... flash) at least once after the partition
# table changed -- that creates the 'anim' partition this writes into.

param(
    [string]$Port = "COM5",
    [string]$Clip = "$PSScriptRoot\..\anim.eyv",
    [int]$Offset = 0x410000,   # anim partition: 0x10000 factory start + 4 MB factory size
    [int]$Baud = 460800
)

$py = "C:\Users\nisch\.espressif\python_env\idf5.5_py3.11_env\Scripts\python.exe"
if (-not (Test-Path $py)) { $py = "python" }   # fall back to PATH python
if (-not (Test-Path $Clip)) { Write-Error "clip not found: $Clip"; exit 1 }

$hexOffset = "0x{0:X}" -f $Offset
Write-Host "Flashing $Clip -> $Port @ $hexOffset" -ForegroundColor Cyan
& $py -m esptool --chip esp32s3 -p $Port -b $Baud write_flash $hexOffset $Clip
