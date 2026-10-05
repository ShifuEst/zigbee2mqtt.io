param([switch]$CheckOnly)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$Root = $PSScriptRoot
Set-Location $Root
New-Item -ItemType Directory -Force -Path "$Root\logs" | Out-Null
$Stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
Start-Transcript -Path "$Root\logs\flash-$Stamp.txt" | Out-Null
try {
    Write-Host 'MakeDIY MD-GATE-ZB1 / TXRX USB / ESP32-H2'
    $ImagePath = Join-Path $Root 'firmware\MD-GATE-H2-USB.bin'
    $Manifest = Get-Content (Join-Path $Root 'firmware\build-manifest.json') -Raw | ConvertFrom-Json
    if ($Manifest.chip -ne 'esp32h2' -or $Manifest.profile -ne 'USB_ONLY' -or $Manifest.required_uart_print_control -ne 3) { throw 'Vale ehitusprofiil.' }
    if ((Get-Item $ImagePath).Length -ne 4194304 -or (Get-FileHash $ImagePath -Algorithm SHA256).Hash -ne $Manifest.sha256) { throw 'Pusivara kontrollsumma voi suurus ei klapi.' }
    $ToolsPath = Join-Path $Root 'tools'
    New-Item -ItemType Directory -Force -Path $ToolsPath | Out-Null
    $ArchivePath = Join-Path $ToolsPath 'esptool-v5.1.0-windows-amd64.zip'
    $ExpectedHash = 'f68a8f7728adfc59cd60f9424928199e76eac66372c7bdc23898aa32753a437a'
    if (!(Test-Path $ArchivePath)) {
        [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
        Write-Host 'Laadin Espressifi ametlikud flashimise tooriistad...'
        Invoke-WebRequest -UseBasicParsing 'https://github.com/espressif/esptool/releases/download/v5.1.0/esptool-v5.1.0-windows-amd64.zip' -OutFile $ArchivePath
    }
    if ((Get-FileHash $ArchivePath -Algorithm SHA256).Hash -ne $ExpectedHash) { throw 'Tooriista ZIP SHA256 ei klapi. Kustuta tools kausta ZIP ja proovi uuesti.' }
    $ExtractPath = Join-Path $ToolsPath 'esptool-5.1.0'
    Expand-Archive $ArchivePath -DestinationPath $ExtractPath -Force
    function Find-Tool([string]$Name) {
        $Items = @(Get-ChildItem $ExtractPath -Recurse -Filter $Name)
        if ($Items.Count -ne 1) { throw "$Name ei ole uheselt leitav." }
        return $Items[0].FullName
    }
    $EspTool = Find-Tool 'esptool.exe'
    $EspFuse = Find-Tool 'espefuse.exe'
    if ($CheckOnly) {
        & $EspTool version
        if ($LASTEXITCODE -ne 0) { throw 'Esptool ei kaivitu.' }
        & $EspFuse --help | Out-Null
        if ($LASTEXITCODE -ne 0) { throw 'Espefuse ei kaivitu.' }
        Write-Host 'PASS: binaar, kontrollsumma ja Windowsi tooriistad. Uhegi seadmega ei uhendatud.'
        return
    }
    Write-Host 'Eemalda H2 releeplaadilt. Uhenda arvutiga ainult H2 USB-andmekaabliga.'
    Write-Host 'Kirjutatakse kogu 4 MB flash. Vana pusivara ja andmed varundatakse enne.'
    Write-Host 'Kui vajalik, kusitakse hiljem BURN: ainult UART_PRINT_CONTROL seatakse pusivalt 3-ks.'
    Write-Host 'See vaigistab ROM-i UART-teated. Varukoopia ega uus flash ei saa eFuse muutust tagasi votta.'
    if ((Read-Host 'Kirjuta H2-ERALDI, et alustada') -cne 'H2-ERALDI') { throw 'Katkestatud enne seadme muutmist.' }
    $Ports = @([System.IO.Ports.SerialPort]::GetPortNames() | Sort-Object)
    if ($Ports.Count -eq 1) {
        $Port = $Ports[0]
        Write-Host "Ainus jadaport: $Port. Kontrollin, et kiip on ESP32-H2."
    } else {
        $Ports | Out-Host
        $Port = (Read-Host 'Sisesta H2 COM-port, nt COM8').Trim().ToUpperInvariant()
    }
    if ($Port -notmatch '^COM[1-9][0-9]*$') { throw 'Vigane COM-port.' }
    $Common = @('--chip','esp32h2','--port',$Port,'--after','no-reset')
    function Run-Esp([string[]]$Arguments) {
        & $EspTool @Common @Arguments
        if ($LASTEXITCODE -ne 0) { throw "Esptool ebaonnestus: $($Arguments[0]). Vaata BOOT/RST juhist." }
    }
    $Info = & $EspTool @Common flash-id 2>&1
    $Rc = $LASTEXITCODE
    $Info | Out-Host
    if ($Rc -ne 0 -or ($Info -join "`n") -notmatch 'Detected flash size:\s*4\s*MB') { throw 'Vaja on tuvastatud ESP32-H2 ja 4 MB flashi.' }
    New-Item -ItemType Directory -Force -Path "$Root\backups" | Out-Null
    $BeforePath = "$Root\backups\H2-$Port-$Stamp-efuse-before.json"
    $AfterPath = "$Root\backups\H2-$Port-$Stamp-efuse-after.json"
    function Read-Fuses([string]$OutputPath) {
        & $EspFuse --chip esp32h2 --port $Port summary --format json --file $OutputPath | Out-Host
        if ($LASTEXITCODE -ne 0 -or !(Test-Path $OutputPath)) { throw 'eFuse lugemine ebaonnestus.' }
        return (Get-Content $OutputPath -Raw | ConvertFrom-Json)
    }
    $Before = Read-Fuses $BeforePath
    if (!$Before.UART_PRINT_CONTROL.readable) { throw 'UART_PRINT_CONTROL ei ole loetav.' }
    if ([int]$Before.SPI_BOOT_CRYPT_CNT.value -ne 0 -or [bool]$Before.SECURE_BOOT_EN.value) { throw 'Seadmel on turvasatted. Skript ei kirjuta kaitstud kiipi ule.' }
    $UartValue = [int]$Before.UART_PRINT_CONTROL.value
    if ($UartValue -lt 0 -or $UartValue -gt 3) { throw 'Ootamatu UART_PRINT_CONTROL vaartus.' }
    if ($UartValue -ne 3 -and !$Before.UART_PRINT_CONTROL.writeable) { throw 'UART-print seade on kirjutuskaitsega; katkestatud.' }
    $BackupPath = "$Root\backups\H2-$Port-$Stamp.bin"
    Run-Esp @('read-flash','0x0','0x400000',$BackupPath)
    if ((Get-Item $BackupPath).Length -ne 4194304) { throw 'Varukoopia suurus ei klapi; kirjutamist ei alustata.' }
    (Get-FileHash $BackupPath -Algorithm SHA256).Hash | Set-Content "$BackupPath.sha256.txt"
    Run-Esp @('write-flash','0x0',$ImagePath)
    Run-Esp @('verify-flash','0x0',$ImagePath)
    if ($UartValue -ne 3) {
        Write-Host 'JARGMINE SAMM ON POORDUMATU: UART_PRINT_CONTROL -> 3.' -ForegroundColor Yellow
        Write-Host 'See keelab ROM-i UART logid. USB flashimist, USB logisid ega UART-i kasutamist programmis see valik ei keela.'
        Write-Host 'Kinnita Espressifi tooriistas BURN. Loobumiseks Ctrl+C; uus programm ei luba enne selle sammu lopetamist releekaske.'
        & $EspFuse --chip esp32h2 --port $Port burn-efuse UART_PRINT_CONTROL 3
        if ($LASTEXITCODE -ne 0) { throw 'eFuse samm ei loppenud. Hoia H2 eraldi. Uuesti kaivitamine loeb seisundi uuesti.' }
    }
    $After = Read-Fuses $AfterPath
    if ([int]$After.UART_PRINT_CONTROL.value -ne 3) { throw 'ROM-i UART vaigistus ei ole kinnitatud.' }
    if ($Before.SPI_BOOT_CRYPT_CNT.value -ne $After.SPI_BOOT_CRYPT_CNT.value -or $Before.SECURE_BOOT_EN.value -ne $After.SECURE_BOOT_EN.value) { throw 'Turvasatte muutus; katkestatud.' }
    Write-Host 'VALMIS: pusivara kirjutatud ja kontrollitud; UART_PRINT_CONTROL=3 kontrollitud.' -ForegroundColor Green
    Write-Host "Vana flash: $BackupPath"
    Write-Host 'Vajuta H2 RST. Jatka juhendi Zigbee ja koormuseta katsega. Tegeliku releeplaadi vastuvotukatse on veel vajalik.'
} catch {
    Write-Host "VIGA: $($_.Exception.Message)" -ForegroundColor Red
    exit 1
} finally { Stop-Transcript | Out-Null }
