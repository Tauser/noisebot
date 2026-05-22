# uninstall.ps1 — Remove o NoiseBot Bridge v2 do Windows Task Scheduler
# Uso: .\uninstall.ps1

$ErrorActionPreference = "Stop"
$TaskName = "NoiseBot Bridge v2"

$task = Get-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue
if (-not $task) {
    Write-Host "Tarefa '$TaskName' nao encontrada. Nada a remover."
    exit 0
}

# Para a tarefa se estiver rodando
if ($task.State -eq "Running") {
    Stop-ScheduledTask -TaskName $TaskName
    Write-Host "Tarefa parada."
}

Unregister-ScheduledTask -TaskName $TaskName -Confirm:$false
Write-Host "Tarefa '$TaskName' removida do Task Scheduler."
