# CGF Projector: turns a presentation into numbered PNG slides for the
# Pictures tab (sources/presentationimport.cpp runs this with PowerShell 5.1).
#
#   -In    PowerPoint file (.pptx .ppt .pptm .ppsx .pps .odp) or PDF
#   -Out   folder for slide0001.png, slide0002.png, ...
#   -Size  each slide is drawn to fit a Size x Size box (PDF pages come out
#          larger on displays scaled above 100 %; the app fits them again)
#
# PowerPoint files are exported by PowerPoint on this computer; hidden slides
# are left out. PDF pages are drawn by Windows (Windows.Data.Pdf), so PDFs
# need no Office. Prints "count N" once and "slide I" after each slide.
# A file named "cancel" in -Out stops it after the current slide, with
# PowerPoint closed properly.
# Exit codes: 0 done, 1 failed (message on stderr), 2 PowerPoint missing.

param(
    [Parameter(Mandatory = $true)][string]$In,
    [Parameter(Mandatory = $true)][string]$Out,
    [int]$Size = 1920
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

function Export-PowerPoint {
    try {
        $app = New-Object -ComObject PowerPoint.Application
    } catch {
        [Console]::Error.WriteLine('PowerPoint is not installed on this computer.')
        exit 2
    }
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

function Export-Pdf {
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

    $file = Wait-Operation ([Windows.Storage.StorageFile]::GetFileFromPathAsync($In)) ([Windows.Storage.StorageFile])
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
        Export-Pdf
    } else {
        Export-PowerPoint
    }
} catch {
    [Console]::Error.WriteLine($_.Exception.Message)
    exit 1
}
