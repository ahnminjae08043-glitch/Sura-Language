param(
    [string]$FontPath = "",
    [string]$Output = "",
    [switch]$ForceDownload
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$cacheDirectory = Join-Path $root "build/font-cache"
$fontUrl = "https://raw.githubusercontent.com/notofonts/noto-cjk/main/Sans/OTF/Korean/NotoSansCJKkr-Regular.otf"
$expectedSha256 = "6BCB2A0703AA137E874FC2DFFA85F6C21BA9A67FA329E81B8C801663AF7E992A"
if ([string]::IsNullOrWhiteSpace($FontPath)) {
    $FontPath = Join-Path $cacheDirectory "NotoSansCJKkr-Regular.otf"
}
if ([string]::IsNullOrWhiteSpace($Output)) {
    $Output = Join-Path $root "stdlib/freestanding/font_ui_atlas.sura"
}

New-Item -ItemType Directory -Force -Path (Split-Path -Parent $FontPath) | Out-Null
if ($ForceDownload -or -not (Test-Path -LiteralPath $FontPath -PathType Leaf)) {
    Invoke-WebRequest -Uri $fontUrl -OutFile $FontPath -UseBasicParsing
}
$actualSha256 = (Get-FileHash -LiteralPath $FontPath -Algorithm SHA256).Hash
if ($actualSha256 -ne $expectedSha256) {
    throw "Noto Sans CJK KR hash mismatch: expected $expectedSha256, got $actualSha256"
}

$generatorSource = @'
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.Drawing.Text;
using System.IO;
using System.Text;

public static class SuraUiFontGenerator
{
    const int Width = 16;
    const int Height = 16;
    const int BytesPerGlyph = 64;
    const int AsciiFirst = 32;
    const int AsciiLast = 126;
    const int HangulFirst = 0xAC00;
    const int HangulLast = 0xD7A3;

    static byte Quantize(byte value)
    {
        if (value < 32) return 0;
        if (value < 104) return 1;
        if (value < 192) return 2;
        return 3;
    }

    static void Rasterize(
        Graphics graphics,
        Bitmap bitmap,
        Font font,
        StringFormat format,
        string text,
        byte[] atlas,
        int glyphIndex)
    {
        graphics.Clear(Color.Black);
        graphics.DrawString(text, font, Brushes.White, new PointF(-1.0f, -1.0f), format);
        int output = glyphIndex * BytesPerGlyph;
        for (int y = 0; y < Height; y++)
        {
            for (int byteColumn = 0; byteColumn < 4; byteColumn++)
            {
                byte packed = 0;
                for (int part = 0; part < 4; part++)
                {
                    int x = byteColumn * 4 + part;
                    byte level = Quantize(bitmap.GetPixel(x, y).R);
                    packed |= (byte)(level << ((3 - part) * 2));
                }
                atlas[output++] = packed;
            }
        }
    }

    static void WriteByteArray(StreamWriter writer, string name, byte[] values)
    {
        writer.Write(name);
        writer.Write(" is static.bytes([");
        for (int i = 0; i < values.Length; i++)
        {
            if (i != 0) writer.Write(", ");
            writer.Write(values[i]);
        }
        writer.WriteLine("], 16)");
    }

    public static void Generate(string fontPath, string outputPath, string sha256)
    {
        var fonts = new PrivateFontCollection();
        fonts.AddFontFile(fontPath);
        if (fonts.Families.Length == 0) throw new InvalidOperationException("font family was not loaded");

        int asciiCount = AsciiLast - AsciiFirst + 1;
        int hangulCount = HangulLast - HangulFirst + 1;
        int glyphCount = asciiCount + hangulCount;
        var atlas = new byte[glyphCount * BytesPerGlyph];
        var advances = new byte[asciiCount];
        using (var bitmap = new Bitmap(Width, Height, PixelFormat.Format32bppArgb))
        using (var graphics = Graphics.FromImage(bitmap))
        using (var font = new Font(fonts.Families[0], 14.0f, FontStyle.Regular, GraphicsUnit.Pixel))
        using (var format = (StringFormat)StringFormat.GenericTypographic.Clone())
        {
            graphics.TextRenderingHint = TextRenderingHint.AntiAliasGridFit;
            graphics.SmoothingMode = System.Drawing.Drawing2D.SmoothingMode.HighQuality;
            graphics.PixelOffsetMode = System.Drawing.Drawing2D.PixelOffsetMode.HighQuality;
            format.FormatFlags |= StringFormatFlags.MeasureTrailingSpaces;
            for (int codepoint = AsciiFirst; codepoint <= AsciiLast; codepoint++)
            {
                string text = char.ConvertFromUtf32(codepoint);
                int glyphIndex = codepoint - AsciiFirst;
                Rasterize(graphics, bitmap, font, format, text, atlas, glyphIndex);
                int advance = (int)Math.Ceiling(graphics.MeasureString(text, font, 32, format).Width);
                if (advance < 4) advance = 4;
                if (advance > 15) advance = 15;
                advances[glyphIndex] = (byte)advance;
            }
            for (int codepoint = HangulFirst; codepoint <= HangulLast; codepoint++)
            {
                int glyphIndex = asciiCount + codepoint - HangulFirst;
                Rasterize(
                    graphics,
                    bitmap,
                    font,
                    format,
                    char.ConvertFromUtf32(codepoint),
                    atlas,
                    glyphIndex);
            }
        }

        Directory.CreateDirectory(Path.GetDirectoryName(outputPath));
        using (var writer = new StreamWriter(outputPath, false, new UTF8Encoding(false)))
        {
            writer.WriteLine("# Generated by tools/sura_ui_font_generate.ps1.");
            writer.WriteLine("# Source font: Noto Sans CJK KR Regular, SIL Open Font License 1.1.");
            writer.WriteLine("# Source SHA-256: " + sha256);
            writer.WriteLine("# 2-bit coverage, 16x16 cells, four pixels per packed byte.");
            writer.WriteLine();
            WriteByteArray(writer, "font_ui_ascii_advance", advances);
            writer.WriteLine();
            WriteByteArray(writer, "font_ui_glyph_atlas", atlas);
        }
    }
}
'@

Add-Type -TypeDefinition $generatorSource -ReferencedAssemblies "System.Drawing" -ErrorAction Stop
[SuraUiFontGenerator]::Generate(
    (Resolve-Path -LiteralPath $FontPath).Path,
    [System.IO.Path]::GetFullPath($Output),
    $actualSha256
)

$outputItem = Get-Item -LiteralPath $Output
Write-Host "sura_ui_font_generate: PASS (glyphs=11267, bytes=$($outputItem.Length), output=$($outputItem.FullName))"
