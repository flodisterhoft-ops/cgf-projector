# CGF Projector: turns a presentation into numbered PNG slides for the
# Pictures tab (sources/presentationimport.cpp runs this with PowerShell 5.1).
#
#   -In       PowerPoint file (.pptx .ppt .pptm .ppsx .pps .odp) or PDF
#   -Out      folder for slide0001.png, slide0002.png, ...
#   -Size     each slide is drawn to fit a Size x Size box (PDF pages come out
#             larger on displays scaled above 100 %; the app fits them again)
#   -Soffice  LibreOffice's soffice.exe, for computers without PowerPoint
#   -Profile  folder for LibreOffice's own settings, kept apart from a
#             LibreOffice the person uses themselves
#   -Engine   auto (PowerPoint, else LibreOffice), powerpoint or libreoffice
#
# PowerPoint files are exported by PowerPoint on this computer. Without
# PowerPoint, LibreOffice turns the file into a PDF first. Hidden slides are
# left out either way. PDF pages are drawn by Windows (Windows.Data.Pdf), so
# PDFs need no Office. Prints "count N" once and "slide I" after each slide.
# A file named "cancel" in -Out stops it after the current slide, with
# PowerPoint closed properly, or stops LibreOffice.
# Exit codes: 0 done, 1 failed (message on stderr), 2 neither PowerPoint nor
# LibreOffice is there.

param(
    [Parameter(Mandatory = $true)][string]$In,
    [Parameter(Mandatory = $true)][string]$Out,
    [int]$Size = 1920,
    [string]$Soffice = '',
    [string]$Profile = '',
    [ValidateSet('auto', 'powerpoint', 'libreoffice')][string]$Engine = 'auto'
)

$ErrorActionPreference = 'Stop'
# UTF-8 without a byte-order mark, so messages in any language reach the app
[Console]::OutputEncoding = New-Object System.Text.UTF8Encoding($false)

function Get-FitSize([double]$w, [double]$h) {
    $scale = [math]::Min($Size / $w, $Size / $h)
    return @([int][math]::Round($w * $scale), [int][math]::Round($h * $scale))
}

function Get-SlidePath([int]$n) {
    return Join-Path $Out ('slide{0:D4}.png' -f $n)
}

function Test-Cancelled {
    return Test-Path (Join-Path $Out 'cancel')
}

function Open-PowerPoint {
    try { return New-Object -ComObject PowerPoint.Application } catch { return $null }
}

function Export-PowerPoint($app) {
    $alerts = $app.DisplayAlerts
    $app.DisplayAlerts = 1    # ppAlertsNone: never wait on a hidden dialog
    $pres = $null
    try {
        # Read-only, untitled = false, no window
        $pres = $app.Presentations.Open($In, -1, 0, 0)
        $fit = Get-FitSize $pres.PageSetup.SlideWidth $pres.PageSetup.SlideHeight
        $slides = @($pres.Slides | Where-Object { $_.SlideShowTransition.Hidden -ne -1 })
        "count $($slides.Count)"
        $n = 0
        foreach ($slide in $slides) {
            if (Test-Cancelled) { break }
            $n++
            $slide.Export((Get-SlidePath $n), 'PNG', $fit[0], $fit[1])
            "slide $n"
        }
    } finally {
        if ($pres) {
            $pres.Close()
            [void][Runtime.InteropServices.Marshal]::ReleaseComObject($pres)
        }
        $app.DisplayAlerts = $alerts
        # Leave PowerPoint running if someone has it open with other files.
        if ($app.Presentations.Count -eq 0) { $app.Quit() }
        [void][Runtime.InteropServices.Marshal]::ReleaseComObject($app)
    }
}

# PowerPoint's default font, Aptos, exists only inside Office. Calibri is on
# every Windows computer and a little narrower, so lines never get longer.
function Write-OfficeProfile {
    $file = Join-Path $Profile 'user\registrymodifications.xcu'
    if (Test-Path -LiteralPath $file) { return }   # LibreOffice keeps these lines
    [void][IO.Directory]::CreateDirectory((Join-Path $Profile 'user'))
    $pairs = ''
    $n = 0
    foreach ($font in @('Aptos', 'Aptos Display', 'Aptos Narrow')) {
        $pairs += '<item oor:path="/org.openoffice.Office.Common/Font/Substitution/FontPairs">' +
            "<node oor:name=`"_$n`" oor:op=`"replace`">" +
            '<prop oor:name="Always" oor:op="fuse"><value>true</value></prop>' +
            '<prop oor:name="OnScreenOnly" oor:op="fuse"><value>false</value></prop>' +
            "<prop oor:name=`"ReplaceFont`" oor:op=`"fuse`"><value>$font</value></prop>" +
            '<prop oor:name="SubstituteFont" oor:op="fuse"><value>Calibri</value></prop></node></item>' + "`n"
        $n++
    }
    $xml = '<?xml version="1.0" encoding="UTF-8"?>' + "`n" +
        '<oor:items xmlns:oor="http://openoffice.org/2001/registry" xmlns:xs="http://www.w3.org/2001/XMLSchema" xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance">' + "`n" +
        '<item oor:path="/org.openoffice.Office.Common/Font/Substitution"><prop oor:name="Replacement" oor:op="fuse"><value>true</value></prop></item>' + "`n" +
        $pairs + '</oor:items>' + "`n"
    [IO.File]::WriteAllText($file, $xml, (New-Object System.Text.UTF8Encoding($false)))
}

