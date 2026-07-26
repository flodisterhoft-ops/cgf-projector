# Registers the CGF sync agent as a per-user scheduled task:
# runs at logon and every 10 minutes. Run this once per machine, as the
# user who operates CGF Projector. Requires cgf-sync.json to exist next
# to cgf-sync-agent.ps1.
#
#   powershell -ExecutionPolicy Bypass -File install-agent.ps1

$agentDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$agent = Join-Path $agentDir "cgf-sync-agent.ps1"
if (-not (Test-Path (Join-Path $agentDir "cgf-sync.json"))) {
    throw "cgf-sync.json not found next to the agent - create it first"
}

$action = New-ScheduledTaskAction -Execute "powershell.exe" `
    -Argument "-NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File `"$agent`""
$triggers = @(
    (New-ScheduledTaskTrigger -AtLogOn),
    (New-ScheduledTaskTrigger -Once -At (Get-Date).AddMinutes(1) `
        -RepetitionInterval (New-TimeSpan -Minutes 10))
)
$settings = New-ScheduledTaskSettingsSet -StartWhenAvailable `
    -DontStopIfGoingOnBatteries -AllowStartIfOnBatteries `
    -MultipleInstances IgnoreNew

Register-ScheduledTask -TaskName "CGF Projector Sync" -Action $action `
    -Trigger $triggers -Settings $settings -Force
Write-Output "Scheduled task 'CGF Projector Sync' registered (logon + every 10 min)."
