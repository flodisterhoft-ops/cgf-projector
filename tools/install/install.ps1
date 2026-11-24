# Installs CGF Projector for this Windows user. No administrator rights needed.
# Double-click install.cmd beside this file, or run:
#   powershell -ExecutionPolicy Bypass -File install.ps1 [-Target <folder>]
#
# Puts the program in %USERPROFILE%\CGFProjector (the launcher CGFProjector.exe
# and an app-<version> folder), adds desktop and Start menu shortcuts, and
# starts the companion at Windows sign-in so updates arrive while the app is
# closed. Installing again over an existing install keeps its song library,
# settings and device token, and adds the new version beside the old one.
# See docs/song-push.md in the cgf-projector repository.

param(
    [string]$Target = (Join-Path $env:USERPROFILE "CGFProjector"),
    [switch]$NoShortcuts,
    [switch]$NoStart
)
$ErrorActionPreference = "Stop"
$source = Join-Path $PSScriptRoot "CGFProjector"
if (-not (Test-Path (Join-Path $source "CGFProjector.exe"))) {
    throw "The CGFProjector folder is missing next to install.ps1. Unzip the whole package first."
}
$Target = [System.IO.Path]::GetFullPath($Target)

function Read-State($path) {
    $state = [ordered]@{}
    if (Test-Path $path) {
        foreach ($line in Get-Content $path) {
            $i = $line.IndexOf("=")
            if ($i -gt 0) { $state[$line.Substring(0, $i).Trim()] = $line.Substring($i + 1).Trim() }
        }
    }
    return $state
}

function Write-State($path, $state) {
    $lines = foreach ($k in "current", "previous", "seq", "trial", "starts", "bad") { "$k=$($state[$k])" }
    $tmp = "$path.tmp"
    [System.IO.File]::WriteAllText($tmp, (($lines -join "`n") + "`n"), (New-Object System.Text.UTF8Encoding $false))
    Move-Item $tmp $path -Force
}

# The app must be closed; a companion from this install is stopped and restarted.
$open = Get-Process CGFProjector -ErrorAction SilentlyContinue |
    Where-Object { $_.Path -and $_.Path.StartsWith($Target, [System.StringComparison]::OrdinalIgnoreCase) }
if ($open) { throw "Close CGF Projector first, then run the installer again." }
Get-Process cgf-companion -ErrorAction SilentlyContinue |
    Where-Object { $_.Path -and $_.Path.StartsWith($Target, [System.StringComparison]::OrdinalIgnoreCase) } |
    Stop-Process -Force

New-Item -ItemType Directory -Force $Target | Out-Null
$newState = Read-State (Join-Path $source "cgf-versions.txt")
$oldState = Read-State (Join-Path $Target "cgf-versions.txt")
$folder = $newState["current"]
if (-not $folder -or -not (Test-Path (Join-Path $source $folder))) { throw "The package has no app folder." }

# Program files: always the package's. The data files stay as they are.
Copy-Item (Join-Path $source "CGFProjector.exe") $Target -Force
$dest = Join-Path $Target $folder
if (Test-Path $dest) { Remove-Item $dest -Recurse -Force }
Copy-Item (Join-Path $source $folder) $dest -Recurse -Force
foreach ($name in "spData.sqlite", "version.txt", "cgf-cloud.json", "cgf-network.json") {
    $from = Join-Path $source $name
    $to = Join-Path $Target $name
    if ((Test-Path $from) -and -not (Test-Path $to)) { Copy-Item $from $to }
}

$state = [ordered]@{
    current  = $folder
    previous = if ($oldState["current"] -and $oldState["current"] -ne $folder -and (Test-Path (Join-Path $Target $oldState["current"]))) { $oldState["current"] } else { "" }
    seq      = [Math]::Max([int]("0" + $newState["seq"]), [int]("0" + $oldState["seq"]))
    trial    = "0"
    starts   = "0"
    bad      = $oldState["bad"]
}
Write-State (Join-Path $Target "cgf-versions.txt") $state

# A zip downloaded from the internet marks its files; Windows would then warn.
Get-ChildItem $Target -Recurse -File | Unblock-File

if (-not $NoShortcuts) {
    $shell = New-Object -ComObject WScript.Shell
    $exe = Join-Path $Target "CGFProjector.exe"
    $links = @(
        @{ Path = Join-Path ([Environment]::GetFolderPath("Desktop")) "CGF Projector.lnk"; Args = "" },
        @{ Path = Join-Path ([Environment]::GetFolderPath("Programs")) "CGF Projector.lnk"; Args = "" },
        @{ Path = Join-Path ([Environment]::GetFolderPath("Startup")) "CGF Projector updates.lnk"; Args = "--companion" }
    )
    foreach ($l in $links) {
        $s = $shell.CreateShortcut($l.Path)
        $s.TargetPath = $exe
        $s.Arguments = $l.Args
        $s.WorkingDirectory = $Target
        $s.IconLocation = "$exe,0"
        $s.Save()
    }
}

if (-not $NoStart) {
    Start-Process (Join-Path $Target "CGFProjector.exe") -ArgumentList "--companion" -WorkingDirectory $Target
}

Write-Host "CGF Projector $($folder.Substring(4)) is installed in $Target"
if (-not (Test-Path (Join-Path $Target "cgf-cloud.json"))) {
    Write-Host "No cgf-cloud.json: songs and updates are not pushed to this computer."
}
