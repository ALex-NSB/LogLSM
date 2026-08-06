# check_bootsec.ps1 — проверка секции загрузчика в собранном образе LOGLSMA.
#
# ЗАЧЕМ. Загрузчик живёт в .bootsec (стр.113..120, 0x08038800..0x0803C7FF) и
# работает в тот момент, когда область приложения СТЁРТА. Любая ссылка оттуда
# вниз — в код или константы приложения — это падение посреди заливки с лечением
# по SWD. Ссылка появляется незаметно: компилятор сам подставляет memcpy при
# копировании структуры, помощники деления, __stack_chk_fail при
# -fstack-protector. Глазами такое не ловится, отсюда скрипт.
#
# ЗАПУСК: Scripts\check_bootsec.bat   (или напрямую этим файлом)
# Ничего ставить не нужно — только PowerShell и objdump из STM32CubeCLT.

param(
    [string]$Elf     = "",
    [string]$Objdump = "",
    [string]$Dump    = ""
)

# --- Карта. Должна совпадать с App/Inc/boot.h и STM32L433XX_FLASH.ld ---------
$APP_BASE  = 0x08000000
$BOOT_BASE = 0x08038800
$BOOT_SIZE = 16 * 1024
$BOOT_END  = $BOOT_BASE + $BOOT_SIZE      # 0x0803C800 — первый адрес ЗА секцией

$root = Split-Path -Parent $PSScriptRoot   # корень проекта (Scripts\..)
$problems = 0

# --- Образ ------------------------------------------------------------------
if (-not $Elf) {
    foreach ($cfg in @("Debug", "Release")) {
        $p = Join-Path $root "Firmware\LOGLSMA\build\$cfg\LOGLSMA.elf"
        if (Test-Path $p) { $Elf = $p; break }
    }
}
if (-not $Elf -or -not (Test-Path $Elf)) {
    Write-Host "Не найден собранный образ. Искал в Firmware\LOGLSMA\build\{Debug,Release}\LOGLSMA.elf"
    Write-Host "Соберите проект или укажите -Elf ПУТЬ"
    exit 1
}
Write-Host "Образ: $((Resolve-Path $Elf).Path)"

# --- objdump ----------------------------------------------------------------
if (-not $Objdump) {
    $cand = Get-Command arm-none-eabi-objdump.exe -ErrorAction SilentlyContinue
    if ($cand) { $Objdump = $cand.Source }
    else {
        $p = "D:\ST\STM32Cube\STM32CubeCLT_1.21.0\GNU-tools-for-STM32\bin\arm-none-eabi-objdump.exe"
        if (Test-Path $p) { $Objdump = $p }
    }
}
if (-not $Objdump -or -not (Test-Path $Objdump)) {
    Write-Host "Не найден arm-none-eabi-objdump. Укажите -Objdump ПУТЬ"
    exit 1
}

