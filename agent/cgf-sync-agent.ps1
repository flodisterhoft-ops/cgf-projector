# CGF Projector sync agent — snapshot download, verify, atomic swap.
#
# Reads cgf-sync.json next to this script:
#   {
#     "source":  "<publish dir path or https URL>",
#     "appDir":  "<folder containing CGFProjector.exe and spData.sqlite>",
#     "sqlite3": "<optional path to sqlite3.exe for carry-forward>",
#     "keep":    3
#   }
#
# Behavior (design: cgf-projector AGENTS.md):
#  - Compares manifest content_version against appDir\version.txt; exits if same.
#  - Downloads to appDir\staging\, verifies sha256 against the manifest.
#  - If CGFProjector.exe is running, leaves the verified file staged and exits
#    (next run while the app is closed completes the swap). Projection is
#    never disturbed; the app never sees a half-written file.
#  - Carry-forward (needs sqlite3): local Settings rows, local quick-add
#    songs (id > 500000 - the reserved client band), and the operators' own
#    Media library entries and slideshows (Media, SlideShows, Slides) are
#    copied into the new snapshot before the swap, in one transaction. The
#    published snapshot never contains media or slideshows; rows it does
#    contain are kept and same-id local rows are skipped. Without sqlite3 the
#    swap still proceeds; local data remains recoverable from the .prev files.
#  - Rotates spData.prev-1..keep, then atomically replaces spData.sqlite.
#  - Stamps version.txt and appends to sync.log. All failures leave the
#    current database untouched.

$ErrorActionPreference = "Stop"
$agentDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$cfg = Get-Content (Join-Path $agentDir "cgf-sync.json") -Raw | ConvertFrom-Json
$appDir = $cfg.appDir
$log = Join-Path $appDir "sync.log"

function Log($msg) {
    $line = "$(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')  $msg"
    Add-Content -Path $log -Value $line
    Write-Output $line
}

function Fetch($name, $dest) {
    if ($cfg.source -match '^https?://') {
        Invoke-WebRequest -Uri "$($cfg.source.TrimEnd('/'))/$name" -OutFile $dest -UseBasicParsing
    } else {
        Copy-Item (Join-Path $cfg.source $name) $dest -Force
    }
}

try {
    $staging = Join-Path $appDir "staging"
    New-Item -ItemType Directory -Force $staging | Out-Null

    # 1. Manifest
    $manPath = Join-Path $staging "manifest.json"
    Fetch "manifest.json" $manPath
    $man = Get-Content $manPath -Raw | ConvertFrom-Json

    $verFile = Join-Path $appDir "version.txt"
    $local = 0
    if (Test-Path $verFile) { $local = [int](Get-Content $verFile -TotalCount 1) }
    if ($man.content_version -le $local) {
        Log "up to date (local v$local, remote v$($man.content_version))"
        exit 0
    }

    # 2. Download + verify (skip if an already-verified copy is staged)
    $staged = Join-Path $staging $man.file
    $ok = (Test-Path $staged) -and ((Get-FileHash $staged -Algorithm SHA256).Hash.ToLower() -eq $man.sha256.ToLower())
    if (-not $ok) {
        Log "downloading v$($man.content_version) ($([math]::Round($man.bytes/1MB)) MB)"
        # A journal left by an interrupted carry-forward must never be applied
        # to a freshly downloaded file.
        Remove-Item "$staged-journal" -Force -ErrorAction SilentlyContinue
        Fetch $man.file $staged
        $hash = (Get-FileHash $staged -Algorithm SHA256).Hash.ToLower()
        if ($hash -ne $man.sha256.ToLower()) {
            Remove-Item $staged -Force
            throw "sha256 mismatch - download discarded"
        }
    }
    Log "verified v$($man.content_version) sha256 OK"

    # 3. Never swap under a running app - stage and retry later
    if (Get-Process CGFProjector -ErrorAction SilentlyContinue) {
        Log "app is running - staged for next run, current database untouched"
        exit 0
    }

    $db = Join-Path $appDir "spData.sqlite"

    # 4. Carry forward local state into the staged snapshot (before swap)
    if ((Test-Path $db) -and $cfg.sqlite3 -and (Test-Path $cfg.sqlite3)) {
        $dbSql = ($db -replace '\\','/') -replace "'", "''"  # SQL string literal
        $sql = @"
ATTACH DATABASE '$dbSql' AS old;
BEGIN;
DELETE FROM Settings;
INSERT INTO Settings SELECT * FROM old.Settings;
INSERT INTO Songs SELECT * FROM old.Songs WHERE id > 500000;
INSERT INTO Media SELECT o.* FROM old.Media o
  WHERE NOT EXISTS (SELECT 1 FROM main.Media m WHERE m.long_path IS o.long_path);
INSERT INTO SlideShows SELECT o.* FROM old.SlideShows o
  WHERE NOT EXISTS (SELECT 1 FROM main.SlideShows s WHERE s.id = o.id);
INSERT INTO Slides SELECT o.* FROM old.Slides o
  WHERE NOT EXISTS (SELECT 1 FROM main.Slides s WHERE s.id = o.id);
COMMIT;
DETACH DATABASE old;
"@
        # Run the SQL from a BOM-free UTF-8 file. Piping the text into sqlite3
        # let Windows PowerShell prepend a byte-order mark, which broke the
        # first line (the ATTACH): the merge then failed on every run and no
        # new snapshot could ever be installed. -bail stops at the first error
        # with the transaction uncommitted.
        $sqlFile = Join-Path $staging "carry-forward.sql"
        [System.IO.File]::WriteAllText($sqlFile, $sql, (New-Object System.Text.UTF8Encoding $false))
        Push-Location $staging
        try { & $cfg.sqlite3 -bail $staged ".read carry-forward.sql" } finally { Pop-Location }
        $exit = $LASTEXITCODE
        Remove-Item $sqlFile -Force -ErrorAction SilentlyContinue
        if ($exit -ne 0) {
            # Discard the possibly half-written copy so the next run starts clean.
            Remove-Item $staged, "$staged-journal" -Force -ErrorAction SilentlyContinue
            throw "carry-forward failed (sqlite3 exit $exit) - staged copy discarded"
        }
        $carried = & $cfg.sqlite3 $staged "select (select count(*) from Songs where id > 500000) || ' local quick-adds, ' || (select count(*) from Media) || ' media files, ' || (select count(*) from SlideShows) || ' slideshows';"
        Log "carry-forward OK (local settings kept; $carried preserved)"
    } elseif (Test-Path $db) {
        Log "WARN no sqlite3 configured - swapping without carry-forward (old DB kept as .prev-1)"
    }

    # 5. Rotate previous versions, then swap
    $keep = if ($cfg.keep) { [int]$cfg.keep } else { 3 }
    if (Test-Path $db) {
        for ($i = $keep; $i -ge 2; $i--) {
            $a = "$db.prev-$($i-1)"; $b = "$db.prev-$i"
            if (Test-Path $a) { Move-Item $a $b -Force }
        }
        Move-Item $db "$db.prev-1" -Force
    }
    Remove-Item "$db-wal","$db-shm" -Force -ErrorAction SilentlyContinue
    Move-Item $staged $db -Force
    Set-Content -Path $verFile -Value $man.content_version
    Log "INSTALLED v$($man.content_version)"
}
catch {
    Log "ERROR $($_.Exception.Message)"
    exit 1
}
