# Builds the deck for tests/steps with PowerPoint: slides whose animations,
# transitions and media the import turns into steps (BUILD.md, "Presentation
# steps"). Text is plain ASCII on purpose; tests/presentations covers other
# alphabets.
#
#   slide 1  title, fade transition, no animation                 1 step
#   slide 2  five lines that fade in one per click, push           6 steps
#   slide 3  A flies in from the left (click), B wipes up from
#            the bottom by itself after it, C zooms in (click),
#            A fades out (click), C spins (click, nothing new to
#            see) and D fades in after the spin; wipe transition   6 steps
#   slides 2 and 3 carry custom data ("tags"), which their copies
#   must not share: PowerPoint refuses such a file
#   slide 4  a picture that is there when the slide appears, a
#            second one that appears by itself, a sound on a click,
#            an animated picture; cover transition                 3 steps
#   slide 5  hidden                                                -
#   slide 6  a video nothing starts (only with -Video); cut        2 steps
#   slide 7  closing words, split transition                       1 step
#
# -Transitions makes a second deck, five slides with one still each:
#   slide 1  white, no transition
#   slide 2  dark; uncover to the right in 0.8 s: slide 1 leaves to the right
#   slide 3  white; uncover down in 1 s: slide 2 leaves to the bottom
#   slide 4  vortex in 1.4 s, an effect the app does not have: a fade as long
#   slide 5  a fade of 0.3 s
param(
    [Parameter(Mandatory = $true)][string]$Out,
    [Parameter(Mandatory = $true)][string]$Picture,
    [Parameter(Mandatory = $true)][string]$Sound,
    [Parameter(Mandatory = $true)][string]$Gif,
    [string]$Video = '',
    [string]$Old = '',      # also save in the older .ppt format
    [string]$Transitions = ''
)
$ErrorActionPreference = 'Stop'

# A two-frame animated GIF, 16 x 16: red, then blue.
$frames = 'R0lGODlhEAAQAIEAAP8AAAAAAAAAAAAAACH/C05FVFNDQVBFMi4wAwEAAAAh+QQAMgAAACwAAAAAEAAQAAAIHQABCBxIsKDBgwgTKlzIsKHDhxAjSpxIsaLFgQEBACH5BAEyAAEALAAAAAAQABAAgQAA/wAAAAAAAAAAAAgdAAEIHEiwoMGDCBMqXMiwocOHECNKnEixosWBAQEAOw=='
[IO.File]::WriteAllBytes($Gif, [Convert]::FromBase64String($frames))

