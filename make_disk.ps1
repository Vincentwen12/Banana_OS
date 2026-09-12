$boot = [System.IO.File]::ReadAllBytes('bootsect.bin')
$kern = [System.IO.File]::ReadAllBytes('kernel.raw')
$disk = New-Object byte[] (1474560)
[Array]::Copy($boot, 0, $disk, 0, 512)
[Array]::Copy($kern, 0, $disk, 512, $kern.Length)
[System.IO.File]::WriteAllBytes('disk.img', $disk)
Write-Host "disk.img created"