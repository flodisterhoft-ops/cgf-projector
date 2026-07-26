# Registers the CGF sync agent as a per-user scheduled task running every
# 10 minutes (which also covers "shortly after logon"). Run once per machine
# as the user who operates CGF Projector. Requires cgf-sync.json next to
# cgf-sync-agent.ps1.
#
#   powershell -ExecutionPolicy Bypass -File install-agent.ps1
#
# Uses schtasks.exe: Register-ScheduledTask is denied for non-elevated users
# on some Windows 11 configurations (verified 2026-07-26); schtasks with an
# interval schedule works without elevation.

$agentDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$agent = Join-Path $agentDir "cgf-sync-agent.ps1"
if (-not (Test-Path (Join-Path $agentDir "cgf-sync.json"))) {
    throw "cgf-sync.json not found next to the agent - create it first"
}

schtasks /Create /F /TN "CGF Projector Sync" /SC MINUTE /MO 10 `
    /TR "powershell -NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File \`"$agent\`""
if ($LASTEXITCODE -ne 0) { throw "schtasks failed with exit code $LASTEXITCODE" }
Write-Output "Scheduled task 'CGF Projector Sync' registered (every 10 minutes)."