$app = New-Object -ComObject PowerPoint.Application
$pres = $app.Presentations.Add(0)
try {
    $click = 1; $with = 2; $after = 3      # msoAnimTrigger
    $appear = 1; $fly = 2; $fade = 10; $wipe = 22; $zoom = 23; $play = 83   # msoAnimEffect

    $s = $pres.Slides.Add(1, 1)
    $s.Shapes[1].TextFrame.TextRange.Text = 'Presentation steps'
    $s.Shapes[2].TextFrame.TextRange.Text = 'A deck for the checks'
    $s.SlideShowTransition.EntryEffect = 3849           # fade smoothly

    $s = $pres.Slides.Add(2, 2)
    $s.Shapes[1].TextFrame.TextRange.Text = 'Five lines'
    $s.Shapes[2].TextFrame.TextRange.Text = "First line`rSecond line`rThird line`rFourth line`rFifth line"
    [void]$s.TimeLine.MainSequence.AddEffect($s.Shapes[2], $fade, 2, $click)    # by first-level paragraph
    $s.Tags.Add('CGF', 'slide two')
    $s.SlideShowTransition.EntryEffect = 3853           # push left: the new slide comes from the right

    $s = $pres.Slides.Add(3, 12)
    $a = $s.Shapes.AddShape(1, 60, 60, 240, 140);   $a.Fill.ForeColor.RGB = 0x0000C0; $a.Name = 'A'
    $b = $s.Shapes.AddShape(1, 360, 60, 240, 140);  $b.Fill.ForeColor.RGB = 0x00A000; $b.Name = 'B'
    $c = $s.Shapes.AddShape(9, 660, 60, 240, 140);  $c.Fill.ForeColor.RGB = 0xC06000; $c.Name = 'C'
    foreach ($shape in $a, $b, $c) { $shape.Line.Visible = 0 }
    $seq = $s.TimeLine.MainSequence
    $e = $seq.AddEffect($a, $fly, 0, $click);  $e.EffectParameters.Direction = 4     # from the left
    $e = $seq.AddEffect($b, $wipe, 0, $after); $e.EffectParameters.Direction = 3     # from the bottom
    $e = $seq.AddEffect($c, $zoom, 0, $click)
    $e = $seq.AddEffect($a, $fade, 0, $click); $e.Exit = -1
    $d = $s.Shapes.AddShape(1, 360, 260, 240, 140); $d.Fill.ForeColor.RGB = 0x800080; $d.Line.Visible = 0; $d.Name = 'D'
    [void]$seq.AddEffect($c, 61, 0, $click)             # spin: an emphasis, nothing appears
    [void]$seq.AddEffect($d, $fade, 0, $after)
    $s.Tags.Add('CGF', 'slide three')
    $s.SlideShowTransition.EntryEffect = 2819           # wipe right: it starts at the left

    $s = $pres.Slides.Add(4, 12)
    [void]$s.Shapes.AddPicture($Picture, 0, -1, 60, 60, 320, 180)
    $second = $s.Shapes.AddPicture($Picture, 0, -1, 420, 60, 320, 180)
    $moving = $s.Shapes.AddPicture($Gif, 0, -1, 60, 300, 96, 96)
    $speaker = $s.Shapes.AddMediaObject2($Sound, 0, -1, 480, 320, 48, 48)
    $seq = $s.TimeLine.MainSequence
    [void]$seq.AddEffect($second, $appear, 0, $after)
    [void]$seq.AddEffect($speaker, $play, 0, $click)
    $s.SlideShowTransition.EntryEffect = 1281           # cover left: the new slide comes from the right

    $s = $pres.Slides.Add(5, 2)
    $s.Shapes[1].TextFrame.TextRange.Text = 'HIDDEN SLIDE'
    $s.SlideShowTransition.Hidden = -1

    $n = 6
    if ($Video) {
        $s = $pres.Slides.Add($n, 11)
        $s.Shapes[1].TextFrame.TextRange.Text = 'A video'
        [void]$s.Shapes.AddMediaObject2($Video, 0, -1, 240, 150, 480, 270)
        $n++
    }

    $s = $pres.Slides.Add($n, 2)
    $s.Shapes[1].TextFrame.TextRange.Text = 'The end'
    $s.Shapes[2].TextFrame.TextRange.Text = 'Thank you'
    $s.SlideShowTransition.EntryEffect = 3587           # split vertical out

    $pres.SaveAs($Out)
    if ($Old) { $pres.SaveCopyAs($Old, 1) }             # ppSaveAsPresentation (.ppt)
    "saved $Out, $($pres.Slides.Count) slides"

    if ($Transitions) {
        $more = $app.Presentations.Add(0)
        try {
            $effects = 0, 2051, 2052, 3863, 3849        # none, uncover right, uncover down, vortex left, fade smoothly
            $lengths = 0, 0.8, 1.0, 1.4, 0.3
            foreach ($i in 0..4) {
                $s = $more.Slides.Add($i + 1, 12)       # blank
                $s.FollowMasterBackground = 0
                $s.Background.Fill.Solid()
                $s.Background.Fill.ForeColor.RGB = if ($i -eq 1) { 0x202020 } else { 0xFFFFFF }
                $box = $s.Shapes.AddShape(1, 380, 220, 200, 100)
                $box.Fill.ForeColor.RGB = 0x808080; $box.Line.Visible = 0
                if ($effects[$i]) {
                    $s.SlideShowTransition.EntryEffect = $effects[$i]
                    $s.SlideShowTransition.Duration = $lengths[$i]
                }
            }
            $more.SaveAs($Transitions)
            "saved $Transitions, $($more.Slides.Count) slides"
        } finally {
            $more.Close()
        }
    }
} finally {
    $pres.Close()
    if ($app.Presentations.Count -eq 0) { $app.Quit() }
}
