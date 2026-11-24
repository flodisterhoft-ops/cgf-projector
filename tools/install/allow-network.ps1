# Lets other devices on the church network reach CGF Projector's network bridge
# (OBS overlay, back screen, phone remote) without Windows Firewall asking again
# after every update. Needs administrator rights once; allow-network.cmd and
# install.ps1 ask for them.
#
# Windows remembers "Allow access" per program path, and every update runs from
# a new app-<version> folder, so it asked again each time. A rule for the
# bridge's port, made by an administrator, covers every version; Windows does
# not ask when such a rule exists. The rules Windows made per version are
# removed, since one of them may block a network type.
#
# Only devices on the same network (or on the church's Tailscale) can connect.

param([int]$Port = 0)
$ErrorActionPreference = "Stop"
$name = "CGF Projector network bridge"

if ($Port -le 0) {
    $Port = 8770
    $settings = Join-Path (Join-Path $env:USERPROFILE "CGFProjector") "cgf-network.json"
    if (Test-Path $settings) {
        $p = (Get-Content $settings -Raw | ConvertFrom-Json).port
        if ($p) { $Port = [int]$p }
    }
}

# The rules Windows created from its prompt, for any installed version.
Get-NetFirewallApplicationFilter |
    Where-Object { $_.Program -like "*\CGFProjector\app-*\CGFProjector.exe" } |
    Get-NetFirewallRule |
    Remove-NetFirewallRule

Get-NetFirewallRule -DisplayName $name -ErrorAction SilentlyContinue | Remove-NetFirewallRule
New-NetFirewallRule -DisplayName $name -Direction Inbound -Action Allow -Protocol TCP -LocalPort $Port `
    -RemoteAddress LocalSubnet, "100.64.0.0/10" -Profile Any `
    -Description "CGF Projector: OBS overlay, back screen and remote on the church network (docs/network-bridge.md)" |
    Out-Null
Write-Host "Windows Firewall now lets the church network reach CGF Projector on port $Port."
