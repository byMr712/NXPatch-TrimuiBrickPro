param(
    [string]$Ver = '1.1',
    [string]$FontPath = (Join-Path $PSScriptRoot 'font1.ttf'),
    [string]$OutDir = (Join-Path $PSScriptRoot '..\port\Tools\.nxredux')
)

# Генератор картинок меню патчера: menu0..2.bmp (подсказки A/B) и menu0b..2b.bmp (B/A).
# Рисует ровно то, что показывает mr712menu: заголовок, три пункта, описание и подсказки.
# Шрифт не входит в репозиторий: положите рядом font1.ttf (тот, которым собраны
# текущие картинки) или передайте -FontPath. Размер картинок 1024x768, 24 бита.

Add-Type -AssemblyName System.Drawing

if (-not (Test-Path -LiteralPath $FontPath)) {
    throw "Не найден шрифт: $FontPath (передайте -FontPath путь к ttf)"
}
if (-not (Test-Path -LiteralPath $OutDir)) {
    throw "Не найдена папка вывода: $OutDir"
}

$W = 1024; $H = 768
$pfc = New-Object System.Drawing.Text.PrivateFontCollection
$pfc.AddFontFile($FontPath)
$fam = $pfc.Families[0]
$px = [System.Drawing.GraphicsUnit]::Pixel

$fTitle = New-Object System.Drawing.Font($fam, 32, [System.Drawing.FontStyle]::Bold, $px)
$fItem  = New-Object System.Drawing.Font($fam, 46, [System.Drawing.FontStyle]::Regular, $px)
$fSel   = New-Object System.Drawing.Font($fam, 46, [System.Drawing.FontStyle]::Bold, $px)
$fDesc  = New-Object System.Drawing.Font($fam, 21, [System.Drawing.FontStyle]::Regular, $px)
$fFoot  = New-Object System.Drawing.Font($fam, 19, [System.Drawing.FontStyle]::Regular, $px)

$white = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(235, 238, 240))
$grey  = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(150, 155, 160))
$dim   = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(120, 125, 130))
$rowBg = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(22, 22, 25))
$bar   = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(230, 234, 236))

$items = @(
    @('Установка патча', 'Добавит русский язык, изменит логотип загрузки, меню инструментов и другие изменения'),
    @('Удаление патча',  'Вернёт вашу прошивку к обычной версии NX Redux'),
    @('Выйти',           'Вернуться в меню, ничего не меняя')
)

function Wrap($text, $font, $maxW) {
    $words = $text -split ' '
    $lines = @(); $cur = ''
    foreach ($w in $words) {
        $try = if ($cur) { "$cur $w" } else { $w }
        if ($g.MeasureString($try, $font).Width -le $maxW) { $cur = $try }
        else { if ($cur) { $lines += $cur }; $cur = $w }
    }
    if ($cur) { $lines += $cur }
    return $lines
}

# $suffix = '' (A/B) или 'b' (B/A)
foreach ($set in @(@('', 'A', 'B'), @('b', 'B', 'A'))) {
    $suffix = $set[0]; $conf = $set[1]; $back = $set[2]
    for ($s = 0; $s -lt 3; $s++) {
        $bmp = New-Object System.Drawing.Bitmap($W, $H, [System.Drawing.Imaging.PixelFormat]::Format24bppRgb)
        $g = [System.Drawing.Graphics]::FromImage($bmp)
        $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
        $g.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::AntiAlias
        $g.Clear([System.Drawing.Color]::FromArgb(10, 10, 12))

        $g.DrawString('NX REDUX  ·  Mr712 Patch', $fTitle, $white, 60, 48)
        $rule = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(34, 255, 255, 255), 1)

        $y = 150
        for ($i = 0; $i -lt 3; $i++) {
            $sel = ($i -eq $s)
            if ($sel) {
                $g.FillRectangle($rowBg, 44, ($y - 14), ($W - 88), 72)
                $g.FillRectangle($bar, 44, ($y - 14), 3, 72)
            }
            $font = if ($sel) { $fSel } else { $fItem }
            $brush = if ($sel) { $white } else { $grey }
            $g.DrawString($items[$i][0], $font, $brush, 70, $y)
            $y += 86
        }

        $g.DrawLine($rule, 60, 470, ($W - 60), 470)
        $yy = 500
        foreach ($line in (Wrap $items[$s][1] $fDesc 880)) {
            $g.DrawString($line, $fDesc, $white, 60, $yy)
            $yy += 30
        }

        $g.DrawString("$conf — ВЫБРАТЬ      $back — НАЗАД", $fFoot, $dim, 60, 706)
        $ver = "Версия патча $Ver"
        $vw = $g.MeasureString($ver, $fFoot).Width
        $g.DrawString($ver, $fFoot, $dim, ($W - 60 - $vw), 706)

        $g.Dispose()
        $out = Join-Path $OutDir "menu${s}${suffix}.bmp"
        $bmp.Save($out, [System.Drawing.Imaging.ImageFormat]::Bmp)
        $bmp.Dispose()
        Write-Output $out
    }
}
