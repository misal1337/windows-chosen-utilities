# Turns lang.txt (one line per text: English|de|fr|es|pt-BR|zh-CN) into lang_data.inc. build.ps1 runs this every time.
$ErrorActionPreference = 'Stop'
$rows = @()
$seen = @{}
$n = 0
foreach ($line in [IO.File]::ReadAllLines("$PSScriptRoot\lang.txt", [Text.Encoding]::UTF8)) {
    $n++
    if ($n -eq 1 -or -not $line.Trim()) { continue }
    $c = $line.Split('|')
    if ($c.Count -ne 6) { throw "lang.txt line ${n}: $($c.Count) columns, not 6" }
    if ($seen.ContainsKey($c[0])) { throw "lang.txt line ${n}: '$($c[0])' is listed twice" }
    $seen[$c[0]] = 1
    $want = (@([regex]::Matches($c[0], '\{\d\}') | ForEach-Object Value) | Sort-Object) -join ''
    foreach ($x in $c) {
        if (-not $x.Trim()) { throw "lang.txt line ${n}: an empty translation" }
        $have = (@([regex]::Matches($x, '\{\d\}') | ForEach-Object Value) | Sort-Object) -join ''
        if ($have -ne $want) { throw "lang.txt line ${n}: '$x' doesn't use the same {n} parts" }
    }
    # a longer fixed text is a more specific template, so it's tried first
    $rows += [pscustomobject]@{ C = $c; Fixed = ($c[0] -replace '\{\d\}', '').Length }
}
function Quote-Text($s) {
    $b = New-Object Text.StringBuilder
    [void]$b.Append('L"')
    foreach ($ch in $s.ToCharArray()) {
        if ($ch -eq '\' -or $ch -eq '"') { [void]$b.Append('\').Append($ch) }
        elseif ([int]$ch -gt 126) { [void]$b.Append(('\x{0:X4}""' -f [int]$ch)) }    # "" ends the escape
        else { [void]$b.Append($ch) }
    }
    [void]$b.Append('"')
    $b.ToString().Replace('""', '" L"')
}
$out = New-Object Text.StringBuilder
[void]$out.Append("// Made from lang.txt by lang.ps1. Don't edit.`nstatic const LangRow LANG_DATA[] = {`n")
foreach ($r in ($rows | Sort-Object Fixed -Descending)) {
    [void]$out.Append('    { { ').Append((($r.C | ForEach-Object { Quote-Text $_ }) -join ', ')).Append(" } },`n")
}
[void]$out.Append("};`n")
[IO.File]::WriteAllText("$PSScriptRoot\lang_data.inc", $out.ToString())
"languages: $($rows.Count) texts"
