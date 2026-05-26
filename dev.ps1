param(
    [string]$RobotHost = "192.168.1.30",
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

$ServerCommand = @"
Set-Location -LiteralPath $(Quote-Ps $ServerDir)
`$env:NOISEBOT_HOST = $(Quote-Ps $RobotHost)
`$env:NOISEBOT_PORT = $(Quote-Ps ([string]$RobotPort))
`$env:NOISEBOT_ROBOT_HTTP_URL = $(Quote-Ps $RobotHttpUrl)
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

Write-Host "Subindo NoiseBot server e dashboard..." -ForegroundColor Cyan
Write-Host "Server:    http://127.0.0.1:8765"
Write-Host "Dashboard: http://127.0.0.1:5173"
Write-Host "Robo TCP:  $RobotHost`:$RobotPort"
Write-Host "Robo HTTP: $RobotHttpUrl"

Start-Process -FilePath $ShellExe -ArgumentList @("-NoExit", "-Command", $ServerCommand) -WorkingDirectory $ServerDir
Start-Process -FilePath $ShellExe -ArgumentList @("-NoExit", "-Command", $AppCommand) -WorkingDirectory $AppDir

if (-not $NoBrowser) {
    Start-Sleep -Seconds 2
    Start-Process "http://127.0.0.1:5173"
}
