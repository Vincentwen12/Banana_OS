# Download real glibc dynamic bash 5.2 (noble) + ld-linux + libtinfo from Ubuntu archive.
# No git. Uses curl.exe + python (tarfile) to unpack .deb files.
$ErrorActionPreference = "Stop"
$here = $PSScriptRoot
$pool = "http://archive.ubuntu.com/ubuntu/pool/main"

function Get-LatestDeb {
    param([string]$dir, [string]$pattern)
    $lines = & curl.exe -s "$pool/$dir/"
    $names = @()
    foreach ($l in $lines) {
        if ($l -match "href=""($pattern)") {
            $names += $Matches[1]
        }
    }
    if ($names.Count -eq 0) { throw "No deb matching $pattern in $pool/$dir/" }
    # pick the highest version string
    ($names | Sort-Object)[-1]
}

$bashDeb  = Get-LatestDeb "b/bash"    "bash_5\.2\.[^""]+_amd64\.deb"
$libcDeb  = Get-LatestDeb "g/glibc"   "libc6_2\.39-[^""]*_amd64\.deb"
$tinfoDeb = Get-LatestDeb "n/ncurses" "libtinfo6_6\.4\+[^""]*_amd64\.deb"

Write-Host "Downloading: $bashDeb"
& curl.exe -s -o "$here\bash.deb" "$pool/b/bash/$bashDeb"
Write-Host "Downloading: $libcDeb"
& curl.exe -s -o "$here\libc6.deb" "$pool/g/glibc/$libcDeb"
Write-Host "Downloading: $tinfoDeb"
& curl.exe -s -o "$here\libtinfo6.deb" "$pool/n/ncurses/$tinfoDeb"

Write-Host "Unpacking..."
python "$here\unpack_deb.py"
Write-Host "Done. Extracted files:"
Get-Item "$here\bash", "$here\ld-linux-x86-64.so.2", "$here\libtinfo.so.6" -ErrorAction SilentlyContinue | Select-Object Name, Length
