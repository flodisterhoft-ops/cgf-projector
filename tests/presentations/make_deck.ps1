# Builds the deck for tests/presentations with PowerPoint: text in German and
# Russian (written as character codes, so this file stays plain ASCII), a
# picture, shapes with effects, a table and a hidden slide. Seven
# slides, six of them shown. -Pdf also saves it as a PDF.
param([Parameter(Mandatory = $true)][string]$Out, [Parameter(Mandatory = $true)][string]$Picture, [string]$Pdf = '')
$ErrorActionPreference = 'Stop'
$app = New-Object -ComObject PowerPoint.Application
$pres = $app.Presentations.Add(0)
try {
    $s = $pres.Slides.Add(1, 1)   # title
    $s.Shapes[1].TextFrame.TextRange.Text = "Die Waffenr$([char]0x00FC)stung Gottes"
    $s.Shapes[2].TextFrame.TextRange.Text = "$([char]0x0415)$([char]0x0444)$([char]0x0435)$([char]0x0441)$([char]0x044F)$([char]0x043D)$([char]0x0430)$([char]0x043C) 6:10-18 | Epheser 6,10-18"
    $s.FollowMasterBackground = 0
    $s.Background.Fill.TwoColorGradient(1, 1)
    $s.Background.Fill.ForeColor.RGB = 0x804020
    $s.Background.Fill.BackColor.RGB = 0x201008
    $s.Shapes[1].TextFrame.TextRange.Font.Color.RGB = 0xFFFFFF
    $s.Shapes[2].TextFrame.TextRange.Font.Color.RGB = 0xC0E0FF

    $s = $pres.Slides.Add(2, 2)   # title + bullets, long text that must shrink to fit
    $s.Shapes[1].TextFrame.TextRange.Text = "Armour of God"
    $s.Shapes[2].TextFrame.TextRange.Text = "Belt of truth (Ephesians 6:14)`rBreastplate of righteousness (Ephesians 6:14)`rShoes of the gospel of peace (Ephesians 6:15)`rShield of faith, with which you can extinguish all the flaming arrows of the evil one (Ephesians 6:16)`rHelmet of salvation (Ephesians 6:17)`rSword of the Spirit, which is the word of God (Ephesians 6:17)`rPraying at all times in the Spirit, with all prayer and supplication (Ephesians 6:18)`rSchild des Glaubens, mit dem ihr alle feurigen Pfeile des B$([char]0x00F6)sen ausl$([char]0x00F6)schen k$([char]0x00F6)nnt"
    $s.Shapes[2].TextFrame2.AutoSize = 2   # shrink text on overflow

    $s = $pres.Slides.Add(3, 12)  # blank: picture, shapes, effects
    [void]$s.Shapes.AddPicture($Picture, 0, -1, 40, 40, 420, 236)
    $r = $s.Shapes.AddShape(5, 500, 60, 400, 150)   # rounded rectangle
    $r.Fill.TwoColorGradient(2, 1); $r.Fill.ForeColor.RGB = 0x2060C0; $r.Fill.BackColor.RGB = 0x80D0FF
    $r.Shadow.Visible = -1; $r.Shadow.Blur = 12; $r.Shadow.OffsetX = 6; $r.Shadow.OffsetY = 6
    $r.TextFrame.TextRange.Text = "Shield of Faith"; $r.TextFrame.TextRange.Font.Size = 36; $r.TextFrame.TextRange.Font.Bold = -1
    $t = $s.Shapes.AddTextbox(1, 520, 260, 380, 120)
    $t.TextFrame.TextRange.Text = "$([char]0x0429)$([char]0x0438)$([char]0x0442) $([char]0x0432)$([char]0x0435)$([char]0x0440)$([char]0x044B)"
    $t.TextFrame.TextRange.Font.Size = 54; $t.TextFrame.TextRange.Font.Name = "Georgia"; $t.Rotation = -8
    $w = $s.Shapes.AddTextEffect(0, "Glaube", "Arial Black", 60, 0, 0, 60, 330)
    $w.Fill.ForeColor.RGB = 0x2020C0
    $o = $s.Shapes.AddShape(12, 420, 380, 120, 120)   # star
    $o.Fill.ForeColor.RGB = 0x00C0FF; $o.Line.Weight = 3

    $s = $pres.Slides.Add(4, 11)  # title only + table
    $s.Shapes[1].TextFrame.TextRange.Text = "Sunday service"
    $tb = $s.Shapes.AddTable(4, 3, 60, 130, 840, 300).Table
    $cells = @("Time","Item","Who","10:00","Opening songs","Alex","10:20","Sermon 1","Valentin","11:10","Sermon 3 (main)","Florian")
    for ($i = 0; $i -lt 12; $i++) { $tb.Cell([int][math]::Floor($i / 3) + 1, ($i % 3) + 1).Shape.TextFrame.TextRange.Text = $cells[$i] }

    $s = $pres.Slides.Add(5, 11)  # title only
    $s.Shapes[1].TextFrame.TextRange.Text = "Order of the armour"

    $s = $pres.Slides.Add(6, 2)   # hidden: must not come out
    $s.Shapes[1].TextFrame.TextRange.Text = "HIDDEN SLIDE"
    $s.SlideShowTransition.Hidden = -1

    $s = $pres.Slides.Add(7, 2)   # last
    $s.Shapes[1].TextFrame.TextRange.Text = "Die Gnade Jesu Christi"
    $s.Shapes[2].TextFrame.TextRange.Text = "und die Liebe Gottes und die Gemeinschaft mit dem Heiligen Geist sei mit euch allen. Amen."
    $s.Shapes[2].TextFrame.TextRange.ParagraphFormat.Bullet.Visible = 0
    $s.Shapes[2].TextFrame.TextRange.Font.Italic = -1

    $pres.SaveAs($Out)
    if ($Pdf) { $pres.SaveAs($Pdf, 32) }   # ppSaveAsPDF
    "saved $Out, $($pres.Slides.Count) slides"
} finally {
    $pres.Close()
    if ($app.Presentations.Count -eq 0) { $app.Quit() }
}
