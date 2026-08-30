$svgIcons = @{
    'AWS_Simple_Icons_Storage_Amazon_S3.svg' = 'bucket.ico'
    'user-round.svg' = 'profile.ico'
    'icons8-amazon-s3.svg' = 's3cmd.ico'
}
$sizes = @(16, 32, 48, 256)

Get-Command magick -ErrorAction Stop | Out-Null
foreach ($icon in $svgIcons.GetEnumerator()) {
    magick -background none -density 1024 `
        (Join-Path $PSScriptRoot $icon.Key) `
        -define 'icon:auto-resize=256,48,32,16' `
        -define 'icon:png-compression-size=16' `
        (Join-Path $PSScriptRoot $icon.Value)
    if ($LASTEXITCODE -ne 0) {
        throw "ImageMagick failed to generate $($icon.Value)"
    }
}
