# CGF Projector: turns a presentation into numbered PNG slides for the
# Pictures tab (sources/presentationimport.cpp runs this with PowerShell 5.1).
#
#   -In       PowerPoint file (.pptx .ppt .pptm .ppsx .pps .odp) or PDF
#   -Out      folder for slide0001.png, slide0002.png, ...
#   -Size     each slide is drawn to fit a box Size wide and -Height high
#   -Height   (Size x Size without it; PDF pages come out larger on displays
#             scaled above 100 %; the app fits them again)
#   -Soffice  LibreOffice's soffice.exe, for computers without PowerPoint
#   -Profile  folder for LibreOffice's own settings, kept apart from a
#             LibreOffice the person uses themselves
#   -Engine   auto (PowerPoint, else LibreOffice), powerpoint or libreoffice
#   -ToPptx   instead of drawing slides, save the file as this .pptx (the app
#             reads animations from .pptx; .ppt and .odp are saved as one first)
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
    [int]$Height = 0,
    [string]$Soffice = '',
    [string]$Profile = '',
    [ValidateSet('auto', 'powerpoint', 'libreoffice')][string]$Engine = 'auto',
    [string]$ToPptx = ''
)

$ErrorActionPreference = 'Stop'
# UTF-8 without a byte-order mark, so messages in any language reach the app
[Console]::OutputEncoding = New-Object System.Text.UTF8Encoding($false)

if ($Height -le 0) { $Height = $Size }

