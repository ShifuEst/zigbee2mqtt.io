$ErrorActionPreference = 'Stop'
$Root = $PSScriptRoot
$Tokens = $null
$ParseErrors = $null
$null = [System.Management.Automation.Language.Parser]::ParseFile((Join-Path $Root 'flash.ps1'), [ref]$Tokens, [ref]$ParseErrors)
if ($ParseErrors.Count) { throw ($ParseErrors | Out-String) }
# No device access: CheckOnly tests the packaged tools against a disposable image.
New-Item -ItemType Directory -Force "$Root\firmware" | Out-Null
$Fixture = "$Root\firmware\MD-GATE-H2-USB.bin"
[System.IO.File]::WriteAllBytes($Fixture, (New-Object byte[] 4194304))
@{chip='esp32h2';profile='USB_ONLY';required_uart_print_control=3;sha256=(Get-FileHash $Fixture -Algorithm SHA256).Hash} | ConvertTo-Json | Set-Content "$Root\firmware\build-manifest.json"
& "$Root\flash.ps1" -CheckOnly
if ($LASTEXITCODE -ne 0) { throw 'CheckOnly failed' }
$Fuse = @(Get-ChildItem "$Root\tools\esptool-5.1.0" -Recurse -Filter espefuse.exe)[0].FullName
$State = "$Root\virtual-efuse.bin"
$BeforePath = "$Root\virtual-before.json"
$AfterPath = "$Root\virtual-after.json"
& $Fuse --chip esp32h2 --virt --path-efuse-file $State summary --format json --file $BeforePath
if ($LASTEXITCODE -ne 0) { throw 'Virtual summary failed' }
$Before = Get-Content $BeforePath -Raw | ConvertFrom-Json
if ($Before.UART_PRINT_CONTROL.value -ne 'Enable' -or !$Before.UART_PRINT_CONTROL.writeable) { throw 'Unexpected fresh virtual field' }
# --do-not-confirm is used ONLY with --virt in this test, never in flash.ps1.
& $Fuse --chip esp32h2 --virt --path-efuse-file $State --do-not-confirm burn-efuse UART_PRINT_CONTROL 3
if ($LASTEXITCODE -ne 0) { throw 'Virtual burn failed' }
& $Fuse --chip esp32h2 --virt --path-efuse-file $State summary --format json --file $AfterPath
if ($LASTEXITCODE -ne 0) { throw 'Virtual readback failed' }
$After = Get-Content $AfterPath -Raw | ConvertFrom-Json
if ($After.UART_PRINT_CONTROL.value -ne 'Disable') { throw 'Virtual readback is not 3' }
if ($After.SPI_BOOT_CRYPT_CNT.value -ne $Before.SPI_BOOT_CRYPT_CNT.value -or $After.SECURE_BOOT_EN.value -ne $Before.SECURE_BOOT_EN.value) { throw 'Other field changed' }
Write-Host 'PASS: Windows PowerShell parse, verified official binaries, CheckOnly path, virtual eFuse transition and readback.'
