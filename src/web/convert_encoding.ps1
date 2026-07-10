$paths = @(
    (Join-Path $PSScriptRoot 'cad_model.h'),
    (Join-Path $PSScriptRoot 'svg_images.h')
)

foreach ($p in $paths) {
    Copy-Item -LiteralPath $p -Destination "$p.bak" -ErrorAction SilentlyContinue
    try {
        # Check bytes for UTF16
        $bytes = [System.IO.File]::ReadAllBytes($p)
        if ($bytes.Length -ge 2 -and $bytes[0] -eq 0xFF -and $bytes[1] -eq 0xFE) {
            # UTF-16 LE
            $text = [System.Text.Encoding]::Unicode.GetString($bytes, 2, $bytes.Length - 2)
        }
        elseif ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF) {
            # UTF-8 BOM
            $text = [System.Text.Encoding]::UTF8.GetString($bytes, 3, $bytes.Length - 3)
        }
        else {
            # Default fallback (might be utf8 without BOM, read it as utf8)
            $text = [System.Text.Encoding]::UTF8.GetString($bytes)
        }
        
        $utf8NoBom = New-Object System.Text.UTF8Encoding($false)
        [System.IO.File]::WriteAllText($p, $text, $utf8NoBom)
        Write-Host "Successfully converted $p"
    }
    catch {
        Write-Host "Failed to convert $p : $_"
    }
}
