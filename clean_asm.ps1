param([string]$File)

$c = Get-Content $File -Raw
$c = $c -replace '\.def\s+[.\w]+;\s*\.scl\s+\d+;\s*\.type\s+\d+;\s*\.endef\s*\n', "`n"
$c = $c -replace 'call\s+\*\.refptr\.(\w+)\(%rip\)', 'call $1'
$c = $c -replace 'jmp\s+\*\.refptr\.(\w+)\(%rip\)', 'jmp $1'
$c = $c -replace 'movq\s+\.refptr\.(\w+)\(%rip\),\s*%(\w+)', 'leaq $1(%rip), %$2'
$c = $c -replace '\.section\s+\.rdata,"dr"', '.section .rodata'
$c = $c -replace '\.section\s+\.rdata\$\$?\.refptr\..*?\n(?:[\t ]*\..*?\n)*', "`n"
$c = $c -replace '\.ident\s+"".*?""\n', ''
$c = $c -replace '\.linkonce\s+discard\n', ''
$c = $c -replace '\.seh_\w+(\s+\w+)?\n', ''
$c = $c -replace '\.lcomm\s+([.\w]+),(\d+),\d+', '.lcomm $1,$2'
[System.IO.File]::WriteAllText($File, $c, [System.Text.UTF8Encoding]::new($false))