# install.ps1 — Instala o NoiseBot Bridge v2 no Windows Task Scheduler
# Uso: .\install.ps1
# Recomendado: executar como o usuario que vai rodar o bridge (nao como Admin)

$ErrorActionPreference = "Stop"

$TaskName   = "NoiseBot Bridge v2"
$Python     = (Get-Command python -ErrorAction SilentlyContinue)?.Source
if (-not $Python) {
    $Python = (Get-Command python3 -ErrorAction SilentlyContinue)?.Source
}
if (-not $Python) {
    Write-Error "Python nao encontrado no PATH. Instale o Python e adicione ao PATH."
    exit 1
}

$WorkDir = $HOME
$LogDir  = Join-Path $HOME ".bridgev2\logs"
New-Item -ItemType Directory -Force -Path $LogDir | Out-Null

Write-Host "Python: $Python"
Write-Host "WorkDir: $WorkDir"
Write-Host "LogDir: $LogDir"

$Action = New-ScheduledTaskAction `
    -Execute $Python `
    -Argument "-m bridgev2" `
    -WorkingDirectory $WorkDir

$Trigger = New-ScheduledTaskTrigger -AtStartup

$Settings = New-ScheduledTaskSettingsSet `
    -ExecutionTimeLimit ([TimeSpan]::Zero) `
    -RestartCount 999 `
    -RestartInterval (New-TimeSpan -Seconds 5) `
    -StartWhenAvailable $true `
    -DisallowDemandStart $false

$Principal = New-ScheduledTaskPrincipal `
    -UserId $env:USERNAME `
    -LogonType Interactive `
    -RunLevel Highest

Register-ScheduledTask `
    -TaskName $TaskName `
    -Action $Action `
    -Trigger $Trigger `
    -Settings $Settings `
    -Principal $Principal `
    -Force | Out-Null

Write-Host ""
Write-Host "Servico instalado: '$TaskName'"
Write-Host ""
Write-Host "Comandos uteis:"
Write-Host "  Iniciar agora : Start-ScheduledTask  -TaskName '$TaskName'"
Write-Host "  Parar         : Stop-ScheduledTask   -TaskName '$TaskName'"
Write-Host "  Status        : Get-ScheduledTask    -TaskName '$TaskName' | Select State"
Write-Host "  Remover       : .\uninstall.ps1"
Write-Host ""
Write-Host "Ou via CLI do bridge:"
Write-Host "  python -m bridgev2 service start"
Write-Host "  python -m bridgev2 service status"