# --- 1. Секция: адрес и размер ----------------------------------------------
$head = & $Objdump -h $Elf
$m = $head | Select-String -Pattern '^\s*\d+\s+\.bootsec\s+([0-9a-f]+)\s+([0-9a-f]+)'
if (-not $m) {
    Write-Host " ОШИБКА  секции .bootsec нет в образе — загрузчик не собран?"
    exit 1
}
$size = [Convert]::ToInt64($m.Matches[0].Groups[1].Value, 16)
$addr = [Convert]::ToInt64($m.Matches[0].Groups[2].Value, 16)
"Секция .bootsec: 0x{0:X8}, {1} Б ({2:N1} КБ из {3} КБ, свободно {4} Б)" -f `
    $addr, $size, ($size / 1024), ($BOOT_SIZE / 1024), ($BOOT_SIZE - $size) | Write-Host
if ($addr -ne $BOOT_BASE) {
    "  ОШИБКА  ожидался адрес 0x{0:X8}" -f $BOOT_BASE | Write-Host; $problems++
}
if ($size -gt $BOOT_SIZE) {
    Write-Host "  ОШИБКА  секция не влезает в отведённые $BOOT_SIZE Б"; $problems++
}

# --- 2. Что лежит в секции --------------------------------------------------
$syms = & $Objdump -t $Elf
$inside  = @()
$foreign = @()
foreach ($line in $syms) {
    $sm = [regex]::Match($line, '^([0-9a-f]{8})\s.{7}\s+(\S+)\s+([0-9a-f]+)\s+(\S+)$')
    if (-not $sm.Success) { continue }
    $a    = [Convert]::ToInt64($sm.Groups[1].Value, 16)
    $sect = $sm.Groups[2].Value
    $nm   = $sm.Groups[4].Value
    if ($sect -eq ".bootsec") { $inside += ,@($a, $nm) }
    elseif ($a -ge $BOOT_BASE -and $a -lt $BOOT_END -and $sect -ne "*ABS*") { $foreign += ,@($a, $nm, $sect) }
}
Write-Host "Символов в секции: $($inside.Count)"
if ($foreign.Count -gt 0) {
    Write-Host "  ОШИБКА  в диапазоне секции лежит чужое:"
    foreach ($f in $foreign | Select-Object -First 10) { "        0x{0:X8} {1} (секция {2})" -f $f[0], $f[1], $f[2] | Write-Host }
    $problems++
}

# --- 3. Точка входа в первом слове ------------------------------------------
$entrySym = ($inside | Where-Object { $_[1] -eq "boot_loader_run" } | Select-Object -First 1)
$sdump = & $Objdump -s -j .bootsec $Elf
# ⚠ objdump печатает адрес БЕЗ ведущих нулей («8038800», не «08038800»),
# поэтому формат без ширины плюс необязательные нули в начале.
$dm = $sdump | Select-String -Pattern ("^\s*0*{0:x}\s+([0-9a-f]{{8}})" -f $BOOT_BASE)
if ($dm) {
    $raw  = $dm.Matches[0].Groups[1].Value          # байты как в файле (little-endian)
    $word = [Convert]::ToInt64(($raw.Substring(6,2) + $raw.Substring(4,2) + $raw.Substring(2,2) + $raw.Substring(0,2)), 16)
    $okRange = ($word -ge $BOOT_BASE -and $word -lt $BOOT_END)
    $okThumb = (($word -band 1) -ne 0)
    $okSym   = (-not $entrySym) -or ((($word -band -2)) -eq (($entrySym[0] -band -2)))
    $note = ""
    if (-not $okThumb) { $note += "  (нет бита Thumb!)" }
    if (-not $okRange) { $note += "  (вне секции!)" }
    if (-not $okSym)   { $note += "  (не совпадает с boot_loader_run!)" }
    $tag = if ($okRange -and $okThumb -and $okSym) { "  ok  " } else { " ОШИБКА " }
    "{0} точка входа в первом слове: 0x{1:X8}{2}" -f $tag, $word, $note | Write-Host
    if ($note) { $problems++ }
} else {
    Write-Host "  ?  не удалось прочитать первое слово секции"
}

# --- 4. Ссылки наружу — главное ---------------------------------------------
$dis = & $Objdump -d ("--start-address=0x{0:X}" -f $BOOT_BASE) ("--stop-address=0x{0:X}" -f $BOOT_END) $Elf

if (-not $Dump) { $Dump = Join-Path (Split-Path -Parent (Resolve-Path $Elf).Path) "bootsec.txt" }
try { $dis | Set-Content -Path $Dump -Encoding UTF8; Write-Host "Дизассемблер секции: $Dump" }
catch { Write-Host "  ?  не удалось сохранить дизассемблер: $_" }

$badBranch  = @()
$badLiteral = @()
foreach ($line in $dis) {
    $lm = [regex]::Match($line, '^\s*([0-9a-f]+):\s+[0-9a-f ]+\s+(\S+)\s+(.*)$')
    if (-not $lm.Success) { continue }
    $pc   = [Convert]::ToInt64($lm.Groups[1].Value, 16)
    $mnem = $lm.Groups[2].Value
    $rest = $lm.Groups[3].Value

    # переходы: bl / b / blx / b.w / beq… — цель первым шестнадцатеричным числом
    if ($mnem -match '^(bl|b|cb)') {
        $tm = [regex]::Match($rest, '^([0-9a-f]{4,8})\s')
        if ($tm.Success) {
            $tgt = [Convert]::ToInt64($tm.Groups[1].Value, 16)
            if ($tgt -ge $APP_BASE -and $tgt -lt $BOOT_BASE) { $badBranch += ,@($pc, "$mnem $($rest.Trim())") }
        }
    }
    # литеральные пулы: objdump дописывает «; 0x8001234 <symbol>»
    foreach ($hm in [regex]::Matches($rest, '; 0x([0-9a-f]{6,8})')) {
        $val = [Convert]::ToInt64($hm.Groups[1].Value, 16)
        if ($val -ge $APP_BASE -and $val -lt $BOOT_BASE) { $badLiteral += ,@($pc, $rest.Trim()) }
    }
}

if ($badBranch.Count -gt 0) {
    Write-Host "  ОШИБКА  переходы из секции в область приложения ($($badBranch.Count)):"
    foreach ($b in $badBranch | Select-Object -First 15) { "        0x{0:X8}  {1}" -f $b[0], $b[1] | Write-Host }
    $problems++
} else { Write-Host "  ok   переходов в область приложения нет" }

if ($badLiteral.Count -gt 0) {
    Write-Host "  ОШИБКА  адреса приложения в литеральных пулах ($($badLiteral.Count)):"
    foreach ($b in $badLiteral | Select-Object -First 15) { "        0x{0:X8}  {1}" -f $b[0], $b[1] | Write-Host }
    $problems++
} else { Write-Host "  ok   адресов приложения в литеральных пулах нет" }

Write-Host ""
if ($problems -gt 0) {
    Write-Host "ЗАМЕЧАНИЙ: $problems. Заливка может упасть на середине — разобрать до прошивки."
    exit 1
} else {
    Write-Host "Секция чистая: загрузчик ни на что за своими пределами не опирается."
    exit 0
}