function Get-FitSize([double]$w, [double]$h) {
    $scale = [math]::Min($Size / $w, $Height / $h)
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
        if ($ToPptx) {
            $pres.SaveCopyAs($ToPptx, 24)    # ppSaveAsOpenXMLPresentation
            return
        }
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

# Fonts that come with Office (or with Google Slides and Canva) and not with
# Windows, and the Windows font to draw them with: the nearest in width that
# has its own bold and italic, measured with tools/presentations/fonts.py, so
# that lines wrap where they did. Aptos is PowerPoint's default font and
# exists only inside Office. A font this computer has is left alone.
$FontReplacements = @(
    @('Aptos', 'Calibri'), @('Aptos Display', 'Calibri'), @('Aptos Narrow', 'Calibri'),
    @('Garamond', 'Times New Roman'), @('Baskerville Old Face', 'Times New Roman'),
    @('Goudy Old Style', 'Times New Roman'), @('Perpetua', 'Times New Roman'),
    @('Book Antiqua', 'Palatino Linotype'), @('Rockwell', 'Palatino Linotype'),
    @('Bookman Old Style', 'Sitka Text'), @('Century Schoolbook', 'Sitka Text'), @('Calisto MT', 'Cambria'),
    @('Century Gothic', 'Trebuchet MS'), @('Gill Sans MT', 'Corbel'), @('Tw Cen MT', 'Calibri'),
    @('Berlin Sans FB', 'Calibri'), @('Franklin Gothic Book', 'Franklin Gothic Medium'),
    @('Lucida Sans', 'Lucida Sans Unicode'), @('Cooper Black', 'Segoe UI Black'), @('Broadway', 'Arial Black'),
    @('Monotype Corsiva', 'Gabriola'), @('Kunstler Script', 'Gabriola'),
    @('Montserrat', 'Verdana'), @('Montserrat Light', 'Verdana'), @('Montserrat Medium', 'Verdana'),
    @('Montserrat SemiBold', 'Verdana'), @('Montserrat ExtraBold', 'Verdana'), @('Montserrat Black', 'Verdana')
)
$FontReplacementsVersion = '2'      # raise it when the list changes: laptops then get the new list

# Puts the list into LibreOffice's settings (its own, in -Profile), for the
# fonts this computer does not have. LibreOffice only replaces a font for
# drawing a PDF when told to do it always, hence the test here.
function Write-OfficeProfile {
    $installed = @()
    foreach ($hive in 'HKLM:', 'HKCU:') {
        $key = Get-ItemProperty -Path "$hive\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Fonts" -ErrorAction SilentlyContinue
        if ($key) { $installed += $key.PSObject.Properties.Name | ForEach-Object { ($_ -replace '\s*\(.*\)\s*$', '') } }
    }
    $missing = @($FontReplacements | Where-Object {
        $family = $_[0]
        -not ($installed | Where-Object { $_ -eq $family -or $_ -like "$family Bold*" -or $_ -like "$family Italic*" -or $_ -like "$family Regular*" })
    })
    $wanted = $FontReplacementsVersion + ':' + (($missing | ForEach-Object { $_[0] }) -join ',')
    $marker = Join-Path $Profile 'cgf-fonts.txt'
    if ((Test-Path -LiteralPath $marker) -and ([IO.File]::ReadAllText($marker).Trim() -eq $wanted)) { return }
    $file = Join-Path $Profile 'user\registrymodifications.xcu'
    [void][IO.Directory]::CreateDirectory((Join-Path $Profile 'user'))
    $items = '<item oor:path="/org.openoffice.Office.Common/Font/Substitution"><prop oor:name="Replacement" oor:op="fuse"><value>true</value></prop></item>' + "`n"
    $n = 0
    foreach ($pair in $missing) {
        $items += '<item oor:path="/org.openoffice.Office.Common/Font/Substitution/FontPairs">' +
            "<node oor:name=`"_$n`" oor:op=`"replace`">" +
            '<prop oor:name="Always" oor:op="fuse"><value>true</value></prop>' +
            '<prop oor:name="OnScreenOnly" oor:op="fuse"><value>false</value></prop>' +
            "<prop oor:name=`"ReplaceFont`" oor:op=`"fuse`"><value>$($pair[0])</value></prop>" +
            "<prop oor:name=`"SubstituteFont`" oor:op=`"fuse`"><value>$($pair[1])</value></prop></node></item>" + "`n"
        $n++
    }
    $utf8 = New-Object System.Text.UTF8Encoding($false)
    if (Test-Path -LiteralPath $file) {
        # LibreOffice has added lines of its own since: only ours are replaced.
        $xml = [IO.File]::ReadAllText($file, $utf8)
        $xml = [regex]::Replace($xml, '<item oor:path="/org\.openoffice\.Office\.Common/Font/Substitution[^"]*">.*?</item>\s*', '',
                                [Text.RegularExpressions.RegexOptions]::Singleline)
        $xml = $xml.Replace('</oor:items>', $items + '</oor:items>')
    } else {
        $xml = '<?xml version="1.0" encoding="UTF-8"?>' + "`n" +
            '<oor:items xmlns:oor="http://openoffice.org/2001/registry" xmlns:xs="http://www.w3.org/2001/XMLSchema" xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance">' + "`n" +
            $items + '</oor:items>' + "`n"
    }
    [IO.File]::WriteAllText($file, $xml, $utf8)
    [IO.File]::WriteAllText($marker, $wanted, $utf8)
}

# LibreOffice turns the presentation into a PDF (or a .pptx); returns the new
# file's path, or nothing after a cancel.
function Convert-WithLibreOffice([string]$Format = 'pdf') {
    # LibreOffice opens almost anything, a text file as one page of text.
    # Presentations are zip files (pptx, odp) or Office's older container (ppt).
    $head = New-Object byte[] 4
    $stream = [IO.File]::OpenRead($In)
    try { [void]$stream.Read($head, 0, 4) } finally { $stream.Dispose() }
    $magic = [BitConverter]::ToString($head)
    if ($magic -ne '50-4B-03-04' -and $magic -ne 'D0-CF-11-E0') { throw 'This file is not a presentation.' }

    if ($Format -eq [IO.Path]::GetExtension($In).ToLowerInvariant().TrimStart('.')) {
        throw "This file is a .$Format already."
    }
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
    $arguments += @('--headless', '--norestore', '--nolockcheck', '--convert-to', $Format, '--outdir', "`"$work`"", "`"$copy`"")
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
    $made = Join-Path $work "deck.$Format"
    if (-not (Test-Path -LiteralPath $made)) { throw 'LibreOffice could not open this file.' }
    return $made
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
            if ($ToPptx) {
                $made = Convert-WithLibreOffice 'pptx'
                if ($made) { Move-Item -LiteralPath $made -Destination $ToPptx -Force }
            } else {
                $pdf = Convert-WithLibreOffice
                if ($pdf) { Export-Pdf $pdf }
            }
        } else {
            [Console]::Error.WriteLine('Neither PowerPoint nor LibreOffice is on this computer.')
            exit 2
        }
    }
} catch {
    [Console]::Error.WriteLine($_.Exception.Message)
    exit 1
}
