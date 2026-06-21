param(
    [string]$RobotHost = "192.168.1.25",
    [int]$RobotPort = 9000,
    [string]$RobotHttpUrl = "",
    [string]$Python = "",
    [string]$EnvFile = "",
    [switch]$NoBrowser
)

$ErrorActionPreference = "Stop"

function Quote-Ps([string]$Value) {
    return "'" + ($Value -replace "'", "''") + "'"
}

function Resolve-Tool([string[]]$Candidates) {
    foreach ($candidate in $Candidates) {
        if ([string]::IsNullOrWhiteSpace($candidate)) {
            continue
        }
        $command = Get-Command $candidate -ErrorAction SilentlyContinue
        if ($command) {
            return $command.Source
        }
        if (Test-Path -LiteralPath $candidate) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }
    return $null
}

function Get-LanAddress() {
    try {
        $gatewayCandidate = Get-NetIPConfiguration -ErrorAction Stop |
            Where-Object {
                $_.IPv4DefaultGateway -and
                $_.IPv4Address -and
                $_.IPv4Address.IPAddress -notlike "127.*" -and
                $_.IPv4Address.IPAddress -notlike "169.254.*"
            } |
            Sort-Object InterfaceMetric |
            Select-Object -First 1

        if ($gatewayCandidate) {
            return $gatewayCandidate.IPv4Address.IPAddress
        }
    } catch {
        # Ambientes restritos podem negar CIM/NetTCPIP; o fallback por ipconfig cobre Windows comum.
    }

    try {
        $candidates = Get-NetIPAddress -AddressFamily IPv4 -ErrorAction Stop |
            Where-Object {
                $_.IPAddress -notlike "127.*" -and
                $_.IPAddress -notlike "169.254.*" -and
                $_.PrefixOrigin -ne "WellKnown"
            } |
            Sort-Object @{
                Expression = { if ($_.IPAddress -like "192.168.1.*") { 0 } elseif ($_.IPAddress -like "192.168.*") { 1 } else { 2 } }
            }, InterfaceMetric

        $first = $candidates | Select-Object -First 1
        if ($first) {
            return $first.IPAddress
        }
    } catch {
    }

    $ipconfig = ipconfig
    foreach ($line in $ipconfig) {
        if ($line -match "IPv4.*:\s*([0-9]+\.[0-9]+\.[0-9]+\.[0-9]+)") {
            $ip = $Matches[1]
            if ($ip -like "192.168.1.*") {
                return $ip
            }
        }
    }
    foreach ($line in $ipconfig) {
        if ($line -match "IPv4.*:\s*([0-9]+\.[0-9]+\.[0-9]+\.[0-9]+)") {
            $ip = $Matches[1]
            if ($ip -notlike "127.*" -and $ip -notlike "169.254.*") {
                return $ip
            }
        }
    }
    return ""
}

function Get-ListeningPids([int]$Port) {
    $pids = @()
    $lines = netstat -ano | Select-String -Pattern "^\s*TCP\s+\S+:$Port\s+\S+\s+LISTENING\s+(\d+)\s*$"
    foreach ($line in $lines) {
        if ($line.Line -match "^\s*TCP\s+\S+:$Port\s+\S+\s+LISTENING\s+(\d+)\s*$") {
            $pids += [int]$Matches[1]
        }
    }
    return @($pids | Sort-Object -Unique)
}

$RootDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$ServerDir = Join-Path $RootDir "server"
$AppDir = Join-Path $RootDir "app"

if (-not (Test-Path -LiteralPath $ServerDir)) {
    throw "Diretorio server nao encontrado: $ServerDir"
}
if (-not (Test-Path -LiteralPath $AppDir)) {
    throw "Diretorio app nao encontrado: $AppDir"
}

if ([string]::IsNullOrWhiteSpace($RobotHttpUrl)) {
    $RobotHttpUrl = "http://$RobotHost"
}
if ([string]::IsNullOrWhiteSpace($EnvFile)) {
    $EnvFile = Join-Path $ServerDir ".env"
}

$PythonExe = Resolve-Tool @(
    $Python,
    "C:\Users\Tauser\AppData\Local\Python\pythoncore-3.14-64\python.exe",
    "C:\Users\Tauser\.platformio\penv\Scripts\python.exe",
    "python",
    "py"
)
if (-not $PythonExe) {
    throw "Python nao encontrado. Use: .\dev.ps1 -Python C:\caminho\python.exe"
}

$ShellExe = Resolve-Tool @("pwsh", "powershell")
if (-not $ShellExe) {
    throw "PowerShell nao encontrado."
}

$LanAddress = Get-LanAddress
$HostName = hostname
$ServerPids = @(Get-ListeningPids -Port 8765)
$AppPids = @(Get-ListeningPids -Port 5173)

$ServerCommand = @"
Set-Location -LiteralPath $(Quote-Ps $ServerDir)
`$env:PYTHONPATH = $(Quote-Ps $ServerDir)
`$env:NOISEBOT_HOST = $(Quote-Ps $RobotHost)
`$env:NOISEBOT_PORT = $(Quote-Ps ([string]$RobotPort))
`$env:NOISEBOT_ROBOT_HTTP_URL = $(Quote-Ps $RobotHttpUrl)
`$env:NOISEBOT_VISION = '1'
& $(Quote-Ps $PythonExe) -m noisebot_server --host $(Quote-Ps $RobotHost) --port $RobotPort --env $(Quote-Ps $EnvFile) --log-file stderr
"@

$AppCommand = @"
Set-Location -LiteralPath $(Quote-Ps $AppDir)
if (Get-Command pnpm -ErrorAction SilentlyContinue) {
    pnpm dev
} elseif (Test-Path -LiteralPath '.\node_modules\.bin\vite.cmd') {
    .\node_modules\.bin\vite.cmd --host 0.0.0.0
} else {
    npm run dev
}
"@

function Stop-ListeningPids([int]$Port, [string]$Label) {
    $pids = @(Get-ListeningPids -Port $Port)
    foreach ($p in $pids) {
        try {
            Stop-Process -Id $p -Force -ErrorAction Stop
            Write-Host "$Label : PID $p encerrado." -ForegroundColor DarkYellow
        } catch {
            Write-Host "$Label : nao foi possivel encerrar PID $p — $_" -ForegroundColor Red
        }
    }
    if ($pids.Count -gt 0) {
        Start-Sleep -Milliseconds 600
    }
}

Write-Host "Subindo NoiseBot server e dashboard..." -ForegroundColor Cyan

Stop-ListeningPids -Port 8765 -Label "Server   "
Stop-ListeningPids -Port 5173 -Label "Dashboard"

if (-not [string]::IsNullOrWhiteSpace($LanAddress)) {
    Write-Host "Celular:   http://$LanAddress`:5173"
}
if (-not [string]::IsNullOrWhiteSpace($HostName)) {
    Write-Host "Alias:     http://$HostName.local`:5173"
    Write-Host "Alias dev: http://jarvis.local`:5173"
}
Write-Host "Robo TCP:  $RobotHost`:$RobotPort"
Write-Host "Robo HTTP: $RobotHttpUrl"

Start-Process -FilePath $ShellExe -ArgumentList @("-NoExit", "-Command", $ServerCommand) -WorkingDirectory $ServerDir
Start-Process -FilePath $ShellExe -ArgumentList @("-NoExit", "-Command", $AppCommand) -WorkingDirectory $AppDir

if (-not $NoBrowser) {
    Start-Sleep -Seconds 2
    Start-Process "http://127.0.0.1:5173"
}