# LibreOffice turns the presentation into a PDF; returns the PDF's path, or
# nothing after a cancel.
function Convert-WithLibreOffice {
    # LibreOffice opens almost anything, a text file as one page of text.
    # Presentations are zip files (pptx, odp) or Office's older container (ppt).
    $head = New-Object byte[] 4
    $stream = [IO.File]::OpenRead($In)
    try { [void]$stream.Read($head, 0, 4) } finally { $stream.Dispose() }
    $magic = [BitConverter]::ToString($head)
    if ($magic -ne '50-4B-03-04' -and $magic -ne 'D0-CF-11-E0') { throw 'This file is not a presentation.' }

    $work = Join-Path $Out 'office'
    [void][IO.Directory]::CreateDirectory($work)
    # A copy with a plain name: any file name works, and the original is
    # never locked or touched.
    $copy = Join-Path $work ('deck' + [IO.Path]::GetExtension($In).ToLowerInvariant())
    Copy-Item -LiteralPath $In -Destination $copy
    $arguments = @()
    if ($Profile) {
        Write-OfficeProfile
        $arguments += '"-env:UserInstallation=' + ([Uri]$Profile).AbsoluteUri + '"'
    }
    $arguments += @('--headless', '--norestore', '--nolockcheck', '--convert-to', 'pdf', '--outdir', "`"$work`"", "`"$copy`"")
    $process = Start-Process -FilePath $Soffice -ArgumentList $arguments -WindowStyle Hidden -PassThru
    $started = Get-Date
    while (-not $process.WaitForExit(250)) {
        $late = ((Get-Date) - $started).TotalMinutes -gt 10
        if ((Test-Cancelled) -or $late) {
            # soffice.exe starts soffice.bin: stop both.
            & (Join-Path $env:SystemRoot 'System32\taskkill.exe') /PID $process.Id /T /F 2>&1 | Out-Null
            if ($late) { throw 'LibreOffice took too long to open this file.' }
            return $null
        }
    }
    $pdf = Join-Path $work 'deck.pdf'
    if (-not (Test-Path -LiteralPath $pdf)) { throw 'LibreOffice could not open this file.' }
    return $pdf
}

function Export-Pdf([string]$Path) {
    Add-Type -AssemblyName System.Runtime.WindowsRuntime
    $asTask = [System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object {
        $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and
        $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1' } | Select-Object -First 1
    $asTaskAction = [System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object {
        $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and
        $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncAction' } | Select-Object -First 1
    # A failed task reports "One or more errors occurred"; pass on the reason.
    function Wait-Task($task) {
        try { [void]$task.Wait(-1) } catch { throw $task.Exception.InnerException }
    }
    function Wait-Operation($operation, [type]$type) {
        $task = $asTask.MakeGenericMethod($type).Invoke($null, @($operation))
        Wait-Task $task
        return $task.Result
    }
    function Wait-Action($action) {
        Wait-Task $asTaskAction.Invoke($null, @($action))
    }

    [void][Windows.Storage.StorageFile, Windows.Storage, ContentType = WindowsRuntime]
    [void][Windows.Storage.Streams.InMemoryRandomAccessStream, Windows.Storage.Streams, ContentType = WindowsRuntime]
    [void][Windows.Data.Pdf.PdfDocument, Windows.Data.Pdf, ContentType = WindowsRuntime]
    [void][Windows.Graphics.Imaging.BitmapEncoder, Windows.Graphics.Imaging, ContentType = WindowsRuntime]

    $file = Wait-Operation ([Windows.Storage.StorageFile]::GetFileFromPathAsync($Path)) ([Windows.Storage.StorageFile])
    $doc = Wait-Operation ([Windows.Data.Pdf.PdfDocument]::LoadFromFileAsync($file)) ([Windows.Data.Pdf.PdfDocument])
    "count $($doc.PageCount)"
    for ($i = 0; $i -lt $doc.PageCount; $i++) {
        if (Test-Cancelled) { break }
        $page = $doc.GetPage($i)
        $fit = Get-FitSize $page.Size.Width $page.Size.Height
        $options = New-Object Windows.Data.Pdf.PdfPageRenderOptions
        $options.DestinationWidth = $fit[0]
        $options.DestinationHeight = $fit[1]
        $options.BitmapEncoderId = [Windows.Graphics.Imaging.BitmapEncoder]::PngEncoderId
        $stream = New-Object Windows.Storage.Streams.InMemoryRandomAccessStream
        Wait-Action ($page.RenderToStreamAsync($stream, $options))
        $source = [System.IO.WindowsRuntimeStreamExtensions]::AsStreamForRead($stream.GetInputStreamAt(0))
        $target = [IO.File]::Create((Get-SlidePath ($i + 1)))
        try { $source.CopyTo($target) } finally { $target.Dispose(); $source.Dispose(); $stream.Dispose() }
        $page.Dispose()
        "slide $($i + 1)"
    }
}

try {
    if ([IO.Path]::GetExtension($In).ToLowerInvariant() -eq '.pdf') {
        Export-Pdf $In
    } else {
        $powerPoint = $null
        if ($Engine -ne 'libreoffice') { $powerPoint = Open-PowerPoint }
        if ($powerPoint) {
            Export-PowerPoint $powerPoint
        } elseif ($Engine -ne 'powerpoint' -and $Soffice -and (Test-Path -LiteralPath $Soffice)) {
            $pdf = Convert-WithLibreOffice
            if ($pdf) { Export-Pdf $pdf }
        } else {
            [Console]::Error.WriteLine('Neither PowerPoint nor LibreOffice is on this computer.')
            exit 2
        }
    }
} catch {
    [Console]::Error.WriteLine($_.Exception.Message)
    exit 1
}
