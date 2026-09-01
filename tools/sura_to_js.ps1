param(
    [Parameter(Mandatory=$true)][string]$Source,
    [string]$Out = "",
    [string]$Engine = "",
    [switch]$AstJson,
    [switch]$AllowUnsupported
)

if (-not $Out) {
    $Out = [System.IO.Path]::ChangeExtension($Source, ".js")
}

$declared = @{}
$loopCounter = 0
$script:jsWhenCounter = 0
$script:jsMatchCounter = 0
$script:jsBlockStack = New-Object System.Collections.Generic.List[string]
$script:jsWhenStack = New-Object System.Collections.Generic.List[object]
$script:jsMatchStack = New-Object System.Collections.Generic.List[object]
$script:jsStructStack = New-Object System.Collections.Generic.List[object]
$script:jsAstClassInternalNames = @{}

function Resolve-JsImportPath {
    param(
        [Parameter(Mandatory=$true)][string]$ImportPath,
        [Parameter(Mandatory=$true)][string]$ImporterPath
    )
    $candidate = if ([System.IO.Path]::IsPathRooted($ImportPath)) {
        $ImportPath
    } else {
        Join-Path (Split-Path -Parent $ImporterPath) $ImportPath
    }
    if (-not (Test-Path -LiteralPath $candidate)) {
        throw "JS target import not found: $ImportPath from $ImporterPath"
    }
    return (Resolve-Path -LiteralPath $candidate).Path
}

function Resolve-JsEngine {
    param([string]$EnginePath)
    if ($EnginePath -and (Test-Path -LiteralPath $EnginePath)) {
        return (Resolve-Path -LiteralPath $EnginePath).Path
    }
    $root = Split-Path -Parent $PSScriptRoot
    foreach ($candidate in @((Join-Path $root "SuraLanguage.exe"), (Join-Path $root "SuraLanguage"))) {
        if (Test-Path -LiteralPath $candidate) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }
    throw "Sura engine not found for JS AST import expansion"
}

function Unescape-SuraImportPath {
    param([string]$Path)
    return $Path.Replace('\"', '"').Replace("\\", "\")
}

function Expand-JsSourceLines {
    param(
        [Parameter(Mandatory=$true)][string]$Path,
        [Parameter(Mandatory=$true)][hashtable]$State
    )
    $resolved = (Resolve-Path -LiteralPath $Path).Path
    if ($State.ContainsKey($resolved)) {
        if ($State[$resolved] -eq "active") {
            throw "cyclic JS target import: $resolved"
        }
        return @("// import skipped: $resolved")
    }

    $State[$resolved] = "active"
    $out = New-Object System.Collections.Generic.List[string]
    foreach ($raw in @(Get-Content -LiteralPath $resolved)) {
        $t = $raw.Trim()
        $m = [regex]::Match($t, '^import\s+(?:"((?:\\.|[^"])*)"|''((?:\\.|[^''])*)'')\s*$')
        if ($m.Success) {
            $rawImport = if ($m.Groups[1].Success) { $m.Groups[1].Value } else { $m.Groups[2].Value }
            $importPath = Unescape-SuraImportPath $rawImport
            $importResolved = Resolve-JsImportPath -ImportPath $importPath -ImporterPath $resolved
            $out.Add("// import $importPath") | Out-Null
            foreach ($line in (Expand-JsSourceLines -Path $importResolved -State $State)) {
                $out.Add($line) | Out-Null
            }
            continue
        }
        $out.Add($raw) | Out-Null
    }
    $State[$resolved] = "done"
    return @($out.ToArray())
}

function Push-JsBlock {
    param([string]$Kind)
    $script:jsBlockStack.Add($Kind) | Out-Null
}

function Pop-JsBlock {
    if ($script:jsBlockStack.Count -eq 0) { return "" }
    $idx = $script:jsBlockStack.Count - 1
    $kind = $script:jsBlockStack[$idx]
    $script:jsBlockStack.RemoveAt($idx)
    return $kind
}

function Get-JsBlockTop {
    if ($script:jsBlockStack.Count -eq 0) { return "" }
    return $script:jsBlockStack[$script:jsBlockStack.Count - 1]
}

function Get-JsWhenTop {
    if ($script:jsWhenStack.Count -eq 0) { return $null }
    return $script:jsWhenStack[$script:jsWhenStack.Count - 1]
}

function Get-JsMatchTop {
    if ($script:jsMatchStack.Count -eq 0) { return $null }
    return $script:jsMatchStack[$script:jsMatchStack.Count - 1]
}

function Get-JsStructTop {
    if ($script:jsStructStack.Count -eq 0) { return $null }
    return $script:jsStructStack[$script:jsStructStack.Count - 1]
}

function Add-JsStructAutoConstructor {
    param([Parameter(Mandatory=$true)]$StructInfo)
    $lines = New-Object System.Collections.Generic.List[string]
    if ($StructInfo.Fields.Count -eq 0) { return @() }
    $paramNames = New-Object System.Collections.Generic.List[string]
    for ($i = 0; $i -lt $StructInfo.Fields.Count; $i++) {
        $paramNames.Add($StructInfo.Fields[$i].Name) | Out-Null
    }
    $params = [string]::Join(", ", [string[]]$paramNames.ToArray())
    $lines.Add("constructor($params) {") | Out-Null
    for ($i = 0; $i -lt $StructInfo.Fields.Count; $i++) {
        $field = $StructInfo.Fields[$i]
        $fallback = if ($field.Default) { Convert-SuraExpression $field.Default } else { "null" }
        $lines.Add("  this.$($field.Name) = $($field.Name) === undefined ? $fallback : $($field.Name);") | Out-Null
    }
    $lines.Add("}") | Out-Null
    return [string[]]$lines.ToArray()
}

function Close-JsWhenArm {
    if ((Get-JsBlockTop) -eq "when-arm") {
        $script:jsBlockStack.RemoveAt($script:jsBlockStack.Count - 1)
        return "}"
    }
    return $null
}

function Start-JsWhenArm {
    param(
        [Parameter(Mandatory=$true)][string]$Condition,
        [switch]$Else
    )
    $when = Get-JsWhenTop
    if ($null -eq $when) { throw "when arm outside when block" }
    $lines = New-Object System.Collections.Generic.List[string]
    if ((Get-JsBlockTop) -eq "when-arm") {
        $script:jsBlockStack.RemoveAt($script:jsBlockStack.Count - 1)
        $lines.Add("}") | Out-Null
    }
    if ($Else) {
        if ($when.HasElse) { throw "when has duplicate else arms" }
        $when.HasElse = $true
        $lines.Add("if (!$($when.Matched)) {") | Out-Null
    } else {
        $lines.Add("if (!$($when.Matched) && ($Condition)) {") | Out-Null
        $lines.Add("$($when.Matched) = true;") | Out-Null
    }
    $when.Arms++
    Push-JsBlock "when-arm"
    return @($lines)
}

function Start-JsMatchArm {
    param(
        [Parameter(Mandatory=$true)][string]$Condition,
        [switch]$Else
    )
    $match = Get-JsMatchTop
    if ($null -eq $match) { throw "match arm outside match block" }
    $lines = New-Object System.Collections.Generic.List[string]
    if ((Get-JsBlockTop) -eq "match-arm") {
        $script:jsBlockStack.RemoveAt($script:jsBlockStack.Count - 1)
        $lines.Add("}") | Out-Null
    }
    if ($Else) {
        if ($match.HasWildcard) { throw "match has duplicate wildcard arms" }
        $match.HasWildcard = $true
        $lines.Add("if (!$($match.Matched)) {") | Out-Null
    } else {
        $lines.Add("if (!$($match.Matched) && ($Condition)) {") | Out-Null
        $lines.Add("$($match.Matched) = true;") | Out-Null
    }
    $match.Arms++
    Push-JsBlock "match-arm"
    return @($lines)
}

function Split-SuraCsv {
    param([string]$Text)

    $items = New-Object System.Collections.Generic.List[string]
    $depth = 0
    $inString = $false
    $escaped = $false
    $start = 0
    for ($i = 0; $i -lt $Text.Length; $i++) {
        $ch = $Text[$i]
        if ($inString) {
            if ($escaped) {
                $escaped = $false
            } elseif ($ch -eq '\') {
                $escaped = $true
            } elseif ($ch -eq '"') {
                $inString = $false
            }
            continue
        }
        if ($ch -eq '"') {
            $inString = $true
        } elseif ($ch -eq '(' -or $ch -eq '[' -or $ch -eq '{') {
            $depth++
        } elseif ($ch -eq ')' -or $ch -eq ']' -or $ch -eq '}') {
            if ($depth -gt 0) { $depth-- }
        } elseif ($ch -eq ',' -and $depth -eq 0) {
            $items.Add($Text.Substring($start, $i - $start).Trim()) | Out-Null
            $start = $i + 1
        }
    }
    if ($start -le $Text.Length) {
        $tail = $Text.Substring($start).Trim()
        if ($tail) { $items.Add($tail) | Out-Null }
    }
    return @($items)
}

function Convert-JsParam {
    param([Parameter(Mandatory=$true)][string]$Param)

    $p = $Param.Trim()
    if ($p -match '^([A-Za-z_][A-Za-z0-9_]*)(?:\s*:\s*.+?)?(?:\s+is\s+(.+))?$') {
        $name = $Matches[1]
        if ($Matches[2]) {
            return "$name = $(Convert-SuraExpression $Matches[2])"
        }
        return $name
    }
    throw "unsupported JS target parameter syntax: $Param"
}

function Convert-JsParamList {
    param([string]$Text)

    $params = @(Split-SuraCsv $Text)
    $out = @()
    foreach ($param in $params) {
        if ($param) { $out += Convert-JsParam $param }
    }
    return ($out -join ", ")
}

function Find-SuraInterpolationEnd {
    param(
        [Parameter(Mandatory=$true)][string]$Text,
        [Parameter(Mandatory=$true)][int]$Start
    )

    $depth = 1
    $inString = $false
    $escaped = $false
    for ($i = $Start; $i -lt $Text.Length; $i++) {
        $ch = $Text[$i]
        if ($inString) {
            if ($ch -eq '\' -and $i + 1 -lt $Text.Length -and $Text[$i + 1] -eq '"') {
                $inString = $false
                $i++
            } elseif ($escaped) {
                $escaped = $false
            } elseif ($ch -eq '\') {
                $escaped = $true
            } elseif ($ch -eq '"') {
                $inString = $false
            }
            continue
        }
        if ($ch -eq '\' -and $i + 1 -lt $Text.Length -and $Text[$i + 1] -eq '"') {
            $inString = $true
            $i++
        } elseif ($ch -eq '"') {
            $inString = $true
        } elseif ($ch -eq '{') {
            $depth++
        } elseif ($ch -eq '}') {
            $depth--
            if ($depth -eq 0) { return $i }
        }
    }
    return -1
}

function Convert-SuraInterpolationExprSource {
    param([Parameter(Mandatory=$true)][string]$Text)

    $out = New-Object System.Text.StringBuilder
    for ($i = 0; $i -lt $Text.Length; $i++) {
        if ($Text[$i] -eq '\' -and $i + 1 -lt $Text.Length) {
            $next = $Text[$i + 1]
            if ($next -eq '"' -or $next -eq "'" -or $next -eq '\') {
                [void]$out.Append($next)
                $i++
                continue
            }
        }
        [void]$out.Append($Text[$i])
    }
    return $out.ToString()
}

function Escape-JsTemplateText {
    param([string]$Text)

    if ($null -eq $Text) { return "" }
    return $Text.Replace(([string][char]96), ([string][char]92 + [string][char]96)).
        Replace(([string][char]36), ([string][char]92 + [string][char]36))
}

function Convert-SuraStringLiteral {
    param([Parameter(Mandatory=$true)][string]$Literal)

    $inner = $Literal.Substring(1, $Literal.Length - 2)
    $out = New-Object System.Text.StringBuilder
    $cur = New-Object System.Text.StringBuilder
    $hasInterpolation = $false
    $i = 0
    while ($i -lt $inner.Length) {
        if ($inner[$i] -eq '{') {
            if ($i + 1 -lt $inner.Length -and $inner[$i + 1] -eq '{') {
                [void]$cur.Append('{')
                $i += 2
                continue
            }
            $end = Find-SuraInterpolationEnd -Text $inner -Start ($i + 1)
            if ($end -lt 0) {
                [void]$cur.Append($inner[$i])
                $i++
                continue
            }
            $expr = $inner.Substring($i + 1, $end - $i - 1)
            if ([string]::IsNullOrWhiteSpace($expr)) {
                [void]$cur.Append('{')
                [void]$cur.Append($expr)
                [void]$cur.Append('}')
                $i = $end + 1
                continue
            }
            [void]$out.Append((Escape-JsTemplateText -Text ($cur.ToString())))
            [void]$cur.Clear()
            [void]$out.Append('${')
            [void]$out.Append((Convert-SuraExpression (Convert-SuraInterpolationExprSource $expr)))
            [void]$out.Append('}')
            $hasInterpolation = $true
            $i = $end + 1
            continue
        }
        [void]$cur.Append($inner[$i])
        $i++
    }

    if (-not $hasInterpolation) { return $Literal }
    [void]$out.Append((Escape-JsTemplateText -Text ($cur.ToString())))
    return ([string][char]96) + $out.ToString() + ([string][char]96)
}

function Convert-SuraLambdaExpression {
    param([Parameter(Mandatory=$true)][string]$Expr)

    $e = $Expr.Trim()
    if ($e -match '^\|\s*(.*?)\s*\|\s*(.+)$') {
        $params = Convert-JsParamList $Matches[1]
        $body = Convert-SuraExpression $Matches[2]
        return "(($params) => $body)"
    }
    return $null
}

function Convert-SuraInlineFunctionExpressions {
    param([Parameter(Mandatory=$true)][string]$Expr)

    $e = $Expr
    while ($true) {
        $m = [regex]::Match($e, 'func\s*\(([^)]*)\)\s*do\s*return\s+(.+?)\s*end')
        if (-not $m.Success) { break }
        $params = Convert-JsParamList $m.Groups[1].Value
        $body = Convert-SuraExpression $m.Groups[2].Value
        $replacement = "(function($params) { return $body; })"
        $e = $e.Substring(0, $m.Index) + $replacement + $e.Substring($m.Index + $m.Length)
    }
    return $e
}

function Convert-SuraDivisionByZeroExpressions {
    param([Parameter(Mandatory=$true)][string]$Expr)

    $e = $Expr.Trim()
    if ($e -match '^(.+?)\s*/\s*(0(?:\.0+)?)$') {
        return "__sura_div($($Matches[1].Trim()), $($Matches[2]))"
    }
    return $Expr
}

function Get-SuraMatchingExpressionDelimiterIndex {
    param(
        [Parameter(Mandatory=$true)][string]$Text,
        [Parameter(Mandatory=$true)][int]$OpenIndex
    )

    $pairs = @{ '(' = ')'; '[' = ']'; '{' = '}' }
    $open = [string]$Text[$OpenIndex]
    if (-not $pairs.ContainsKey($open)) { return -1 }

    $stack = New-Object System.Collections.Generic.List[char]
    for ($i = $OpenIndex; $i -lt $Text.Length; $i++) {
        $ch = $Text[$i]
        if ($ch -eq '(' -or $ch -eq '[' -or $ch -eq '{') {
            $stack.Add($ch)
            continue
        }
        if ($ch -ne ')' -and $ch -ne ']' -and $ch -ne '}') { continue }
        if ($stack.Count -eq 0) { return -1 }
        $top = [string]$stack[$stack.Count - 1]
        if ([string]$pairs[$top] -ne [string]$ch) { return -1 }
        $stack.RemoveAt($stack.Count - 1)
        if ($stack.Count -eq 0) { return $i }
    }
    return -1
}

function Split-SuraTopLevelExpressionList {
    param([Parameter(Mandatory=$true)][string]$Text)

    $parts = New-Object System.Collections.Generic.List[string]
    $start = 0
    $round = 0
    $square = 0
    $curly = 0
    for ($i = 0; $i -lt $Text.Length; $i++) {
        switch ($Text[$i]) {
            '(' { $round++; continue }
            ')' { $round--; continue }
            '[' { $square++; continue }
            ']' { $square--; continue }
            '{' { $curly++; continue }
            '}' { $curly--; continue }
            ',' {
                if ($round -eq 0 -and $square -eq 0 -and $curly -eq 0) {
                    $parts.Add($Text.Substring($start, $i - $start))
                    $start = $i + 1
                }
            }
        }
    }
    $parts.Add($Text.Substring($start))
    return $parts.ToArray()
}

function Find-SuraTopLevelExpressionColon {
    param([Parameter(Mandatory=$true)][string]$Text)

    $round = 0
    $square = 0
    $curly = 0
    for ($i = 0; $i -lt $Text.Length; $i++) {
        switch ($Text[$i]) {
            '(' { $round++; continue }
            ')' { $round--; continue }
            '[' { $square++; continue }
            ']' { $square--; continue }
            '{' { $curly++; continue }
            '}' { $curly--; continue }
            ':' {
                if ($round -eq 0 -and $square -eq 0 -and $curly -eq 0) { return $i }
            }
        }
    }
    return -1
}

function Get-SuraTopLevelWordOperatorPositions {
    param(
        [Parameter(Mandatory=$true)][string]$Text,
        [Parameter(Mandatory=$true)][string]$Word
    )

    $positions = New-Object System.Collections.Generic.List[int]
    $round = 0
    $square = 0
    $curly = 0
    for ($i = 0; $i -le $Text.Length - $Word.Length; $i++) {
        $ch = $Text[$i]
        switch ($ch) {
            '(' { $round++; continue }
            ')' { $round--; continue }
            '[' { $square++; continue }
            ']' { $square--; continue }
            '{' { $curly++; continue }
            '}' { $curly--; continue }
        }
        if ($round -ne 0 -or $square -ne 0 -or $curly -ne 0) { continue }
        if ($Text.Substring($i, $Word.Length) -cne $Word) { continue }
        $beforeOk = $i -eq 0 -or $Text[$i - 1] -notmatch '[A-Za-z0-9_]'
        $afterIndex = $i + $Word.Length
        $afterOk = $afterIndex -ge $Text.Length -or $Text[$afterIndex] -notmatch '[A-Za-z0-9_]'
        if ($beforeOk -and $afterOk) {
            $positions.Add($i)
            $i += $Word.Length - 1
        }
    }
    return $positions.ToArray()
}

function Get-SuraTopLevelComparisonOperators {
    param([Parameter(Mandatory=$true)][string]$Text)

    $operators = New-Object System.Collections.Generic.List[object]
    $round = 0
    $square = 0
    $curly = 0
    for ($i = 0; $i -lt $Text.Length; $i++) {
        $ch = $Text[$i]
        switch ($ch) {
            '(' { $round++; continue }
            ')' { $round--; continue }
            '[' { $square++; continue }
            ']' { $square--; continue }
            '{' { $curly++; continue }
            '}' { $curly--; continue }
        }
        if ($round -ne 0 -or $square -ne 0 -or $curly -ne 0) { continue }

        if ($i -le $Text.Length - 2) {
            $two = $Text.Substring($i, 2)
            if ($two -in @('==', '!=', '<=', '>=')) {
                $operators.Add([pscustomobject]@{ Index = $i; Length = 2; Text = $two })
                $i++
                continue
            }
        }
        if ($ch -eq '<' -or $ch -eq '>') {
            $operators.Add([pscustomobject]@{ Index = $i; Length = 1; Text = [string]$ch })
            continue
        }
        if ($i -le $Text.Length - 2 -and $Text.Substring($i, 2) -ceq 'in') {
            $beforeOk = $i -eq 0 -or $Text[$i - 1] -notmatch '[A-Za-z0-9_]'
            $afterIndex = $i + 2
            $afterOk = $afterIndex -ge $Text.Length -or $Text[$afterIndex] -notmatch '[A-Za-z0-9_]'
            if ($beforeOk -and $afterOk) {
                $operators.Add([pscustomobject]@{ Index = $i; Length = 2; Text = 'in' })
                $i++
            }
        }
    }
    return $operators.ToArray()
}

function Get-SuraTopLevelAdditiveOperators {
    param([Parameter(Mandatory=$true)][string]$Text)

    $operators = New-Object System.Collections.Generic.List[object]
    $round = 0
    $square = 0
    $curly = 0
    for ($i = 0; $i -lt $Text.Length; $i++) {
        $ch = $Text[$i]
        switch ($ch) {
            '(' { $round++; continue }
            ')' { $round--; continue }
            '[' { $square++; continue }
            ']' { $square--; continue }
            '{' { $curly++; continue }
            '}' { $curly--; continue }
        }
        if ($round -ne 0 -or $square -ne 0 -or $curly -ne 0) { continue }
        if ($ch -ne '+' -and $ch -ne '-') { continue }

        $prev = $i - 1
        while ($prev -ge 0 -and [char]::IsWhiteSpace($Text[$prev])) { $prev-- }
        if ($prev -lt 0 -or $Text[$prev] -match '[\(\[\{,:+\-*/%!?<>=&|]') { continue }
        if (($Text[$prev] -eq 'e' -or $Text[$prev] -eq 'E') -and $prev -gt 0 -and [char]::IsDigit($Text[$prev - 1]) -and $i + 1 -lt $Text.Length -and [char]::IsDigit($Text[$i + 1])) { continue }
        $operators.Add([pscustomobject]@{ Index = $i; Length = 1; Text = [string]$ch })
    }
    return $operators.ToArray()
}

function Convert-SuraRuntimeAdditiveOperators {
    param([Parameter(Mandatory=$true)][string]$Text)

    $operators = @(Get-SuraTopLevelAdditiveOperators $Text)
    if ($operators.Count -eq 0) { return $Text.Trim() }

    $first = $operators[0]
    $result = $Text.Substring(0, $first.Index).Trim()
    for ($i = 0; $i -lt $operators.Count; $i++) {
        $op = $operators[$i]
        $rightStart = $op.Index + $op.Length
        $rightEnd = if ($i + 1 -lt $operators.Count) { $operators[$i + 1].Index } else { $Text.Length }
        $right = $Text.Substring($rightStart, $rightEnd - $rightStart).Trim()
        if ($op.Text -eq '+') {
            $result = "__sura_add($result, $right)"
        } else {
            $result = "($result - $right)"
        }
    }
    return $result
}

function Convert-SuraRuntimeComparisonOperators {
    param([Parameter(Mandatory=$true)][string]$Text)

    $operators = @(Get-SuraTopLevelComparisonOperators $Text)
    if ($operators.Count -eq 0) { return Convert-SuraRuntimeAdditiveOperators $Text }

    $first = $operators[0]
    $result = Convert-SuraRuntimeAdditiveOperators $Text.Substring(0, $first.Index)
    for ($i = 0; $i -lt $operators.Count; $i++) {
        $op = $operators[$i]
        $rightStart = $op.Index + $op.Length
        $rightEnd = if ($i + 1 -lt $operators.Count) { $operators[$i + 1].Index } else { $Text.Length }
        $right = Convert-SuraRuntimeAdditiveOperators $Text.Substring($rightStart, $rightEnd - $rightStart)
        if ($op.Text -eq 'in') {
            $result = "__sura_in($result, $right)"
        } else {
            $result = "$result $($op.Text) $right"
        }
    }
    return $result
}

function Convert-SuraRuntimeLogicalAndOperators {
    param([Parameter(Mandatory=$true)][string]$Text)

    $positions = @(Get-SuraTopLevelWordOperatorPositions $Text 'and')
    if ($positions.Count -eq 0) { return Convert-SuraRuntimeComparisonOperators $Text }
    $parts = New-Object System.Collections.Generic.List[string]
    $start = 0
    foreach ($position in $positions) {
        $parts.Add((Convert-SuraRuntimeComparisonOperators $Text.Substring($start, $position - $start)))
        $start = $position + 3
    }
    $parts.Add((Convert-SuraRuntimeComparisonOperators $Text.Substring($start)))
    return '(' + ($parts -join ' && ') + ')'
}

function Convert-SuraRuntimeLogicalOrOperators {
    param([Parameter(Mandatory=$true)][string]$Text)

    $positions = @(Get-SuraTopLevelWordOperatorPositions $Text 'or')
    if ($positions.Count -eq 0) { return Convert-SuraRuntimeLogicalAndOperators $Text }
    $parts = New-Object System.Collections.Generic.List[string]
    $start = 0
    foreach ($position in $positions) {
        $parts.Add((Convert-SuraRuntimeLogicalAndOperators $Text.Substring($start, $position - $start)))
        $start = $position + 2
    }
    $parts.Add((Convert-SuraRuntimeLogicalAndOperators $Text.Substring($start)))
    return '(' + ($parts -join ' || ') + ')'
}

function Convert-SuraRuntimeOperatorList {
    param(
        [Parameter(Mandatory=$true)][AllowEmptyString()][string]$Text,
        [switch]$Dictionary
    )

    if ($Text.Trim() -eq '') { return $Text }
    $converted = New-Object System.Collections.Generic.List[string]
    foreach ($part in @(Split-SuraTopLevelExpressionList $Text)) {
        if ($Dictionary) {
            $colon = Find-SuraTopLevelExpressionColon $part
            if ($colon -ge 0) {
                $key = $part.Substring(0, $colon).Trim()
                $value = Convert-SuraRuntimeOperators $part.Substring($colon + 1)
                $converted.Add("$key`: $value")
                continue
            }
        }
        $converted.Add((Convert-SuraRuntimeOperators $part))
    }
    return $converted -join ', '
}

function Convert-SuraRuntimeOperators {
    param([Parameter(Mandatory=$true)][AllowEmptyString()][string]$Text)

    $out = New-Object System.Text.StringBuilder
    for ($i = 0; $i -lt $Text.Length; $i++) {
        $ch = $Text[$i]
        if ($ch -ne '(' -and $ch -ne '[' -and $ch -ne '{') {
            [void]$out.Append($ch)
            continue
        }
        $close = Get-SuraMatchingExpressionDelimiterIndex $Text $i
        if ($close -lt 0) {
            [void]$out.Append($ch)
            continue
        }
        $inner = $Text.Substring($i + 1, $close - $i - 1)
        $convertedInner = Convert-SuraRuntimeOperatorList $inner -Dictionary:($ch -eq '{')
        [void]$out.Append($ch)
        [void]$out.Append($convertedInner)
        [void]$out.Append($Text[$close])
        $i = $close
    }
    return Convert-SuraRuntimeLogicalOrOperators $out.ToString()
}

function Convert-SuraExpression {
    param([Parameter(Mandatory=$true)][string]$Expr)

    $e = $Expr.Trim()
    $lambda = Convert-SuraLambdaExpression $e
    if ($null -ne $lambda) { return $lambda }
    $e = Convert-SuraInlineFunctionExpressions $e

    $stringLiterals = New-Object System.Collections.Generic.List[string]
    $e = [regex]::Replace($e, '"([^"\\]*(?:\\.[^"\\]*)*)"', [System.Text.RegularExpressions.MatchEvaluator]{
        param($m)
        $idx = $stringLiterals.Count
        $stringLiterals.Add((Convert-SuraStringLiteral $m.Value)) | Out-Null
        return "__SURA_STRING_LITERAL_$idx`__"
    })

    $e = Convert-SuraRuntimeOperators $e
    $e = [regex]::Replace($e, '\bnil\b', 'null')
    $e = [regex]::Replace($e, '\band\b', '&&')
    $e = [regex]::Replace($e, '\bor\b', '||')
    $e = [regex]::Replace($e, '\bnot\s+', '!')
    $e = [regex]::Replace($e, '\bself\b', 'this')
    $e = [regex]::Replace($e, '\bsuper\.init\s*\(', 'super(')

    $receiverSource = $e
    $e = [regex]::Replace($e, '\.(?:len|length|size)\(\)', [System.Text.RegularExpressions.MatchEvaluator]{
        param($m)
        $before = $receiverSource.Substring(0, $m.Index)
        $receiver = ""
        if ($before -match '([A-Za-z_][A-Za-z0-9_]*)$') { $receiver = $Matches[1] }
        if ($receiver -in @("array", "string", "dict", "set", "math", "path", "os", "cli", "json", "fs", "regex", "datetime", "crypto", "db", "log", "console", "http", "async", "test", "random", "python", "ffi", "plugin", "vector", "graphics3d", "g3d", "rag", "tensor", "stream", "tool", "llm")) {
            return $m.Value
        }
        return ".length"
    })
    $e = $e -replace '\.upper\(\)', '.toUpperCase()'
    $e = $e -replace '\.lower\(\)', '.toLowerCase()'
    $e = [regex]::Replace($e, '\.contains\(([^()]*)\)', '.includes($1)')
    $e = $e -replace '\.index_of\(', '.indexOf('
    $e = $e -replace '\.starts_with\(', '.startsWith('
    $e = $e -replace '\.ends_with\(', '.endsWith('
    $e = $e -replace '\.sub\(', '.slice('
    $e = Convert-SuraDivisionByZeroExpressions $e

    for ($i = 0; $i -lt $stringLiterals.Count; $i++) {
        $e = $e.Replace("__SURA_STRING_LITERAL_$i`__", $stringLiterals[$i])
    }

    return $e
}

function Convert-SimpleStatement {
    param(
        [Parameter(Mandatory=$true)][string]$Text,
        [switch]$Inline
    )

    $t = $Text.Trim()
    if ($t -eq "") { return "" }

    if ($t -match '^return(?:\s+(.+))?$') {
        if ($Matches[1]) { return "return $(Convert-SuraExpression $Matches[1]);" }
        return "return;"
    }

    if ($t -match '^throw(?:\s+(.+))?$') {
        if ($Matches[1]) { return "throw $(Convert-SuraExpression $Matches[1]);" }
        return "throw null;"
    }

    if ($t -match '^(assert|assert_eq|assert_ne|assert_neq|assert_contains|assert_not_contains|assert_match|assert_type|assert_len|assert_between|assert_approx)\s+(.+)$') {
        return "$($Matches[1])($(Convert-SuraExpression $Matches[2]));"
    }

    if ($t -match '^(print|print_n|print_no_nl)\s+(.+)$') {
        return "$($Matches[1])($(Convert-SuraExpression $Matches[2]));"
    }

    if ($t -match '^type\s+(.+)$') {
        return "type($(Convert-SuraExpression $Matches[1]));"
    }

    if ($t -match '^clock\s*$') {
        return "clock();"
    }

    if ($t -match '^input(?:\s+(.+))?$') {
        if ($Matches[1]) { return "input($(Convert-SuraExpression $Matches[1]));" }
        return "input();"
    }

    if ($t -match '^exit(?:\s+(.+))?$') {
        if ($Matches[1]) { return "exit($(Convert-SuraExpression $Matches[1]));" }
        return "exit();"
    }

    if ($t -match '^random\s+(.+?)\s+~\s+(.+?)\s+([A-Za-z_][A-Za-z0-9_]*)$') {
        $expr = "random($(Convert-SuraExpression $Matches[1]), $(Convert-SuraExpression $Matches[2]))"
        $name = $Matches[3]
        if ($script:declared.ContainsKey($name)) {
            return "$name = $expr;"
        }
        $script:declared[$name] = $true
        return "var $name = $expr;"
    }

    if ($t -match '^([A-Za-z_][A-Za-z0-9_]*)\s*(?::\s*[^=]+?)?\s+is\s+(.+)$') {
        $name = $Matches[1]
        $expr = Convert-SuraExpression $Matches[2]
        if ($script:declared.ContainsKey($name)) {
            return "$name = $expr;"
        }
        $script:declared[$name] = $true
        return "var $name = $expr;"
    }

    if ($t -match '^(.+?)(\[[^\]]+\]|\.[A-Za-z_][A-Za-z0-9_]*)\s+is\s+(.+)$') {
        return "$(Convert-SuraExpression ($Matches[1] + $Matches[2])) = $(Convert-SuraExpression $Matches[3]);"
    }

    if ($t -match '^([A-Za-z_][A-Za-z0-9_]*(?:\[[^\]]+\]|\.[A-Za-z_][A-Za-z0-9_]*)*)\s*(\+=|-=|\*=|/=|%=)\s*(.+)$') {
        return "$(Convert-SuraExpression $Matches[1]) $($Matches[2]) $(Convert-SuraExpression $Matches[3]);"
    }

    if ($t -match '^(break|continue)$') {
        return "$t;"
    }

    if ($t -match '^[A-Za-z_][A-Za-z0-9_]*(?:\.[A-Za-z_][A-Za-z0-9_]*)?\s*\(.*\)$') {
        return "$(Convert-SuraExpression $t);"
    }

    if ($Inline) {
        throw "unsupported inline statement: $t"
    }

    return "$(Convert-SuraExpression $t);"
}

$js = New-Object System.Collections.Generic.List[string]
$js.Add("// Generated by sura_to_js.ps1. Portable Sura subset target.")
$js.Add("// JS target class/extends, lambda, when, and try/catch/finally lowering keeps more core language syntax runnable off the native VM.")
$js.Add('"use strict";')
$js.Add("const __sura_fs = typeof require === 'function' ? require('fs') : null;")
$js.Add("const __sura_path = (() => { try { return typeof require === 'function' ? require('path') : null; } catch (_) { return null; } })();")
$js.Add("const __sura_crypto = (() => { try { return typeof require === 'function' ? require('crypto') : null; } catch (_) { return null; } })();")
$js.Add("const __sura_child_process = (() => { try { return typeof require === 'function' ? require('child_process') : null; } catch (_) { return null; } })();")
$js.Add("const __sura_node_os = (() => { try { return typeof require === 'function' ? require('os') : null; } catch (_) { return null; } })();")
$js.Add("const __sura_Buffer = typeof Buffer === 'function' ? Buffer : null;")
$js.Add("const __sura_process = typeof process === 'object' ? process : null;")
$js.Add("const __sura_host_console = (typeof globalThis !== 'undefined' && globalThis.console) ? globalThis.console : { log: () => {}, info: () => {}, warn: () => {}, error: () => {}, debug: () => {}, clear: () => {} };")
$js.Add("const nil = null;")
$js.Add("const print = (...x) => __sura_host_console.log(...x);")
$js.Add("function __sura_print_text(value) { if (value === null || value === undefined) return 'nil'; if (typeof value === 'boolean') return value ? 'true' : 'false'; if (typeof value === 'number') return Number.isInteger(value) ? String(value) : String(value); if (typeof value === 'string') return value; if (Array.isArray(value) || typeof value === 'object') return JSON.stringify(value); return String(value); }")
$js.Add("function __sura_add(left, right) { if (typeof left === 'number' && typeof right === 'number') return left + right; if (typeof left === 'string' || typeof right === 'string') return __sura_print_text(left) + __sura_print_text(right); if (Array.isArray(left) && Array.isArray(right)) return left.concat(right); return null; }")
$js.Add("function __sura_in(needle, receiver) { if (Array.isArray(receiver)) return receiver.includes(needle); if (typeof receiver === 'string') return receiver.includes(__sura_print_text(needle)); if (receiver && typeof receiver === 'object') return Object.prototype.hasOwnProperty.call(receiver, __sura_print_text(needle)); return null; }")
$js.Add("const print_n = (...x) => { const out = x.map(__sura_print_text).join(''); if (__sura_process && __sura_process.stdout && typeof __sura_process.stdout.write === 'function') __sura_process.stdout.write(out); else __sura_host_console.log(out); return null; };")
$js.Add("const print_no_nl = print_n;")
$js.Add("function input(prompt = '') { if (prompt !== undefined && prompt !== null && String(prompt) !== '') print_n(prompt); if (!__sura_fs || typeof __sura_fs.readFileSync !== 'function') return ''; try { return String(__sura_fs.readFileSync(0, 'utf8')).replace(/\r?\n$/, ''); } catch (_) { return ''; } }")
$js.Add("function exit(code = 0) { const value = Number(code) || 0; if (__sura_process && typeof __sura_process.exit === 'function') __sura_process.exit(value); throw new Error('exit(): process exit is unavailable'); }")
$js.Add("let __sura_log_file = '';")
$js.Add("let __sura_log_json = false;")
$js.Add("let __sura_log_level = 'DEBUG';")
$js.Add("const sqrt = Math.sqrt, sin = Math.sin, cos = Math.cos, tan = Math.tan;")
$js.Add("const floor = Math.floor, ceil = Math.ceil, round = Math.round, abs = Math.abs, pow = Math.pow;")
$js.Add("const min = Math.min, max = Math.max;")
$js.Add("function __sura_div(a, b) { const denom = Number(b); if (denom === 0) throw new Error('division by zero'); return Number(a) / denom; }")
$js.Add("let __sura_rng_state = null;")
$js.Add("function __sura_rng_next() { if (__sura_rng_state === null) return Math.random(); __sura_rng_state = ((__sura_rng_state * 1664525 + 1013904223) >>> 0); return __sura_rng_state / 4294967296; }")
$js.Add("function __sura_int_arg(name, value) { const n = Number(value); if (!Number.isFinite(n) || Math.floor(n) !== n) throw new Error(name + '(): argument must be an integer'); return n; }")
$js.Add("function __sura_for_step(value) { const n = Number(value); if (!Number.isFinite(n) || n === 0) throw new Error('for step must be a non-zero finite number'); return n; }")
$js.Add("function __sura_iter(value) { if (value === null || value === undefined) return []; if (Array.isArray(value) || typeof value === 'string') return value; if (typeof value[Symbol.iterator] === 'function') return value; return []; }")
$js.Add("function __sura_entries(value) { if (value === null || value === undefined) return []; if (Array.isArray(value) || typeof value === 'string') return Array.from(value).map((item, index) => [index, item]); if (typeof value === 'object') return Object.entries(value); if (typeof value[Symbol.iterator] === 'function') return Array.from(value).map((item, index) => [index, item]); return []; }")
$js.Add("function __sura_define_method(proto, name, fn) { if (!Object.prototype.hasOwnProperty.call(proto, name)) Object.defineProperty(proto, name, { value: fn, configurable: true, writable: true, enumerable: false }); }")
$js.Add("__sura_define_method(String.prototype, 'index_of', function(value) { return String(this).indexOf(value); });")
$js.Add("__sura_define_method(String.prototype, 'starts_with', function(value) { return String(this).startsWith(value); });")
$js.Add("__sura_define_method(String.prototype, 'ends_with', function(value) { return String(this).endsWith(value); });")
$js.Add("__sura_define_method(String.prototype, 'sub', function(start, end = undefined) { return String(this).slice(start, end); });")
$js.Add("__sura_define_method(String.prototype, 'to_num', function() { const n = Number(String(this)); return Number.isNaN(n) ? null : n; });")
$js.Add("__sura_define_method(Array.prototype, 'index_of', function(value) { return this.indexOf(value); });")
$js.Add("__sura_define_method(Array.prototype, 'sub', function(start, end = undefined) { return this.slice(start, end); });")
$js.Add("__sura_define_method(Object.prototype, 'has', function(key) { return Object.prototype.hasOwnProperty.call(this, key); });")
$js.Add("__sura_define_method(Object.prototype, 'keys', function() { return Object.keys(this); });")
$js.Add("__sura_define_method(Object.prototype, 'values', function() { return Object.values(this); });")
$js.Add("__sura_define_method(Object.prototype, 'delete', function(key) { const ok = Object.prototype.hasOwnProperty.call(this, key); delete this[key]; return ok; });")
$js.Add("function random_seed(seed) { seed = __sura_int_arg('random_seed', seed); if (seed < 0) throw new Error('random_seed(): seed must be non-negative'); __sura_rng_state = seed >>> 0; return seed; }")
$js.Add("function random(a, b) { if (a === undefined) return __sura_rng_next(); if (b === undefined) return random_int(a); return random_int(a, b); }")
$js.Add("function random_int(a, b) { let lo = 0, hi = __sura_int_arg('random_int', a); if (b === undefined) { if (hi <= 0) throw new Error('random_int(): max must be positive'); hi -= 1; } else { lo = hi; hi = __sura_int_arg('random_int', b); if (hi < lo) throw new Error('random_int(): max must be greater than or equal to min'); } return lo + Math.floor(__sura_rng_next() * (hi - lo + 1)); }")
$js.Add("function random_float(a = undefined, b = undefined) { let lo = 0, hi = 1; if (a !== undefined && b === undefined) { hi = Number(a); } else if (a !== undefined && b !== undefined) { lo = Number(a); hi = Number(b); } if (!Number.isFinite(lo) || !Number.isFinite(hi) || hi < lo) throw new Error('random_float(): max must be greater than or equal to min'); return lo + __sura_rng_next() * (hi - lo); }")
$js.Add("function random_bool(probability = 0.5) { probability = Number(probability); if (!Number.isFinite(probability) || probability < 0 || probability > 1) throw new Error('random_bool(): probability must be between 0 and 1'); return __sura_rng_next() < probability; }")
$js.Add("function random_choice(arr) { if (!Array.isArray(arr) || arr.length === 0) throw new Error('random_choice(): array must not be empty'); return arr[random_int(arr.length)]; }")
$js.Add("function random_shuffle(arr) { if (!Array.isArray(arr)) throw new Error('random_shuffle(): argument must be an array'); const out = arr.slice(); for (let i = out.length - 1; i > 0; i--) { const j = random_int(0, i); const tmp = out[i]; out[i] = out[j]; out[j] = tmp; } return out; }")
$js.Add("function random_bytes(count) { count = __sura_int_arg('random_bytes', count); if (count < 0) throw new Error('random_bytes(): count must be non-negative'); if (count > 1048576) throw new Error('random_bytes(): count exceeds 1048576'); const out = []; for (let i = 0; i < count; i++) out.push(random_int(0, 255)); return out; }")
$js.Add("function uuid_v4() { const b = random_bytes(16); b[6] = (b[6] & 15) | 64; b[8] = (b[8] & 63) | 128; const h = b.map(x => x.toString(16).padStart(2, '0')); return h.slice(0, 4).join('') + '-' + h.slice(4, 6).join('') + '-' + h.slice(6, 8).join('') + '-' + h.slice(8, 10).join('') + '-' + h.slice(10, 16).join(''); }")
$js.Add("const uuid = uuid_v4;")
$js.Add("Object.assign(random, { random, seed: random_seed, int: random_int, integer: random_int, float: random_float, number: random_float, bool: random_bool, choice: random_choice, shuffle: random_shuffle, bytes: random_bytes, uuid: uuid_v4, random_seed, random_int, random_float, random_bool, random_choice, random_shuffle, random_bytes });")
$js.Add("function clock() { return Date.now() / 1000; }")
$js.Add("function argv() { return __sura_process ? __sura_process.argv.slice(2) : []; }")
$js.Add("function argc() { return argv().length; }")
$js.Add("function script_name() { return __sura_process && __sura_process.argv.length > 1 ? __sura_process.argv[1] : ''; }")
$js.Add("function __sura_env_name(name, fn) { name = String(name); if (!/^[A-Za-z_][A-Za-z0-9_]*$/.test(name)) throw new Error(fn + '(): invalid environment variable name ' + JSON.stringify(name)); return name; }")
$js.Add("function env_get(name, fallback = null) { name = __sura_env_name(name, 'env_get'); if (!__sura_process || !__sura_process.env || __sura_process.env[name] === undefined) return fallback; return String(__sura_process.env[name]); }")
$js.Add("function env_require(name) { const value = env_get(name, null); if (value === null) throw new Error('env_require(): missing environment variable ' + JSON.stringify(String(name))); return value; }")
$js.Add("function env_set(name, value) { name = __sura_env_name(name, 'env_set'); if (!__sura_process || !__sura_process.env) throw new Error('env_set(): process env is unavailable'); __sura_process.env[name] = String(value); return true; }")
$js.Add("function home_dir() { if (__sura_node_os && typeof __sura_node_os.homedir === 'function') return __sura_node_os.homedir(); return env_get('HOME', env_get('USERPROFILE', '')); }")
$js.Add("function temp_dir() { if (__sura_node_os && typeof __sura_node_os.tmpdir === 'function') return __sura_node_os.tmpdir(); return env_get('TMPDIR', env_get('TEMP', env_get('TMP', ''))); }")
$js.Add("function path_separator() { return __sura_path && __sura_path.sep ? __sura_path.sep : (__sura_process && __sura_process.platform === 'win32' ? '\\' : '/'); }")
$js.Add("function os_name() { const p = __sura_process && __sura_process.platform ? __sura_process.platform : ''; if (p === 'win32') return 'windows'; if (p === 'darwin') return 'macos'; if (p === 'linux') return 'linux'; if (p) return p; return 'unknown'; }")
$js.Add("function is_windows() { return os_name() === 'windows'; }")
$js.Add("function __sura_path_list() { const raw = env_get('PATH', ''); return raw ? String(raw).split(is_windows() ? ';' : ':') : []; }")
$js.Add("function __sura_executable_suffixes(command) { if (!is_windows()) return ['']; const p = __sura_require_path('which'); if (p.extname(String(command))) return ['']; const raw = env_get('PATHEXT', '.COM;.EXE;.BAT;.CMD'); return [''].concat(String(raw).split(';').filter(Boolean).map(ext => ext.startsWith('.') ? ext : '.' + ext)); }")
$js.Add("function __sura_executable_candidate(path) { const fs = __sura_require_fs('which'); try { const st = fs.statSync(path); if (!st.isFile()) return false; if (!is_windows() && fs.accessSync) fs.accessSync(path, fs.constants.X_OK); return true; } catch (_) { return false; } }")
$js.Add("function which(command) { const fs = __sura_require_fs('which'); const p = __sura_require_path('which'); command = String(command); if (!command) return ''; const direct = p.isAbsolute(command) || command.includes('/') || command.includes('\\\\'); const dirs = direct ? [''] : __sura_path_list(); for (const dir of dirs) { const base = direct ? command : p.join(dir || '.', command); for (const suffix of __sura_executable_suffixes(command)) { const candidate = base + suffix; if (__sura_executable_candidate(candidate)) return p.resolve(candidate); } } return ''; }")
$js.Add("function cmd_exists(command) { return which(command) !== ''; }")
$js.Add("const command_exists = cmd_exists;")
$js.Add("function __sura_cmd_quote_safe(ch) { const code = ch.charCodeAt(0); if ((code >= 48 && code <= 57) || (code >= 65 && code <= 90) || (code >= 97 && code <= 122)) return true; if ('_-./:@+=,'.includes(ch)) return true; if (is_windows() && ch === '\\') return true; if (!is_windows() && ch === '%') return true; return false; }")
$js.Add("function cmd_quote(text) { text = String(text); const dq = String.fromCharCode(34); const sq = String.fromCharCode(39); if (text === '') return is_windows() ? dq + dq : sq + sq; let safe = true; for (const ch of text) { if (!__sura_cmd_quote_safe(ch)) { safe = false; break; } } if (safe) return text; if (is_windows()) { let out = dq; for (const ch of text) { if (ch === dq) out += '\\' + dq; else if (ch === '^') out += '^^'; else if (ch === '%') out += '^%'; else if (ch === '!') out += '^!'; else out += ch; } return out + dq; } return sq + text.replace(/'/g, sq + '\\' + sq + sq) + sq; }")
$js.Add("function cmd_join(args) { if (!Array.isArray(args)) throw new Error('cmd_join(): arg 1 must be an array'); return args.map(x => { if (typeof x !== 'string') throw new Error('cmd_join(): all args must be strings'); return cmd_quote(x); }).join(' '); }")
$js.Add("function __sura_env_value(raw) { raw = String(raw).trim(); if (raw === '') return ''; const quote = raw[0]; if (quote === String.fromCharCode(34) || quote === String.fromCharCode(39)) { let out = '', escaped = false; for (let i = 1; i < raw.length; i++) { const ch = raw[i]; if (escaped) { out += ch === 'n' ? '\n' : ch === 'r' ? '\r' : ch === 't' ? '\t' : ch; escaped = false; continue; } if (quote === String.fromCharCode(34) && ch === '\\') { escaped = true; continue; } if (ch === quote) return out; out += ch; } throw new Error('env_load(): unterminated quoted value'); } const hash = raw.search(/\s#/); if (hash >= 0) raw = raw.slice(0, hash); return raw.trim(); }")
$js.Add("function env_load(path, overrideExisting = false) { const loaded = {}; const lines = file_read(path).replace(/^\uFEFF/, '').split(/\r?\n/); for (let line of lines) { let work = line.trim(); if (!work || work.startsWith('#')) continue; work = work.replace(/^export\s+/, '').trim(); const eq = work.indexOf('='); if (eq < 0) throw new Error('env_load(): expected KEY=value'); const name = __sura_env_name(work.slice(0, eq).trim(), 'env_load'); if (!overrideExisting && env_get(name, null) !== null) continue; const value = __sura_env_value(work.slice(eq + 1)); env_set(name, value); loaded[name] = value; } return loaded; }")
$js.Add("function __sura_cli_tokens(text) { const tokens = []; let cur = '', quote = null, escaped = false, had = false; for (const ch of String(text)) { if (escaped) { cur += ch; escaped = false; had = true; continue; } if (ch === '\\') { escaped = true; had = true; continue; } if (quote) { if (ch === quote) quote = null; else cur += ch; had = true; continue; } const code = ch.charCodeAt(0); if (code === 34 || code === 39) { quote = ch; had = true; continue; } if (/\s/.test(ch)) { if (had) { tokens.push(cur); cur = ''; had = false; } continue; } cur += ch; had = true; } if (escaped) cur += '\\'; if (had) tokens.push(cur); return tokens; }")
$js.Add("function __sura_cli_value_flags(spec = []) { const out = new Set(); const add = x => String(x).split(/[\s,]+/).filter(Boolean).forEach(v => out.add(v)); if (typeof spec === 'string') add(spec); else if (Array.isArray(spec)) spec.forEach(add); else if (spec && typeof spec === 'object') Object.keys(spec).forEach(k => { if (spec[k]) out.add(k); }); else throw new Error('cli_parse(): arg 2 must be a string, array, or dict of value-taking flags'); return out; }")
$js.Add("function __sura_cli_is_value_token(token) { token = String(token); if (token === '' || token === '-') return true; if (token.length >= 2 && token[0] === '-' && /[0-9.]/.test(token[1])) return true; return !(token.length >= 2 && token[0] === '-'); }")
$js.Add("function __sura_cli_add_value(out, key, value) { if (!Object.prototype.hasOwnProperty.call(out, key)) { out[key] = value; return; } if (!Array.isArray(out[key])) out[key] = [out[key]]; out[key].push(value); }")
$js.Add("function cli_parse(text, valueFlags = []) { const tokens = __sura_cli_tokens(text); const flags = __sura_cli_value_flags(valueFlags); const out = {args: []}; let positionalOnly = false; for (let i = 0; i < tokens.length; i++) { let tok = tokens[i]; if (positionalOnly) { out.args.push(tok); continue; } if (tok === '--') { positionalOnly = true; continue; } if (tok.startsWith('--') && tok.length > 2) { tok = tok.slice(2); const eq = tok.indexOf('='); if (eq >= 0) __sura_cli_add_value(out, tok.slice(0, eq), tok.slice(eq + 1)); else if (tok.startsWith('no-') && tok.length > 3) __sura_cli_add_value(out, tok.slice(3), false); else if (flags.has(tok) && i + 1 < tokens.length && __sura_cli_is_value_token(tokens[i + 1])) __sura_cli_add_value(out, tok, tokens[++i]); else __sura_cli_add_value(out, tok, true); } else if (tok.length > 1 && tok[0] === '-' && !__sura_cli_is_value_token(tok)) { const shortFlags = tok.slice(1); const eq = shortFlags.indexOf('='); if (eq >= 0) __sura_cli_add_value(out, shortFlags.slice(0, eq), shortFlags.slice(eq + 1)); else if (shortFlags.length === 1 && flags.has(shortFlags) && i + 1 < tokens.length && __sura_cli_is_value_token(tokens[i + 1])) __sura_cli_add_value(out, shortFlags, tokens[++i]); else for (const flag of shortFlags) __sura_cli_add_value(out, flag, true); } else out.args.push(tok); } return out; }")
$js.Add("function __sura_require_crypto(name) { if (!__sura_crypto) throw new Error(name + '(): crypto is unavailable'); return __sura_crypto; }")
$js.Add("function __sura_require_buffer(name) { if (!__sura_Buffer) throw new Error(name + '(): Buffer is unavailable'); return __sura_Buffer; }")
$js.Add("function sha256(text) { return __sura_require_crypto('sha256').createHash('sha256').update(String(text), 'utf8').digest('hex'); }")
$js.Add("function file_sha256(path) { const crypto = __sura_require_crypto('file_sha256'); const fs = __sura_require_fs('file_sha256'); return crypto.createHash('sha256').update(fs.readFileSync(String(path))).digest('hex'); }")
$js.Add("const sha256_file = file_sha256;")
$js.Add("function hmac_sha256(key, message) { return __sura_require_crypto('hmac_sha256').createHmac('sha256', String(key)).update(String(message), 'utf8').digest('hex'); }")
$js.Add("function file_hmac_sha256(key, path) { const crypto = __sura_require_crypto('file_hmac_sha256'); const fs = __sura_require_fs('file_hmac_sha256'); return crypto.createHmac('sha256', String(key)).update(fs.readFileSync(String(path))).digest('hex'); }")
$js.Add("const hmac_sha256_file = file_hmac_sha256;")
$js.Add("function crypto_random_bytes(count) { count = __sura_int_arg('crypto_random_bytes', count); if (count < 0) throw new Error('crypto_random_bytes(): count must be non-negative'); if (count > 1048576) throw new Error('crypto_random_bytes(): count exceeds 1048576'); return Array.from(__sura_require_crypto('crypto_random_bytes').randomBytes(count)); }")
$js.Add("const secure_random_bytes = crypto_random_bytes;")
$js.Add("function crypto_random_hex(count) { count = __sura_int_arg('crypto_random_hex', count); if (count < 0) throw new Error('crypto_random_hex(): count must be non-negative'); if (count > 1048576) throw new Error('crypto_random_hex(): count exceeds 1048576'); return __sura_require_crypto('crypto_random_hex').randomBytes(count).toString('hex'); }")
$js.Add("const secure_random_hex = crypto_random_hex;")
$js.Add("function constant_time_eq(left, right) { const Buffer = __sura_require_buffer('constant_time_eq'); left = Buffer.from(String(left), 'utf8'); right = Buffer.from(String(right), 'utf8'); const len = Math.max(left.length, right.length); let diff = left.length ^ right.length; for (let i = 0; i < len; i++) diff |= (i < left.length ? left[i] : 0) ^ (i < right.length ? right[i] : 0); return diff === 0; }")
$js.Add("const crypto_constant_time_eq = constant_time_eq;")
$js.Add("const secure_compare = constant_time_eq;")
$js.Add("function hex_encode(text) { return __sura_require_buffer('hex_encode').from(String(text), 'utf8').toString('hex'); }")
$js.Add("function hex_decode(text) { return __sura_require_buffer('hex_decode').from(String(text), 'hex').toString('utf8'); }")
$js.Add("function base64_encode(text) { return __sura_require_buffer('base64_encode').from(String(text), 'utf8').toString('base64'); }")
$js.Add("function base64_decode(text) { return __sura_require_buffer('base64_decode').from(String(text), 'base64').toString('utf8'); }")
$js.Add("function base64_url_encode(text) { let out = base64_encode(text).split('+').join('-').split('/').join('_'); while (out.endsWith('=')) out = out.slice(0, -1); return out; }")
$js.Add("function base64_url_decode(text) { let s = String(text).split('-').join('+').split('_').join('/'); if (s.length % 4 === 1) throw new Error('base64_url_decode(): invalid input length'); while (s.length % 4) s += '='; return base64_decode(s); }")
$js.Add("function url_encode(text) { return encodeURIComponent(String(text)); }")
$js.Add("function url_decode(text) { return decodeURIComponent(String(text)); }")
$js.Add("function regex_match(text, pattern) { return new RegExp(String(pattern)).test(String(text)); }")
$js.Add("function regex_replace(text, pattern, replacement) { return String(text).replace(new RegExp(String(pattern), 'g'), String(replacement)); }")
$js.Add("function regex_find_all(text, pattern) { const re = new RegExp(String(pattern), 'g'); const out = []; let m; text = String(text); while ((m = re.exec(text)) !== null) { out.push(m[0]); if (m[0] === '') re.lastIndex++; } return out; }")
$js.Add('function regex_escape(text) { return String(text).replace(/[\\^$.*+?()[\]{}|]/g, "\\$&"); }')
$js.Add("function __sura_regex_match_array(match) { return Array.from(match).map(v => v === undefined ? null : v); }")
$js.Add("function regex_capture(text, pattern) { const m = new RegExp(String(pattern)).exec(String(text)); return m ? __sura_regex_match_array(m) : null; }")
$js.Add("function regex_captures(text, pattern) { const re = new RegExp(String(pattern), 'g'); const out = []; let m; text = String(text); while ((m = re.exec(text)) !== null) { out.push(__sura_regex_match_array(m)); if (m[0] === '') re.lastIndex++; } return out; }")
$js.Add("function regex_split(text, pattern) { return String(text).split(new RegExp(String(pattern))); }")
$js.Add("function __sura_pad(value, width) { return String(Math.trunc(Math.abs(Number(value)))).padStart(width, '0'); }")
$js.Add("function __sura_datetime_date(timestamp, name) { const n = Number(timestamp); if (!Number.isFinite(n)) throw new Error(name + '(): timestamp must be a number'); const d = new Date(n * 1000); if (Number.isNaN(d.getTime())) throw new Error(name + '(): timestamp is out of range'); return d; }")
$js.Add("function __sura_day_of_year(d, utc) { const start = utc ? Date.UTC(d.getUTCFullYear(), 0, 1) : new Date(d.getFullYear(), 0, 1).getTime(); const current = utc ? Date.UTC(d.getUTCFullYear(), d.getUTCMonth(), d.getUTCDate()) : new Date(d.getFullYear(), d.getMonth(), d.getDate()).getTime(); return Math.floor((current - start) / 86400000) + 1; }")
$js.Add("function __sura_is_dst(d) { const jan = new Date(d.getFullYear(), 0, 1).getTimezoneOffset(); const jul = new Date(d.getFullYear(), 6, 1).getTimezoneOffset(); if (jan === jul) return 0; return d.getTimezoneOffset() === Math.min(jan, jul) ? 1 : 0; }")
$js.Add("function __sura_datetime_format(timestamp, fmt, utc) { const d = __sura_datetime_date(timestamp, utc ? 'datetime_utc_format' : 'datetime_format'); const part = token => { switch (token) { case '%Y': return String(utc ? d.getUTCFullYear() : d.getFullYear()); case '%m': return __sura_pad((utc ? d.getUTCMonth() : d.getMonth()) + 1, 2); case '%d': return __sura_pad(utc ? d.getUTCDate() : d.getDate(), 2); case '%H': return __sura_pad(utc ? d.getUTCHours() : d.getHours(), 2); case '%M': return __sura_pad(utc ? d.getUTCMinutes() : d.getMinutes(), 2); case '%S': return __sura_pad(utc ? d.getUTCSeconds() : d.getSeconds(), 2); case '%w': return String(utc ? d.getUTCDay() : d.getDay()); case '%j': return __sura_pad(__sura_day_of_year(d, utc), 3); case '%%': return '%'; default: return token; } }; return String(fmt).replace(/%[YmdHMSwj%]/g, part); }")
$js.Add("function __sura_datetime_from_parts(match, includeTime, includeSeconds) { const y = Number(match[1]), mo = Number(match[2]), day = Number(match[3]); const h = includeTime ? Number(match[4]) : 0; const mi = includeTime ? Number(match[5]) : 0; const s = includeSeconds ? Number(match[6]) : 0; const d = new Date(y, mo - 1, day, h, mi, s); if (Number.isNaN(d.getTime()) || d.getFullYear() !== y || d.getMonth() !== mo - 1 || d.getDate() !== day || d.getHours() !== h || d.getMinutes() !== mi || d.getSeconds() !== s) throw new Error('datetime_parse(): parsed time is out of range'); return Math.floor(d.getTime() / 1000); }")
$js.Add("function datetime_parse(text, fmt = '%Y-%m-%dT%H:%M:%S') { text = String(text); fmt = String(fmt); let m = null; if (fmt === '%Y-%m-%dT%H:%M:%S') { m = /^(\d{4})-(\d{1,2})-(\d{1,2})T(\d{1,2}):(\d{1,2}):(\d{1,2})$/.exec(text); if (m) return __sura_datetime_from_parts(m, true, true); } else if (fmt === '%Y-%m-%d %H:%M:%S') { m = /^(\d{4})-(\d{1,2})-(\d{1,2}) (\d{1,2}):(\d{1,2}):(\d{1,2})$/.exec(text); if (m) return __sura_datetime_from_parts(m, true, true); } else if (fmt === '%Y-%m-%d %H:%M') { m = /^(\d{4})-(\d{1,2})-(\d{1,2}) (\d{1,2}):(\d{1,2})$/.exec(text); if (m) return __sura_datetime_from_parts(m, true, false); } else if (fmt === '%Y-%m-%d') { m = /^(\d{4})-(\d{1,2})-(\d{1,2})$/.exec(text); if (m) return __sura_datetime_from_parts(m, false, false); } else { throw new Error('datetime_parse(): unsupported format ' + JSON.stringify(fmt)); } throw new Error('datetime_parse(): input does not match format'); }")
$js.Add("function datetime_format(timestamp, fmt) { return __sura_datetime_format(timestamp, fmt, false); }")
$js.Add("function datetime_utc_format(timestamp, fmt) { return __sura_datetime_format(timestamp, fmt, true); }")
$js.Add("function datetime_add(timestampValue, seconds) { return Number(timestampValue) + Number(seconds); }")
$js.Add("function datetime_diff(endTimestamp, startTimestamp) { return Number(endTimestamp) - Number(startTimestamp); }")
$js.Add("function timestamp() { return Math.floor(Date.now() / 1000); }")
$js.Add("function datetime_now() { return datetime_format(timestamp(), '%Y-%m-%dT%H:%M:%S'); }")
$js.Add("function datetime_parts(timestampValue, utc = false) { const d = __sura_datetime_date(timestampValue, 'datetime_parts'); utc = !!utc; const year = utc ? d.getUTCFullYear() : d.getFullYear(); const month = (utc ? d.getUTCMonth() : d.getMonth()) + 1; const day = utc ? d.getUTCDate() : d.getDate(); const hour = utc ? d.getUTCHours() : d.getHours(); const minute = utc ? d.getUTCMinutes() : d.getMinutes(); const second = utc ? d.getUTCSeconds() : d.getSeconds(); return { year, month, day, hour, minute, second, weekday: utc ? d.getUTCDay() : d.getDay(), yearday: __sura_day_of_year(d, utc), is_dst: utc ? 0 : __sura_is_dst(d), utc }; }")
$js.Add("function sleep_ms(ms) { ms = Math.trunc(Number(ms)); if (ms < 0) throw new Error('sleep_ms(): duration must be non-negative'); if (ms <= 0) return null; if (typeof SharedArrayBuffer === 'function' && typeof Atomics === 'object' && typeof Atomics.wait === 'function') { try { Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0, ms); return null; } catch (_) {} } const end = Date.now() + ms; while (Date.now() < end) {} return null; }")
$js.Add("const wait = sleep_ms;")
$js.Add("function __sura_nilish(x) { return x === null || x === undefined; }")
$js.Add("function type(x) { return __sura_nilish(x) ? 'nil' : Array.isArray(x) ? 'array' : typeof x === 'boolean' ? 'bool' : typeof x === 'number' ? 'number' : typeof x === 'string' ? 'string' : typeof x === 'function' ? 'function' : 'dict'; }")
$js.Add("function __eq(a, b) { if (__sura_nilish(a) && __sura_nilish(b)) return true; return JSON.stringify(a) === JSON.stringify(b); }")
$js.Add("function assert(cond, msg) { if (!cond) throw new Error(msg || 'assert failed'); }")
$js.Add("function assert_eq(actual, expected, msg) { if (!__eq(actual, expected)) throw new Error(msg || ('assert_eq failed: ' + JSON.stringify(actual) + ' != ' + JSON.stringify(expected))); }")
$js.Add("function assert_ne(actual, expected, msg) { if (__eq(actual, expected)) throw new Error(msg || 'assert_ne failed'); }")
$js.Add("const assert_neq = assert_ne;")
$js.Add("function assert_contains(container, value, msg) { if (!contains(container, value)) throw new Error(msg || 'assert_contains failed'); return true; }")
$js.Add("function assert_not_contains(container, value, msg) { if (contains(container, value)) throw new Error(msg || 'assert_not_contains failed'); return true; }")
$js.Add("function assert_match(text, pattern, msg) { if (!regex_match(text, pattern)) throw new Error(msg || 'assert_match failed'); return true; }")
$js.Add("function assert_type(value, expected, msg) { const actual = type(value); if (actual !== String(expected)) throw new Error(msg || ('assert_type failed: expected ' + String(expected) + ', got ' + actual)); return true; }")
$js.Add("function assert_len(value, expected, msg) { const actual = length(value); if (actual !== Math.trunc(Number(expected))) throw new Error(msg || ('assert_len failed: expected ' + String(expected) + ', got ' + String(actual))); return true; }")
$js.Add("function assert_between(value, low, high, msg) { const v = Number(value), lo = Number(low), hi = Number(high); if (v < lo || v > hi) throw new Error(msg || ('assert_between failed: expected ' + String(value) + ' between ' + String(low) + ' and ' + String(high))); return true; }")
$js.Add("function assert_approx(actual, expected, epsilon = 1e-9, msg = undefined) { if (typeof epsilon !== 'number') { msg = epsilon; epsilon = 1e-9; } epsilon = Number(epsilon); if (epsilon < 0) throw new Error('assert_approx(): epsilon must be non-negative'); if (Math.abs(Number(actual) - Number(expected)) > epsilon) throw new Error(msg || ('assert_approx failed: expected ' + String(actual) + ' ~= ' + String(expected) + ' within ' + String(epsilon))); return true; }")
$js.Add("function __sura_test_result(name, passed, message) { return {name: String(name), passed: !!passed, ok: !!passed, status: passed ? 'pass' : 'fail', message: String(message || ''), line: 0}; }")
$js.Add("function check(name, condition, message = undefined) { const passed = !!condition; return __sura_test_result(name, passed, message === undefined ? (passed ? '' : 'condition failed') : message); }")
$js.Add("function check_eq(name, actual, expected, message = undefined) { const passed = __eq(actual, expected); const out = __sura_test_result(name, passed, message === undefined ? (passed ? '' : 'expected ' + JSON.stringify(actual) + ' == ' + JSON.stringify(expected)) : message); out.actual = actual; out.expected = expected; return out; }")
$js.Add("function check_match(name, text, pattern, message = undefined) { let passed = false, msg = ''; try { passed = regex_match(text, pattern); msg = message === undefined ? (passed ? '' : 'expected ' + String(text) + ' to match ' + String(pattern)) : String(message); } catch (err) { msg = 'regex error: ' + err.message; } const out = __sura_test_result(name, passed, msg); out.text = String(text); out.pattern = String(pattern); return out; }")
$js.Add("function __sura_test_passed(item) { if (typeof item === 'boolean') return item; if (item && typeof item === 'object') { if (Object.prototype.hasOwnProperty.call(item, 'ok')) return !!item.ok; if (Object.prototype.hasOwnProperty.call(item, 'passed')) return !!item.passed; } return false; }")
$js.Add("function __sura_test_name(item, index) { return item && typeof item === 'object' && item.name !== undefined ? String(item.name) : 'check ' + (index + 1); }")
$js.Add("function __sura_test_message(item) { return item && typeof item === 'object' && item.message !== undefined ? String(item.message) : (__sura_test_passed(item) ? '' : 'failed'); }")
$js.Add("function test_summary(results) { if (!Array.isArray(results)) throw new Error('test_summary(): argument must be an array'); const failures = []; let passed = 0; for (const item of results) { if (__sura_test_passed(item)) passed++; else failures.push(item); } const total = results.length; const failed = total - passed; return {total, passed, failed, ok: failed === 0, failures}; }")
$js.Add("function test_report(results, title = 'Sura tests') { if (!Array.isArray(results)) throw new Error('test_report(): argument must be an array'); let passed = 0, failed = 0; const lines = [String(title)]; results.forEach((item, i) => { const ok = __sura_test_passed(item); if (ok) passed++; else failed++; const msg = __sura_test_message(item); lines.push('[' + (ok ? 'PASS' : 'FAIL') + '] ' + __sura_test_name(item, i) + (msg ? ' - ' + msg : '')); }); lines.push('total: ' + results.length + ', passed: ' + passed + ', failed: ' + failed); return lines.join('\n'); }")
$js.Add("function length(x) { return Array.isArray(x) || typeof x === 'string' ? x.length : Object.keys(x || {}).length; }")
$js.Add("const array_len = length, array_length = length, array_size = length;")
$js.Add("const string_len = length, string_length = length, string_size = length;")
$js.Add("function to_int(x) { return Math.trunc(Number(x)); }")
$js.Add("function to_float(x) { return Number(x); }")
$js.Add("function to_str(x) { return String(x); }")
$js.Add("function to_bool(x) { return !!x; }")
$js.Add("function json_parse(x) { return JSON.parse(x); }")
$js.Add("function json_try_parse(x, fallback = null) { try { return json_parse(x); } catch (_) { return fallback; } }")
$js.Add("function json_stringify(x) { return JSON.stringify(x); }")
$js.Add("function json_pretty(x, indent = 2) { indent = __sura_int_arg('json_pretty', indent); if (indent < 0 || indent > 16) throw new Error('json_pretty(): indent must be an integer from 0 to 16'); return JSON.stringify(x, null, indent); }")
$js.Add("const serialize = json_stringify, deserialize = json_parse;")
$js.Add("function contains(x, y) { return Array.isArray(x) || typeof x === 'string' ? x.includes(y) : Object.prototype.hasOwnProperty.call(x, y); }")
$js.Add("function split(x, sep) { return String(x).split(sep); }")
$js.Add("function join(x, sep) { return x.join(sep); }")
$js.Add("function trim(x) { return String(x).trim(); }")
$js.Add("function upper(x) { return String(x).toUpperCase(); }")
$js.Add("function lower(x) { return String(x).toLowerCase(); }")
$js.Add("const string_upper = upper, string_lower = lower, string_trim = trim;")
$js.Add("function startsWith(x, prefix) { return String(x).startsWith(String(prefix)); }")
$js.Add("function endsWith(x, suffix) { return String(x).endsWith(String(suffix)); }")
$js.Add("function indexOf(x, value) { return String(x).indexOf(String(value)); }")
$js.Add("const string_contains = contains, string_indexOf = indexOf, string_index_of = indexOf;")
$js.Add("const string_startsWith = startsWith, string_starts_with = startsWith, string_endsWith = endsWith, string_ends_with = endsWith;")
$js.Add("function substring(x, start, end = undefined) { return String(x).substring(start, end); }")
$js.Add("function replace(x, search, replacement) { return String(x).replaceAll(String(search), String(replacement)); }")
$js.Add("function slice(x, start, end = undefined) { return x.slice(start, end); }")
$js.Add("const string_substring = substring, string_replace = replace, string_slice = slice, string_sub = slice;")
$js.Add("function sort(x) { x.sort((a, b) => a < b ? -1 : (a > b ? 1 : 0)); return x; }")
$js.Add("function reverse(x) { x.reverse(); return x; }")
$js.Add("function concat(...items) { if (!items.length) return []; return items[0].concat(...items.slice(1)); }")
$js.Add("function clamp(x, lo, hi) { return Math.min(Math.max(Number(x), Number(lo)), Number(hi)); }")
$js.Add("function string_lines(text) { text = String(text).replace(/\r\n/g, '\n').replace(/\r/g, '\n'); if (text === '') return []; const parts = text.split('\n'); if (text.endsWith('\n')) parts.pop(); return parts; }")
$js.Add("function string_words(text) { const trimmed = String(text).trim(); return trimmed === '' ? [] : trimmed.split(/\s+/); }")
$js.Add("function string_repeat(text, count) { count = Math.trunc(Number(count)); if (!Number.isFinite(count) || count < 0) throw new Error('string_repeat(): arg 2 must be a non-negative integer'); return String(text).repeat(count); }")
$js.Add("function string_pad_left(text, width, fill = ' ') { text = String(text); width = Math.trunc(Number(width)); fill = String(fill); if (!Number.isFinite(width) || width < 0) throw new Error('string_pad_left(): arg 2 must be a non-negative integer'); if (fill === '') throw new Error('string_pad_left(): fill must not be empty'); return text.length >= width ? text : text.padStart(width, fill); }")
$js.Add("function string_pad_right(text, width, fill = ' ') { text = String(text); width = Math.trunc(Number(width)); fill = String(fill); if (!Number.isFinite(width) || width < 0) throw new Error('string_pad_right(): arg 2 must be a non-negative integer'); if (fill === '') throw new Error('string_pad_right(): fill must not be empty'); return text.length >= width ? text : text.padEnd(width, fill); }")
$js.Add("function push(arr, ...items) { arr.push(...items); return arr.length; }")
$js.Add("function pop(arr) { return arr.pop(); }")
$js.Add("function array_sum(arr) { if (!Array.isArray(arr)) throw new Error('array_sum(): arg 1 must be an array'); let sum = 0; for (const item of arr) { if (typeof item !== 'number' || !Number.isFinite(item)) throw new Error('array_sum(): array value must be a number, got ' + String(item)); sum += item; } return sum; }")
$js.Add("function array_avg(arr) { if (!Array.isArray(arr)) throw new Error('array_avg(): arg 1 must be an array'); return arr.length === 0 ? null : array_sum(arr) / arr.length; }")
$js.Add("function array_min(arr) { if (!Array.isArray(arr)) throw new Error('array_min(): arg 1 must be an array'); if (arr.length === 0) return null; let best = null; for (const item of arr) { if (typeof item !== 'number' || !Number.isFinite(item)) throw new Error('array_min(): array value must be a number, got ' + String(item)); best = best === null ? item : Math.min(best, item); } return best; }")
$js.Add("function array_max(arr) { if (!Array.isArray(arr)) throw new Error('array_max(): arg 1 must be an array'); if (arr.length === 0) return null; let best = null; for (const item of arr) { if (typeof item !== 'number' || !Number.isFinite(item)) throw new Error('array_max(): array value must be a number, got ' + String(item)); best = best === null ? item : Math.max(best, item); } return best; }")
$js.Add("function array_unique(arr) { if (!Array.isArray(arr)) throw new Error('array_unique(): arg 1 must be an array'); const out = []; for (const item of arr) if (!out.some(existing => __eq(existing, item))) out.push(item); return out; }")
$js.Add("function array_flatten(arr, depth = 1) { if (!Array.isArray(arr)) throw new Error('array_flatten(): arg 1 must be an array'); depth = Math.trunc(Number(depth)); if (!Number.isFinite(depth) || depth < 0) throw new Error('array_flatten(): arg 2 must be a non-negative integer'); return arr.flat(depth); }")
$js.Add("function array_range(start, end = undefined, step = undefined) { if (end === undefined) { end = Number(start); start = 0; } else { start = Number(start); end = Number(end); } step = step === undefined ? 1 : Number(step); if (!Number.isFinite(start) || !Number.isFinite(end) || !Number.isFinite(step)) throw new Error('array_range(): args must be finite numbers'); if (step === 0) throw new Error('array_range(): step must not be zero'); const out = []; if (step > 0) { for (let v = start; v < end; v += step) out.push(v); } else { for (let v = start; v > end; v += step) out.push(v); } return out; }")
$js.Add("function array_chunk(arr, size) { if (!Array.isArray(arr)) throw new Error('array_chunk(): arg 1 must be an array'); size = Math.trunc(Number(size)); if (!Number.isFinite(size) || size <= 0) throw new Error('array_chunk(): arg 2 must be a positive integer'); const out = []; for (let i = 0; i < arr.length; i += size) out.push(arr.slice(i, i + size)); return out; }")
$js.Add("function array_zip(...items) { if (!items.length) throw new Error('array_zip(): expected 1+ arg(s), got 0'); items.forEach((arr, i) => { if (!Array.isArray(arr)) throw new Error('array_zip(): arg ' + (i + 1) + ' must be an array'); }); const limit = Math.min(...items.map(arr => arr.length)); const out = []; for (let i = 0; i < limit; i++) out.push(items.map(arr => arr[i])); return out; }")
$js.Add("function array_repeat(value, count) { count = Math.trunc(Number(count)); if (!Number.isFinite(count) || count < 0) throw new Error('array_repeat(): arg 2 must be a non-negative integer'); return Array.from({length: count}, () => value); }")
$js.Add("function __sura_need_set_array(name, value, argIndex) { if (!Array.isArray(value)) throw new Error(name + '(): arg ' + argIndex + ' must be an array'); return value; }")
$js.Add("function __sura_array_has(arr, item) { return arr.some(existing => __eq(existing, item)); }")
$js.Add("function __sura_set_push(out, item) { if (!__sura_array_has(out, item)) out.push(item); }")
$js.Add("function set_union(...items) { if (!items.length) throw new Error('set_union(): expected 1+ arg(s), got 0'); const out = []; items.forEach((arr, i) => { arr = __sura_need_set_array('set_union', arr, i + 1); for (const item of arr) __sura_set_push(out, item); }); return out; }")
$js.Add("function set_intersection(...items) { if (!items.length) throw new Error('set_intersection(): expected 1+ arg(s), got 0'); const first = __sura_need_set_array('set_intersection', items[0], 1); const rest = items.slice(1).map((arr, i) => __sura_need_set_array('set_intersection', arr, i + 2)); const out = []; for (const item of first) if (rest.every(arr => __sura_array_has(arr, item))) __sura_set_push(out, item); return out; }")
$js.Add("function set_difference(...items) { if (!items.length) throw new Error('set_difference(): expected 1+ arg(s), got 0'); const first = __sura_need_set_array('set_difference', items[0], 1); const rest = items.slice(1).map((arr, i) => __sura_need_set_array('set_difference', arr, i + 2)); const out = []; for (const item of first) if (!rest.some(arr => __sura_array_has(arr, item))) __sura_set_push(out, item); return out; }")
$js.Add("function set_symmetric_difference(left, right) { left = __sura_need_set_array('set_symmetric_difference', left, 1); right = __sura_need_set_array('set_symmetric_difference', right, 2); const out = []; for (const item of left) if (!__sura_array_has(right, item)) __sura_set_push(out, item); for (const item of right) if (!__sura_array_has(left, item)) __sura_set_push(out, item); return out; }")
$js.Add("function set_is_subset(left, right) { left = __sura_need_set_array('set_is_subset', left, 1); right = __sura_need_set_array('set_is_subset', right, 2); return left.every(item => __sura_array_has(right, item)); }")
$js.Add("function set_is_superset(left, right) { left = __sura_need_set_array('set_is_superset', left, 1); right = __sura_need_set_array('set_is_superset', right, 2); return right.every(item => __sura_array_has(left, item)); }")
$js.Add("function __sura_require_fs(name) { if (!__sura_fs) throw new Error(name + '(): fs is unavailable'); return __sura_fs; }")
$js.Add("function __sura_mkdir_parent(path) { const fs = __sura_require_fs('file_write'); const s = String(path); const slash = Math.max(s.lastIndexOf('/'), s.lastIndexOf('\\')); if (slash > 0) fs.mkdirSync(s.slice(0, slash), {recursive: true}); }")
$js.Add("function file_read(path) { return __sura_require_fs('file_read').readFileSync(String(path), 'utf8'); }")
$js.Add("function file_write(path, text) { const fs = __sura_require_fs('file_write'); __sura_mkdir_parent(path); text = String(text); fs.writeFileSync(String(path), text, 'utf8'); return text.length; }")
$js.Add("function __sura_byte_array(name, bytes) { if (!Array.isArray(bytes)) throw new Error(name + '(): arg 2 must be an array'); return bytes.map((value, index) => { const n = Number(value); if (!Number.isInteger(n) || n < 0 || n > 255) throw new Error(name + '(): byte ' + (index + 1) + ' must be an integer from 0 to 255'); return n; }); }")
$js.Add("function file_read_bytes(path) { return Array.from(__sura_require_fs('file_read_bytes').readFileSync(String(path))); }")
$js.Add("function file_write_bytes(path, bytes) { const fs = __sura_require_fs('file_write_bytes'); const Buffer = __sura_require_buffer('file_write_bytes'); __sura_mkdir_parent(path); const data = __sura_byte_array('file_write_bytes', bytes); fs.writeFileSync(String(path), Buffer.from(data)); return data.length; }")
$js.Add("function file_read_json(path) { return json_parse(file_read(path)); }")
$js.Add("function file_write_json(path, value) { return file_write(path, json_stringify(value)); }")
$js.Add("function file_append(path, text) { const fs = __sura_require_fs('file_append'); __sura_mkdir_parent(path); text = String(text); fs.appendFileSync(String(path), text, 'utf8'); return text.length; }")
$js.Add("function file_exists(path) { return __sura_require_fs('file_exists').existsSync(String(path)); }")
$js.Add("function file_delete(path) { const fs = __sura_require_fs('file_delete'); path = String(path); if (!fs.existsSync(path)) return false; fs.unlinkSync(path); return true; }")
$js.Add('function __sura_remove_tree_count(fs, path) { if (!fs.existsSync(path)) return 0; const stat = fs.lstatSync(path); if (!stat.isDirectory()) return 1; let count = 1; for (const name of fs.readdirSync(path)) count += __sura_remove_tree_count(fs, __sura_require_path("file_remove_tree").join(path, name)); return count; }')
$js.Add('function file_remove_tree(path) { const fs = __sura_require_fs("file_remove_tree"); const p = __sura_require_path("file_remove_tree"); path = String(path); if (!path.trim()) throw new Error("file_remove_tree(): path must not be empty"); const normalized = p.normalize(path); if (normalized === "." || normalized === "..") throw new Error("file_remove_tree(): refusing to remove current or parent directory"); if (!fs.existsSync(path)) return 0; const abs = p.resolve(path); const root = p.parse(abs).root; if (abs === root) throw new Error("file_remove_tree(): refusing to remove filesystem root"); if (__sura_process && abs === p.resolve(__sura_process.cwd())) throw new Error("file_remove_tree(): refusing to remove current directory"); if (__sura_node_os && abs === p.resolve(__sura_node_os.tmpdir())) throw new Error("file_remove_tree(): refusing to remove temp directory root"); const count = __sura_remove_tree_count(fs, path); fs.rmSync(path, {recursive: true, force: false}); return count; }')
$js.Add("function file_lines(path) { let text = file_read(path).replace(/\r\n/g, '\n').replace(/\r/g, '\n'); if (text.endsWith('\n')) text = text.slice(0, -1); return text === '' ? [] : text.split('\n'); }")
$js.Add("const read_file = file_read, write_file = file_write, append_file = file_append, exists = file_exists, delete_file = file_delete, remove_tree = file_remove_tree;")
$js.Add('function file_list(path) { return __sura_require_fs("file_list").readdirSync(String(path)).sort(); }')
$js.Add('const list_dir = file_list;')
$js.Add('function file_walk(root, ext = "") { const fs = __sura_require_fs("file_walk"); const p = __sura_require_path("file_walk"); root = String(root); ext = String(ext || ""); if (ext && !ext.startsWith(".")) ext = "." + ext; const out = []; const walk = dir => { for (const name of fs.readdirSync(dir).sort()) { const full = p.join(dir, name); const stat = fs.statSync(full); if (stat.isDirectory()) walk(full); else if (stat.isFile() && (!ext || p.extname(full) === ext)) out.push(p.normalize(full).replace(/\\/g, "/")); } }; walk(root); return out.sort(); }')
$js.Add('const walk_files = file_walk;')
$js.Add('function __sura_glob_base(pattern) { const p = __sura_require_path("file_glob"); const wild = String(pattern).search(/[*?]/); if (wild < 0) { const dir = p.dirname(String(pattern)); return dir && dir !== "." ? dir : "."; } const prefix = String(pattern).slice(0, wild); const slash = Math.max(prefix.lastIndexOf("/"), prefix.lastIndexOf("\\")); return slash < 0 ? "." : prefix.slice(0, slash + 1); }')
$js.Add('function __sura_glob_regex(pattern) { pattern = String(pattern).replace(/\\/g, "/"); let out = "^"; const specials = "\\.^$|()[]{}+"; for (let i = 0; i < pattern.length;) { const ch = pattern[i]; if (ch === "*") { if (pattern[i + 1] === "*") { if (pattern[i + 2] === "/") { out += "(?:.*/)?"; i += 3; } else { out += ".*"; i += 2; } } else { out += "[^/]*"; i++; } } else if (ch === "?") { out += "[^/]"; i++; } else if (ch === "/") { out += "/"; i++; } else { out += specials.includes(ch) ? "\\" + ch : ch; i++; } } return new RegExp(out + "$"); }')
$js.Add('function file_glob(pattern) { const fs = __sura_require_fs("file_glob"); const p = __sura_require_path("file_glob"); pattern = String(pattern); if (!pattern) throw new Error("file_glob(): pattern must not be empty"); const out = []; if (!/[*?]/.test(pattern)) { if (fs.existsSync(pattern) && fs.statSync(pattern).isFile()) out.push(p.normalize(pattern).replace(/\\/g, "/")); return out.sort(); } const normalized = pattern.replace(/\\/g, "/"); const base = __sura_glob_base(pattern); if (!fs.existsSync(base) || !fs.statSync(base).isDirectory()) return []; const rx = __sura_glob_regex(normalized); const walk = dir => { for (const name of fs.readdirSync(dir).sort()) { const full = p.join(dir, name); const stat = fs.statSync(full); if (stat.isDirectory()) walk(full); else if (stat.isFile()) { const candidate = p.normalize(full).replace(/\\/g, "/"); if (rx.test(candidate)) out.push(candidate); } } }; walk(base); return out.sort(); }')
$js.Add('const glob_files = file_glob;')
$js.Add('function mkdir(path) { const fs = __sura_require_fs("mkdir"); path = String(path); const existed = fs.existsSync(path); fs.mkdirSync(path, {recursive: true}); return !existed; }')
$js.Add('function file_is_dir(path) { return __sura_require_fs("file_is_dir").statSync(String(path)).isDirectory(); }')
$js.Add('function file_is_file(path) { return __sura_require_fs("file_is_file").statSync(String(path)).isFile(); }')
$js.Add('const is_dir = file_is_dir, is_file = file_is_file;')
$js.Add('function file_size(path) { return __sura_require_fs("file_size").statSync(String(path)).size; }')
$js.Add('function file_info(path) { const fs = __sura_require_fs("file_info"); const p = __sura_require_path("file_info"); const raw = String(path); const exists = fs.existsSync(raw); const stat = exists ? fs.statSync(raw) : null; const abs = p.resolve(raw); return {path: raw, absolute: p.normalize(abs), name: p.basename(raw), dir: p.dirname(raw), ext: p.extname(raw), stem: p.basename(raw, p.extname(raw)), exists, is_file: !!(stat && stat.isFile()), is_dir: !!(stat && stat.isDirectory()), size: stat && stat.isFile() ? stat.size : null, modified: stat ? Math.floor(stat.mtimeMs / 1000) : null}; }')
$js.Add('function __sura_copy_parent(path) { const p = __sura_require_path("file_copy"); const dir = p.dirname(String(path)); if (dir && dir !== ".") __sura_require_fs("file_copy").mkdirSync(dir, {recursive: true}); }')
$js.Add('function file_copy(src, dst, overwrite = true) { const fs = __sura_require_fs("file_copy"); src = String(src); dst = String(dst); __sura_copy_parent(dst); if (!overwrite && fs.existsSync(dst)) return false; fs.copyFileSync(src, dst); return true; }')
$js.Add('function file_move(src, dst, overwrite = true) { const fs = __sura_require_fs("file_move"); src = String(src); dst = String(dst); __sura_copy_parent(dst); if (overwrite && fs.existsSync(dst)) fs.rmSync(dst, {force: true}); fs.renameSync(src, dst); return true; }')
$js.Add('const copy_file = file_copy, move_file = file_move;')
$js.Add("function log_set_file(path, append = true) { path = String(path); __sura_log_file = path; if (path && !append) file_write(path, ''); return true; }")
$js.Add("function log_set_json(enabled) { __sura_log_json = !!enabled; return __sura_log_json; }")
$js.Add("function __sura_log_normalize_level(level, name = 'log_set_level') { level = String(level).toUpperCase(); if (level === 'WARNING') level = 'WARN'; if (!['TRACE','DEBUG','INFO','WARN','ERROR','FATAL','OFF'].includes(level)) throw new Error(name + '(): level must be TRACE, DEBUG, INFO, WARN, ERROR, FATAL, or OFF'); return level; }")
$js.Add("function __sura_log_rank(level) { level = String(level).toUpperCase(); if (level === 'TRACE') return 0; if (level === 'DEBUG') return 10; if (level === 'INFO') return 20; if (level === 'WARN' || level === 'WARNING') return 30; if (level === 'ERROR') return 40; if (level === 'FATAL') return 50; if (level === 'OFF') return 1000000; return 20; }")
$js.Add("function log_set_level(level) { __sura_log_level = __sura_log_normalize_level(level); return __sura_log_level; }")
$js.Add("function log_get_level() { return __sura_log_level; }")
$js.Add("function log_level(level = undefined) { return level === undefined ? log_get_level() : log_set_level(level); }")
$js.Add("function __sura_log_line(level, message, fields = undefined) { level = String(level).toUpperCase(); message = String(message); if (__sura_log_json) { const record = {time: datetime_now(), level, message}; if (fields !== undefined) record.fields = fields; return JSON.stringify(record); } return '[' + level + ' ' + datetime_now() + '] ' + message; }")
$js.Add("function __sura_log_emit(level, message, fields = undefined) { level = String(level).toUpperCase(); if (__sura_log_rank(level) < __sura_log_rank(__sura_log_level)) return null; const line = __sura_log_line(level, message, fields); __sura_host_console.log(line); if (__sura_log_file) file_append(__sura_log_file, line + '\n'); return null; }")
$js.Add("function log_event(level, message, fields = undefined) { return __sura_log_emit(level, message, fields); }")
$js.Add("function log_debug(...items) { return __sura_log_emit('DEBUG', items.map(x => String(x)).join(' ')); }")
$js.Add("function log_info(...items) { return __sura_log_emit('INFO', items.map(x => String(x)).join(' ')); }")
$js.Add("function log_warn(...items) { return __sura_log_emit('WARN', items.map(x => String(x)).join(' ')); }")
$js.Add("function log_error(...items) { return __sura_log_emit('ERROR', items.map(x => String(x)).join(' ')); }")
$js.Add("const __sura_console_timers = new Map();")
$js.Add("const __sura_console_counts = new Map();")
$js.Add("const __sura_console_profiles = new Map();")
$js.Add("let __sura_console_group_depth = 0;")
$js.Add("const __sura_console_join = items => items.map(__sura_print_text).join(' ');")
$js.Add("const __sura_console_indent = () => '  '.repeat(__sura_console_group_depth);")
$js.Add("function __sura_console_emit(method, text) { const fn = __sura_host_console[method] || __sura_host_console.log; fn.call(__sura_host_console, __sura_console_indent() + String(text)); return null; }")
$js.Add("function console_log(...items) { return __sura_console_emit('log', __sura_console_join(items)); }")
$js.Add("const console_print = console_log;")
$js.Add("function console_write(...items) { const out = __sura_console_indent() + __sura_console_join(items); if (__sura_process && __sura_process.stdout && typeof __sura_process.stdout.write === 'function') __sura_process.stdout.write(out); else { const fn = __sura_host_console.log || (() => {}); fn.call(__sura_host_console, out); } return null; }")
$js.Add("function console_write_line(...items) { return console_log(...items); }")
$js.Add("const console_writeln = console_write_line;")
$js.Add("const console_println = console_write_line;")
$js.Add("const console_line = console_write_line;")
$js.Add("function console_info(...items) { return __sura_console_emit('info', __sura_console_join(items)); }")
$js.Add("function console_debug(...items) { return __sura_console_emit('debug', __sura_console_join(items)); }")
$js.Add("function console_warn(...items) { return __sura_console_emit('warn', __sura_console_join(items)); }")
$js.Add("function console_error(...items) { return __sura_console_emit('error', __sura_console_join(items)); }")
$js.Add("function console_exception(...items) { return console_error(...items); }")
$js.Add("function console_raw(...items) { return __sura_console_write_raw(items.map(__sura_print_text).join('')); }")
$js.Add("function console_flush() { return null; }")
$js.Add("function __sura_console_json_text(value, indent = 2) { indent = indent === undefined || indent === null ? 2 : Number(indent); if (!Number.isInteger(indent) || indent < 0 || indent > 16) throw new Error('console_json(): indent must be an integer from 0 to 16'); const text = indent === 0 ? JSON.stringify(value) : JSON.stringify(value, null, indent); return text === undefined ? 'null' : text; }")
$js.Add("function console_json(value, indent = 2) { console_log(__sura_console_json_text(value, indent)); return null; }")
$js.Add("function console_inspect(value, indent = 2) { if (Array.isArray(value) || (value && typeof value === 'object')) return __sura_console_json_text(value, indent); return __sura_print_text(value); }")
$js.Add("function console_hrtime() { if (__sura_process && __sura_process.hrtime && typeof __sura_process.hrtime.bigint === 'function') return Number(__sura_process.hrtime.bigint()) / 1000000; if (typeof performance !== 'undefined' && performance.now) return performance.now(); return Date.now(); }")
$js.Add("function console_beep() { return __sura_console_write_raw('\x07'); }")
$js.Add("function console_clear() { if (__sura_host_console.clear) __sura_host_console.clear(); return null; }")
$js.Add("function console_assert(condition, ...items) { if (!condition) console_error(items.length ? 'Assertion failed: ' + __sura_console_join(items) : 'Assertion failed'); return null; }")
$js.Add("function console_time(label = 'default') { __sura_console_timers.set(String(label), Date.now()); return null; }")
$js.Add("function console_time_log(label = 'default', ...items) { label = String(label); if (!__sura_console_timers.has(label)) { console_warn(""Timer '"" + label + ""' does not exist""); return null; } const elapsed = Date.now() - __sura_console_timers.get(label); console_log(label + ': ' + elapsed + ' ms' + (items.length ? ' ' + __sura_console_join(items) : '')); return elapsed; }")
$js.Add("function console_time_end(label = 'default') { label = String(label); if (!__sura_console_timers.has(label)) { console_warn(""Timer '"" + label + ""' does not exist""); return null; } const elapsed = Date.now() - __sura_console_timers.get(label); __sura_console_timers.delete(label); console_log(label + ': ' + elapsed + ' ms'); return elapsed; }")
$js.Add("function console_time_stamp(label = undefined) { const elapsed = Date.now(); console_log('Timestamp' + (label === undefined ? '' : ' ' + __sura_print_text(label)) + ': ' + elapsed + ' ms'); return elapsed; }")
$js.Add("function console_count(label = 'default') { label = String(label); const count = (__sura_console_counts.get(label) || 0) + 1; __sura_console_counts.set(label, count); console_log(label + ': ' + count); return count; }")
$js.Add("function console_count_reset(label = 'default') { __sura_console_counts.delete(String(label)); return null; }")
$js.Add("function __sura_console_cell(value) { return __sura_print_text(value).replace(/[\r\n\t]/g, ' '); }")
$js.Add("function __sura_console_token(text) { return String(text).toLowerCase().replace(/[_\-\s]/g, ''); }")
$js.Add("function __sura_console_style_code(raw) { const name = __sura_console_token(raw); const styles = {reset: 0, default: 0, normal: 0, bold: 1, dim: 2, faint: 2, italic: 3, underline: 4, blink: 5, inverse: 7, invert: 7, hidden: 8, strike: 9, strikethrough: 9}; return Object.prototype.hasOwnProperty.call(styles, name) ? styles[name] : null; }")
$js.Add("function __sura_console_color_code(raw, background, fn = 'console_color') { const name = __sura_console_token(raw); if (name === 'reset' || name === 'default' || name === 'none') return background ? 49 : 39; const base = background ? 40 : 30, bright = background ? 100 : 90; const colors = {black: 0, red: 1, green: 2, yellow: 3, blue: 4, magenta: 5, purple: 5, cyan: 6, white: 7}; if (Object.prototype.hasOwnProperty.call(colors, name)) return base + colors[name]; const brightColors = {gray: 0, grey: 0, brightblack: 0, brightred: 1, brightgreen: 2, brightyellow: 3, brightblue: 4, brightmagenta: 5, brightpurple: 5, brightcyan: 6, brightwhite: 7}; if (Object.prototype.hasOwnProperty.call(brightColors, name)) return bright + brightColors[name]; throw new Error(fn + '(): unknown ' + (background ? 'background' : 'foreground') + ' color ' + JSON.stringify(String(raw))); }")
$js.Add("function __sura_console_ansi(codes) { return codes.length ? String.fromCharCode(27) + '[' + codes.join(';') + 'm' : ''; }")
$js.Add("function __sura_console_style_codes(styles) { const values = Array.isArray(styles) ? styles : [styles]; const codes = []; for (const item of values) { if (typeof item !== 'string') throw new Error('console_style(): style array must contain strings'); let code = __sura_console_style_code(item); if (code === null) code = __sura_console_color_code(item, false, 'console_style'); codes.push(code); } return codes; }")
$js.Add("function console_style(text, styles) { return __sura_console_ansi(__sura_console_style_codes(styles)) + __sura_print_text(text) + String.fromCharCode(27) + '[0m'; }")
$js.Add("function console_color(text, fg, bg = undefined) { const codes = [__sura_console_color_code(fg, false, 'console_color')]; if (bg !== undefined && bg !== null) codes.push(__sura_console_color_code(bg, true, 'console_color')); return __sura_console_ansi(codes) + __sura_print_text(text) + String.fromCharCode(27) + '[0m'; }")
$js.Add("const console_colour = console_color;")
$js.Add("function console_strip_ansi(text) { return String(text).replace(/\x1B\[[0-9;?]*[ -/]*[@-~]/g, ''); }")
$js.Add("function __sura_console_write_raw(text) { if (__sura_process && __sura_process.stdout && typeof __sura_process.stdout.write === 'function') __sura_process.stdout.write(String(text)); else { const fn = __sura_host_console.log || (() => {}); fn.call(__sura_host_console, String(text)); } return null; }")
$js.Add("function console_set_color(fg, bg = undefined) { const codes = [__sura_console_color_code(fg, false, 'console_set_color')]; if (bg !== undefined && bg !== null) codes.push(__sura_console_color_code(bg, true, 'console_set_color')); return __sura_console_write_raw(__sura_console_ansi(codes)); }")
$js.Add("const console_set_colour = console_set_color;")
$js.Add("function console_reset_color() { return __sura_console_write_raw(String.fromCharCode(27) + '[0m'); }")
$js.Add("const console_reset_colour = console_reset_color;")
$js.Add("function console_is_tty() { return !!(__sura_process && __sura_process.stdout && __sura_process.stdout.isTTY); }")
$js.Add("function console_width() { return Number((__sura_process && __sura_process.stdout && __sura_process.stdout.columns) || 0); }")
$js.Add("function console_height() { return Number((__sura_process && __sura_process.stdout && __sura_process.stdout.rows) || 0); }")
$js.Add("function console_size() { return {width: console_width(), height: console_height(), is_tty: console_is_tty()}; }")
$js.Add("function console_status() { return {width: console_width(), height: console_height(), is_tty: console_is_tty(), group_depth: __sura_console_group_depth, timers: __sura_console_timers.size, counters: __sura_console_counts.size, profiles: __sura_console_profiles.size}; }")
$js.Add("function console_table(value) { let headers = [], rows = []; if (Array.isArray(value)) { const keys = Array.from(new Set(value.flatMap(x => x && typeof x === 'object' && !Array.isArray(x) ? Object.keys(x) : []))).sort(); headers = ['(index)'].concat(keys.length ? keys : ['value']); rows = value.map((item, i) => keys.length && item && typeof item === 'object' && !Array.isArray(item) ? [String(i)].concat(keys.map(k => Object.prototype.hasOwnProperty.call(item, k) ? __sura_console_cell(item[k]) : '')) : [String(i), __sura_console_cell(item)]); } else if (value && typeof value === 'object') { headers = ['key', 'value']; rows = Object.keys(value).sort().map(k => [k, __sura_console_cell(value[k])]); } else { headers = ['value']; rows = [[__sura_console_cell(value)]]; } const widths = headers.map((h, i) => Math.max(h.length, ...rows.map(r => String(r[i] ?? '').length))); const line = row => row.map((cell, i) => String(cell ?? '').padEnd(widths[i])).join(' | '); console_log(line(headers)); rows.forEach(row => console_log(line(row))); return null; }")
$js.Add("function console_dir(value, options = undefined) { console_log(__sura_console_cell(value)); return null; }")
$js.Add("function console_dirxml(...items) { return console_log(items.length === 1 ? __sura_console_cell(items[0]) : __sura_console_join(items)); }")
$js.Add("function console_trace(...items) { console_error(items.length ? 'Trace: ' + __sura_console_join(items) : 'Trace'); return null; }")
$js.Add("function console_group(...items) { if (items.length) console_log(...items); __sura_console_group_depth++; return null; }")
$js.Add("const console_group_collapsed = console_group;")
$js.Add("function console_group_end() { if (__sura_console_group_depth > 0) __sura_console_group_depth--; return null; }")
$js.Add("function console_profile(label = 'default') { label = String(label); __sura_console_profiles.set(label, Date.now()); console_log(""Profile '"" + label + ""' started""); return null; }")
$js.Add("function console_profile_end(label = 'default') { label = String(label); if (!__sura_console_profiles.has(label)) { console_warn(""Profile '"" + label + ""' does not exist""); return null; } const elapsed = Date.now() - __sura_console_profiles.get(label); __sura_console_profiles.delete(label); console_log(""Profile '"" + label + ""': "" + elapsed + ' ms'); return elapsed; }")
$js.Add("const console_input = input;")
$js.Add("const console_read_line = input;")
$js.Add("const console_readline = input;")
$js.Add("const console_readLine = input;")
$js.Add("const console_prompt = input;")
$js.Add("function __sura_query_scalar(value, name) { if (Array.isArray(value) || (value && typeof value === 'object')) throw new Error(name + '(): nested arrays or dicts are not supported as scalar values'); if (value === null || value === undefined) return ''; return String(value); }")
$js.Add("function query_build(params) { if (!params || typeof params !== 'object' || Array.isArray(params)) throw new Error('query_build(): expected dict'); const out = []; for (const key of Object.keys(params).sort()) { const value = params[key]; const values = Array.isArray(value) ? value : [value]; for (const item of values) out.push(encodeURIComponent(String(key)) + '=' + encodeURIComponent(__sura_query_scalar(item, 'query_build'))); } return out.join('&'); }")
$js.Add("function __sura_query_decode(text) { return decodeURIComponent(String(text).replace(/\+/g, ' ')); }")
$js.Add("function query_parse(query) { query = String(query); const question = query.indexOf('?'); if (question >= 0) query = query.slice(question + 1); const hash = query.indexOf('#'); if (hash >= 0) query = query.slice(0, hash); const out = {}; for (const pair of query.split('&')) { if (!pair) continue; const eq = pair.indexOf('='); const key = __sura_query_decode(eq < 0 ? pair : pair.slice(0, eq)); const value = __sura_query_decode(eq < 0 ? '' : pair.slice(eq + 1)); if (!Object.prototype.hasOwnProperty.call(out, key)) out[key] = value; else if (Array.isArray(out[key])) out[key].push(value); else out[key] = [out[key], value]; } return out; }")
$js.Add("function __sura_form_encode(text) { return encodeURIComponent(String(text)).replace(/%20/g, '+'); }")
$js.Add("function form_build(params) { if (!params || typeof params !== 'object' || Array.isArray(params)) throw new Error('form_build(): expected dict'); const out = []; for (const key of Object.keys(params).sort()) { const value = params[key]; const values = Array.isArray(value) ? value : [value]; for (const item of values) out.push(__sura_form_encode(key) + '=' + __sura_form_encode(__sura_query_scalar(item, 'form_build'))); } return out.join('&'); }")
$js.Add("function form_parse(body) { const out = {}; for (const pair of String(body).split('&')) { if (!pair) continue; const eq = pair.indexOf('='); const key = __sura_query_decode(eq < 0 ? pair : pair.slice(0, eq)); const value = __sura_query_decode(eq < 0 ? '' : pair.slice(eq + 1)); if (!Object.prototype.hasOwnProperty.call(out, key)) out[key] = value; else if (Array.isArray(out[key])) out[key].push(value); else out[key] = [out[key], value]; } return out; }")
$js.Add("function __sura_form_params_valid(params) { if (!params || typeof params !== 'object' || Array.isArray(params)) return false; for (const key of Object.keys(params)) { const value = params[key]; if (Array.isArray(value)) { for (const item of value) if (Array.isArray(item) || (item && typeof item === 'object')) return false; } else if (value && typeof value === 'object') return false; } return true; }")
$js.Add("function __sura_url_valid_scheme(scheme) { return /^[A-Za-z][A-Za-z0-9+.-]*$/.test(String(scheme)); }")
$js.Add("function url_parse(url) { const input = String(url); let rest = input; let scheme = '', authority = '', userinfo = '', host = '', portText = '', path = '', query = '', fragment = ''; let hasAuthority = false; const hash = rest.indexOf('#'); if (hash >= 0) { fragment = rest.slice(hash + 1); rest = rest.slice(0, hash); } const question = rest.indexOf('?'); if (question >= 0) { query = rest.slice(question + 1); rest = rest.slice(0, question); } const colon = rest.indexOf(':'); const slash = rest.indexOf('/'); if (colon >= 0 && (slash < 0 || colon < slash)) { const candidate = rest.slice(0, colon); if (__sura_url_valid_scheme(candidate)) { scheme = candidate; rest = rest.slice(colon + 1); } } if (rest.startsWith('//')) { hasAuthority = true; rest = rest.slice(2); const slash2 = rest.indexOf('/'); authority = slash2 < 0 ? rest : rest.slice(0, slash2); path = slash2 < 0 ? '' : rest.slice(slash2); let hostport = authority; const at = hostport.lastIndexOf('@'); if (at >= 0) { userinfo = hostport.slice(0, at); hostport = hostport.slice(at + 1); } if (hostport.startsWith('[')) { const close = hostport.indexOf(']'); if (close >= 0) { host = hostport.slice(0, close + 1); if (hostport[close + 1] === ':') portText = hostport.slice(close + 2); } else host = hostport; } else { const lastColon = hostport.lastIndexOf(':'); if (lastColon >= 0 && hostport.indexOf(':') === lastColon && /^[0-9]+$/.test(hostport.slice(lastColon + 1))) { host = hostport.slice(0, lastColon); portText = hostport.slice(lastColon + 1); } else host = hostport; } } else path = rest; if (portText !== '' && !/^[0-9]+$/.test(portText)) throw new Error('url_parse(): port must contain digits only'); const port = portText === '' ? null : Number(portText); if (port !== null && (port < 0 || port > 65535)) throw new Error('url_parse(): port must be from 0 to 65535'); const origin = scheme && host ? scheme + '://' + host + (portText ? ':' + portText : '') : ''; return {url: input, scheme, has_authority: hasAuthority, authority, userinfo, host, port, path, query, params: query_parse(query), fragment, origin}; }")
$js.Add("function __sura_url_part(parts, key, name) { const value = parts[key]; if (value === undefined || value === null) return ''; if (typeof value !== 'string') throw new Error(name + '(): ' + key + ' must be a string'); return value; }")
$js.Add("function __sura_url_port(parts, name) { const value = parts.port; if (value === undefined || value === null) return ''; if (typeof value === 'number') { if (!Number.isInteger(value) || value < 0 || value > 65535) throw new Error(name + '(): port must be an integer from 0 to 65535'); return String(value); } if (typeof value === 'string') { if (value && !/^[0-9]+$/.test(value)) throw new Error(name + '(): port string must contain digits only'); if (value && Number(value) > 65535) throw new Error(name + '(): port must be an integer from 0 to 65535'); return value; } throw new Error(name + '(): port must be a number or string'); }")
$js.Add("function url_build(parts) { if (!parts || typeof parts !== 'object' || Array.isArray(parts)) throw new Error('url_build(): expected dict'); let scheme = __sura_url_part(parts, 'scheme', 'url_build'); if (scheme.endsWith(':')) scheme = scheme.slice(0, -1); if (scheme && !__sura_url_valid_scheme(scheme)) throw new Error('url_build(): invalid scheme'); let authority = __sura_url_part(parts, 'authority', 'url_build'); if (!authority) { const host = __sura_url_part(parts, 'host', 'url_build'); if (host) { const userinfo = __sura_url_part(parts, 'userinfo', 'url_build'); authority = (userinfo ? userinfo + '@' : '') + host; const port = __sura_url_port(parts, 'url_build'); if (port) authority += ':' + port; } } let path = __sura_url_part(parts, 'path', 'url_build'); let out = ''; if (authority || parts.has_authority) { if (scheme) out += scheme + ':'; out += '//' + authority; if (path && !path.startsWith('/')) out += '/'; } else if (scheme) out += scheme + ':'; out += path; let query = __sura_url_part(parts, 'query', 'url_build'); if (query.startsWith('?')) query = query.slice(1); if (!query && parts.params !== undefined && parts.params !== null) { if (typeof parts.params !== 'object' || Array.isArray(parts.params)) throw new Error('url_build(): params must be a dict'); query = query_build(parts.params); } if (query) out += '?' + query; let fragment = __sura_url_part(parts, 'fragment', 'url_build'); if (fragment.startsWith('#')) fragment = fragment.slice(1); if (fragment) out += '#' + fragment; return out; }")
$js.Add("function auth_bearer(token) { return {Authorization: 'Bearer ' + String(token)}; }")
$js.Add("function auth_basic(username, password) { return {Authorization: 'Basic ' + base64_encode(String(username) + ':' + String(password))}; }")
$js.Add("function headers_merge(...headers) { const out = {}; for (const item of headers) { if (!item || typeof item !== 'object' || Array.isArray(item)) throw new Error('headers_merge(): expected header dictionaries'); for (const key of Object.keys(item)) out[key] = String(item[key]); } return out; }")
$js.Add("function __sura_header_key(name, fn) { const key = String(name); if (!/^[A-Za-z0-9_-]+$/.test(key)) throw new Error(fn + '(): header name contains unsupported characters'); return key.toLowerCase(); }")
$js.Add("function headers_get(headers, name, fallback = null) { if (!headers || typeof headers !== 'object' || Array.isArray(headers)) throw new Error('headers_get(): expected header dictionary'); const target = __sura_header_key(name, 'headers_get'); for (const key of Object.keys(headers)) if (key.toLowerCase() === target) return headers[key]; return fallback; }")
$js.Add("function headers_has(headers, name) { if (!headers || typeof headers !== 'object' || Array.isArray(headers)) throw new Error('headers_has(): expected header dictionary'); const target = __sura_header_key(name, 'headers_has'); for (const key of Object.keys(headers)) if (key.toLowerCase() === target) return true; return false; }")
$js.Add("function __sura_headers_redact_default(lower) { return lower === 'authorization' || lower === 'proxy-authorization' || lower === 'cookie' || lower === 'set-cookie' || lower === 'x-api-key' || lower === 'api-key' || lower === 'x-auth-token' || lower === 'x-csrf-token' || lower === 'x-xsrf-token' || lower.includes('token') || lower.includes('secret') || lower.includes('api-key') || lower.includes('apikey'); }")
$js.Add("function __sura_headers_redact_name(name) { const key = String(name).trim(); if (!key) return ''; if (!/^[A-Za-z0-9_-]+$/.test(key)) throw new Error('headers_redact(): header name contains unsupported characters'); return key.toLowerCase(); }")
$js.Add("function __sura_headers_redact_extra(names, lower) { if (names === undefined || names === null) return false; if (typeof names === 'string') return names.split(',').some(item => __sura_headers_redact_name(item) === lower); if (Array.isArray(names)) { for (const item of names) { if (typeof item !== 'string') throw new Error('headers_redact(): names array must contain strings'); if (__sura_headers_redact_name(item) === lower) return true; } return false; } if (typeof names === 'object') { for (const key of Object.keys(names)) if (names[key] && __sura_headers_redact_name(key) === lower) return true; return false; } throw new Error('headers_redact(): names must be nil, string, array, or dict'); }")
$js.Add("function headers_redact(headers, names = undefined, mask = '[REDACTED]') { if (!headers || typeof headers !== 'object' || Array.isArray(headers)) throw new Error('headers_redact(): expected header dictionary'); mask = String(mask); if (/[\r\n]/.test(mask)) throw new Error('headers_redact(): mask contains unsupported characters'); const out = {}; for (const key of Object.keys(headers)) { const lower = __sura_header_key(key, 'headers_redact'); out[key] = (__sura_headers_redact_default(lower) || __sura_headers_redact_extra(names, lower)) ? mask : String(headers[key]); } return out; }")
$js.Add("function __sura_cookie_name(name, fn) { name = String(name); if (!/^[!#$%&'*+.^_|~0-9A-Za-z-]+$/.test(name)) throw new Error(fn + '(): cookie name contains unsupported characters'); return name; }")
$js.Add("function __sura_cookie_unquote(value) { value = String(value).trim(); const quote = String.fromCharCode(34), slash = String.fromCharCode(92); if (value.length < 2 || value[0] !== quote || value[value.length - 1] !== quote) return value; let out = '', escaped = false; for (let i = 1; i + 1 < value.length; i++) { const ch = value[i]; if (escaped) { out += ch; escaped = false; } else if (ch === slash) escaped = true; else out += ch; } if (escaped) out += slash; return out; }")
$js.Add("function __sura_cookie_decode(value, fn) { try { return decodeURIComponent(value); } catch (err) { throw new Error(fn + '(): invalid percent escape'); } }")
$js.Add("function __sura_cookie_add(out, key, value) { if (!Object.prototype.hasOwnProperty.call(out, key)) out[key] = value; else if (Array.isArray(out[key])) out[key].push(value); else out[key] = [out[key], value]; }")
$js.Add("function cookie_parse(headerOrHeaders) { let text; if (typeof headerOrHeaders === 'string') text = headerOrHeaders; else if (headerOrHeaders && typeof headerOrHeaders === 'object' && !Array.isArray(headerOrHeaders)) { const header = headers_get(headerOrHeaders, 'cookie', undefined); if (header === undefined || header === null) return {}; text = String(header); } else throw new Error('cookie_parse(): expected cookie header text or header dictionary'); text = String(text).trim(); if (text.toLowerCase().startsWith('cookie:')) text = text.slice(7).trim(); const out = {}; for (let part of text.split(';')) { part = part.trim(); if (!part) continue; const eq = part.indexOf('='); if (eq < 0) continue; const key = part.slice(0, eq).trim(); if (!key || key[0] === '$') continue; __sura_cookie_name(key, 'cookie_parse'); const value = __sura_cookie_decode(__sura_cookie_unquote(part.slice(eq + 1)), 'cookie_parse'); __sura_cookie_add(out, key, value); } return out; }")
$js.Add("function cookie_build(cookies) { if (!cookies || typeof cookies !== 'object' || Array.isArray(cookies)) throw new Error('cookie_build(): expected cookie dictionary'); const parts = []; for (const key of Object.keys(cookies).sort()) { __sura_cookie_name(key, 'cookie_build'); const values = Array.isArray(cookies[key]) ? cookies[key] : [cookies[key]]; for (const value of values) { if (value && typeof value === 'object') throw new Error('cookie_build(): nested arrays or dicts are not supported as cookie values'); parts.push(key + '=' + encodeURIComponent(value === null || value === undefined ? '' : String(value))); } } return parts.join('; '); }")
$js.Add("function cookie_get(headerOrCookies, name, fallback = null) { name = __sura_cookie_name(name, 'cookie_get'); let cookies; if (typeof headerOrCookies === 'string') cookies = cookie_parse(headerOrCookies); else if (headerOrCookies && typeof headerOrCookies === 'object' && !Array.isArray(headerOrCookies)) { const header = headers_get(headerOrCookies, 'cookie', undefined); cookies = header === undefined || header === null ? headerOrCookies : cookie_parse(String(header)); } else throw new Error('cookie_get(): expected cookie header text, header dictionary, or cookie dictionary'); return Object.prototype.hasOwnProperty.call(cookies, name) ? cookies[name] : fallback; }")
$js.Add("function __sura_content_type_text(headersOrValue, fn) { if (typeof headersOrValue === 'string') return headersOrValue; if (headersOrValue && typeof headersOrValue === 'object' && !Array.isArray(headersOrValue)) return String(headers_get(headersOrValue, 'content-type', '')); throw new Error(fn + '(): expected header dict or content-type text'); }")
$js.Add("function http_content_type(headersOrValue, fallback = '') { const text = __sura_content_type_text(headersOrValue, 'http_content_type'); const media = String(text).split(';', 1)[0].trim().toLowerCase(); return media || String(fallback); }")
$js.Add("function http_charset(headersOrValue, fallback = '') { const text = __sura_content_type_text(headersOrValue, 'http_charset'); const parts = String(text).split(';').slice(1); const quote = String.fromCharCode(34); for (const part of parts) { const eq = part.indexOf('='); if (eq < 0) continue; const key = part.slice(0, eq).trim().toLowerCase(); if (key !== 'charset') continue; let value = part.slice(eq + 1).trim(); if (value.length >= 2 && value[0] === quote && value[value.length - 1] === quote) value = value.slice(1, -1); return value.toLowerCase(); } return String(fallback); }")
$js.Add("function http_is_json(headersOrValue) { const media = http_content_type(headersOrValue, ''); return media === 'application/json' || media.endsWith('+json'); }")
$js.Add("function __sura_http_status_code(status, name) { status = Number(status); if (!Number.isInteger(status) || status < 0 || status > 999) throw new Error(name + '(): status must be an integer from 0 to 999'); return status; }")
$js.Add("function http_status_ok(status) { status = __sura_http_status_code(status, 'http_status_ok'); return status >= 200 && status < 300; }")
$js.Add("function http_status_text(status) { status = __sura_http_status_code(status, 'http_status_text'); const names = {100:'Continue',101:'Switching Protocols',102:'Processing',103:'Early Hints',200:'OK',201:'Created',202:'Accepted',203:'Non-Authoritative Information',204:'No Content',205:'Reset Content',206:'Partial Content',207:'Multi-Status',208:'Already Reported',226:'IM Used',300:'Multiple Choices',301:'Moved Permanently',302:'Found',303:'See Other',304:'Not Modified',305:'Use Proxy',307:'Temporary Redirect',308:'Permanent Redirect',400:'Bad Request',401:'Unauthorized',402:'Payment Required',403:'Forbidden',404:'Not Found',405:'Method Not Allowed',406:'Not Acceptable',407:'Proxy Authentication Required',408:'Request Timeout',409:'Conflict',410:'Gone',411:'Length Required',412:'Precondition Failed',413:'Content Too Large',414:'URI Too Long',415:'Unsupported Media Type',416:'Range Not Satisfiable',417:'Expectation Failed',418:'I\'m a teapot',421:'Misdirected Request',422:'Unprocessable Content',423:'Locked',424:'Failed Dependency',425:'Too Early',426:'Upgrade Required',428:'Precondition Required',429:'Too Many Requests',431:'Request Header Fields Too Large',451:'Unavailable For Legal Reasons',500:'Internal Server Error',501:'Not Implemented',502:'Bad Gateway',503:'Service Unavailable',504:'Gateway Timeout',505:'HTTP Version Not Supported',506:'Variant Also Negotiates',507:'Insufficient Storage',508:'Loop Detected',510:'Not Extended',511:'Network Authentication Required'}; return names[status] || ''; }")
$js.Add("function http_status_retryable(status) { status = __sura_http_status_code(status, 'http_status_retryable'); return status === 408 || status === 409 || status === 425 || status === 429 || status === 500 || status === 502 || status === 503 || status === 504; }")
$js.Add("function http_retry_after(headersOrValue, defaultMs = 0) { defaultMs = __sura_int_arg('http_retry_after', defaultMs); if (defaultMs < 0) throw new Error('http_retry_after(): default_ms must be non-negative'); const parse = value => { if (value === undefined || value === null) return defaultMs; if (typeof value === 'number') { if (!Number.isInteger(value) || value < 0 || value > 2147483) return defaultMs; return value * 1000; } const text = String(value).trim(); if (!/^[0-9]+$/.test(text)) return defaultMs; const seconds = Number(text); return seconds > 2147483 ? defaultMs : seconds * 1000; }; if (typeof headersOrValue === 'string' || typeof headersOrValue === 'number') return parse(headersOrValue); if (!headersOrValue || typeof headersOrValue !== 'object' || Array.isArray(headersOrValue)) throw new Error('http_retry_after(): expected header dict or header value text'); for (const key of Object.keys(headersOrValue)) if (key.toLowerCase() === 'retry-after') return parse(headersOrValue[key]); return defaultMs; }")
$js.Add("function http_backoff_delays(attempts, baseMs = 250, factor = 2, maxMs = 60000) { attempts = __sura_int_arg('http_backoff_delays', attempts); baseMs = __sura_int_arg('http_backoff_delays', baseMs); factor = Number(factor); maxMs = __sura_int_arg('http_backoff_delays', maxMs); if (attempts < 1 || attempts > 50) throw new Error('http_backoff_delays(): attempts must be 1..50'); if (baseMs < 0 || maxMs < 0) throw new Error('http_backoff_delays(): delay values must be non-negative'); if (!Number.isFinite(factor) || factor < 1) throw new Error('http_backoff_delays(): factor must be >= 1'); const out = []; let delay = baseMs; for (let i = 0; i < attempts; i++) { out.push(Math.round(Math.min(delay, maxMs))); if (delay < maxMs) delay = Math.min(delay * factor, maxMs); } return out; }")
$js.Add("function __sura_require_child_process(name) { if (!__sura_child_process) throw new Error(name + '(): child_process is unavailable'); return __sura_child_process; }")
$js.Add("function __sura_require_node_os(name) { if (!__sura_node_os) throw new Error(name + '(): os is unavailable'); return __sura_node_os; }")
$js.Add("function __sura_http_file_path(url, name) { url = String(url); if (!url.startsWith('file://')) throw new Error(name + '(): URL must start with http://, https://, or file://'); return decodeURIComponent(url.slice(7)); }")
$js.Add("function __sura_http_apply_query(url, query, name) { url = String(url); if (query === undefined) return url; if (!query || typeof query !== 'object' || Array.isArray(query)) throw new Error(name + '(): query must be a dict'); const keys = Object.keys(query); if (!keys.length) return url; if (url.startsWith('file://')) throw new Error(name + '(): file:// URLs do not support query'); const qs = query_build(query); const hash = url.indexOf('#'); const base = hash >= 0 ? url.slice(0, hash) : url; const frag = hash >= 0 ? url.slice(hash) : ''; const sep = base.includes('?') ? (base.endsWith('?') || base.endsWith('&') ? '' : '&') : '?'; return base + sep + qs + frag; }")
$js.Add("function __sura_http_status(rawHeaders) { let status = 0; for (const line of String(rawHeaders || '').split(/\r?\n/)) { const m = line.match(/^HTTP\/\S+\s+(\d+)/i); if (m) status = Number(m[1]); } return status; }")
$js.Add("function __sura_http_headers(rawHeaders) { const headers = {}; for (const line of String(rawHeaders || '').split(/\r?\n/)) { const colon = line.indexOf(':'); if (colon <= 0) continue; const key = line.slice(0, colon).trim().toLowerCase(); const value = line.slice(colon + 1).trim(); if (!key) continue; headers[key] = Object.prototype.hasOwnProperty.call(headers, key) ? headers[key] + ', ' + value : value; } return headers; }")
$js.Add("function __sura_http_temp() { const fs = __sura_require_fs('http_request'); const p = __sura_require_path('http_request'); const os = __sura_require_node_os('http_request'); const dir = fs.mkdtempSync(p.join(os.tmpdir(), 'sura-js-http-')); return {dir, headers: p.join(dir, 'headers.txt'), body: p.join(dir, 'body.txt')}; }")
$js.Add("function __sura_http_spec(spec, name) { if (!spec || typeof spec !== 'object' || Array.isArray(spec)) throw new Error(name + '(): spec must be a dict'); if (typeof spec.url !== 'string') throw new Error(name + '(): spec.url must be a string'); if (/[\r\n]/.test(spec.url) || spec.url.includes(String.fromCharCode(34))) throw new Error(name + '(): URL contains unsupported characters'); const bodySources = (spec.body !== undefined ? 1 : 0) + (spec.json !== undefined ? 1 : 0) + (spec.form !== undefined ? 1 : 0); if (bodySources > 1) throw new Error(name + '(): accepts only one of body, json, or form'); const hasBody = bodySources > 0; const method = String(spec.method || (hasBody ? 'POST' : 'GET')).toUpperCase(); if (!/^[A-Z]+$/.test(method)) throw new Error(name + '(): method must contain letters only'); const url = __sura_http_apply_query(spec.url, spec.query, name); const isFile = url.startsWith('file://'); if (isFile && (hasBody || method !== 'GET')) throw new Error(name + '(): file:// only supports body-less GET'); if (!isFile && !url.startsWith('http://') && !url.startsWith('https://')) throw new Error(name + '(): URL must start with http://, https://, or file://'); let timeout = spec.timeout === undefined ? 20 : __sura_int_arg(name, spec.timeout); if (timeout <= 0 || timeout > 3600) throw new Error(name + '(): timeout must be 1..3600 seconds'); const headers = []; let hasContentType = false; if (spec.headers !== undefined) { if (!spec.headers || typeof spec.headers !== 'object' || Array.isArray(spec.headers)) throw new Error(name + '(): headers must be a dict'); for (const key of Object.keys(spec.headers)) { if (!/^[A-Za-z0-9_.-]+$/.test(key)) throw new Error(name + '(): header name contains unsupported characters'); const value = String(spec.headers[key]); if (/[\r\n]/.test(value)) throw new Error(name + '(): header value contains unsupported characters'); if (key.toLowerCase() === 'content-type') hasContentType = true; headers.push([key, value]); } } let body = ''; if (spec.json !== undefined) { body = JSON.stringify(spec.json); if (!hasContentType) headers.push(['Content-Type', 'application/json']); } else if (spec.form !== undefined) { body = form_build(spec.form); if (!hasContentType) { const ct = spec.content_type === undefined ? 'application/x-www-form-urlencoded' : String(spec.content_type); if (/[\r\n]/.test(ct)) throw new Error(name + '(): content_type contains unsupported characters'); headers.push(['Content-Type', ct]); } } else if (spec.body !== undefined) { body = typeof spec.body === 'string' ? spec.body : JSON.stringify(spec.body); if (!hasContentType) { const ct = spec.content_type === undefined ? 'application/json' : String(spec.content_type); if (/[\r\n]/.test(ct)) throw new Error(name + '(): content_type contains unsupported characters'); headers.push(['Content-Type', ct]); } } return {url, method, timeout, headers, hasBody, body, isFile}; }")
$js.Add("function http_get(url) { return http_request({url}); }")
$js.Add("function http_json(url) { return json_parse(http_get(url)); }")
$js.Add("function http_post(url, body, content_type = 'application/json') { return http_request({url, method: 'POST', body, content_type}); }")
$js.Add("function http_request(spec) { return http_request_full(spec).body; }")
$js.Add("function http_request_full(spec) { const plan = __sura_http_spec(spec, 'http_request_full'); if (plan.isFile) { const body = file_read(__sura_http_file_path(plan.url, 'http_request_full')); return {status: 200, ok: true, body, headers: {}, url: plan.url}; } const cp = __sura_require_child_process('http_request_full'); const fs = __sura_require_fs('http_request_full'); const tmp = __sura_http_temp(); try { const args = ['-L', '-sS', '-D', tmp.headers, '-o', tmp.body, '--max-time', String(plan.timeout), '-X', plan.method]; for (const pair of plan.headers) args.push('-H', pair[0] + ': ' + pair[1]); if (plan.hasBody) args.push('--data-binary', '@-'); args.push('--', plan.url); const result = cp.spawnSync('curl', args, {input: plan.hasBody ? plan.body : undefined, encoding: 'utf8', maxBuffer: 128 * 1024 * 1024}); if (result.error) throw new Error('http_request_full(): curl failed: ' + result.error.message); if (result.status !== 0) throw new Error('http_request_full(): curl exited ' + result.status + ': ' + String(result.stderr || '').trim()); const rawHeaders = fs.existsSync(tmp.headers) ? fs.readFileSync(tmp.headers, 'utf8') : ''; const body = fs.existsSync(tmp.body) ? fs.readFileSync(tmp.body, 'utf8') : ''; const status = __sura_http_status(rawHeaders); return {status, ok: status >= 200 && status < 300, body, headers: __sura_http_headers(rawHeaders), url: plan.url}; } finally { try { fs.rmSync(tmp.dir, {recursive: true, force: true}); } catch (_) {} } }")
$js.Add("function http_request_retry(spec, attempts = 3, delayMs = 250) { attempts = __sura_int_arg('http_request_retry', attempts); delayMs = __sura_int_arg('http_request_retry', delayMs); if (attempts < 1 || attempts > 20) throw new Error('http_request_retry(): attempts must be 1..20'); if (delayMs < 0 || delayMs > 60000) throw new Error('http_request_retry(): delay_ms must be 0..60000'); const response = http_request_full(spec); response.attempts = 1; return response; }")
$js.Add("function http_request_json(spec) { return json_parse(http_request(spec)); }")
$js.Add("function http_request_json_checked(spec) { const response = http_request_full(spec); if (!response.ok) throw new Error('http_request_json_checked(): HTTP status ' + response.status); return json_parse(response.body); }")
$js.Add("function http_request_retry_json(spec, attempts = 3, delayMs = 250) { return json_parse(http_request_retry(spec, attempts, delayMs).body); }")
$js.Add("function http_request_retry_json_checked(spec, attempts = 3, delayMs = 250) { const response = http_request_retry(spec, attempts, delayMs); if (!response.ok) throw new Error('http_request_retry_json_checked(): HTTP status ' + response.status); return json_parse(response.body); }")
$js.Add(@'
function __sura_http_server_port(name, port = 8000) {
  port = __sura_int_arg(name, port);
  if (port <= 0 || port > 65535) throw new Error(name + '(): port must be 1..65535');
  return port;
}
function __sura_http_server_temp(name) {
  const fs = __sura_require_fs(name);
  const p = __sura_require_path(name);
  const os = __sura_require_node_os(name);
  const dir = fs.mkdtempSync(p.join(os.tmpdir(), 'sura-js-http-server-'));
  return {dir, script: p.join(dir, 'server.js'), ready: p.join(dir, 'ready.txt'), routes: p.join(dir, 'routes.json')};
}
function __sura_http_server_spawn(name, temp, scriptText, args, port, type, extra = {}) {
  const fs = __sura_require_fs(name);
  const cp = __sura_require_child_process(name);
  if (!__sura_process || !__sura_process.execPath) throw new Error(name + '(): Node.js process path is unavailable');
  fs.writeFileSync(temp.script, scriptText, 'utf8');
  const child = cp.spawn(__sura_process.execPath, [temp.script, ...args.map(x => String(x)), String(port), temp.ready], {detached: true, stdio: 'ignore'});
  child.unref();
  for (let i = 0; i < 50 && !fs.existsSync(temp.ready); i++) sleep_ms(100);
  if (!fs.existsSync(temp.ready)) {
    try { if (child.pid) __sura_process.kill(child.pid); } catch (_) {}
    try { fs.rmSync(temp.dir, {recursive: true, force: true}); } catch (_) {}
    throw new Error(name + '(): failed to start node server');
  }
  const actualPort = Number(fs.readFileSync(temp.ready, 'utf8').trim()) || port;
  return Object.assign({type, pid: child.pid, port: actualPort, host: '127.0.0.1', url: 'http://127.0.0.1:' + actualPort + '/', runner: temp.script, temp_dir: temp.dir}, extra);
}
function __sura_http_static_server_script() {
  return String.raw`
const http = require('http');
const fs = require('fs');
const path = require('path');
const root = path.resolve(process.argv[2]);
const port = Number(process.argv[3] || '8000');
const ready = process.argv[4] || '';

function send(res, req, status, body, type) {
  res.writeHead(status, {'Content-Type': type || 'text/plain; charset=utf-8'});
  if (req.method === 'HEAD') return res.end();
  res.end(body);
}
function insideRoot(file) {
  const resolved = path.resolve(file);
  return resolved === root || resolved.startsWith(root + path.sep);
}
const server = http.createServer((req, res) => {
  if (req.method !== 'GET' && req.method !== 'HEAD') return send(res, req, 405, 'method not allowed');
  let pathname = '/';
  try {
    pathname = decodeURIComponent(new URL(req.url, 'http://127.0.0.1').pathname);
  } catch (_) {
    return send(res, req, 400, 'bad request');
  }
  let rel = pathname.replace(/^\/+/, '');
  if (!rel) rel = 'index.html';
  let file = path.resolve(root, rel);
  if (!insideRoot(file)) return send(res, req, 403, 'forbidden');
  fs.stat(file, (statErr, stat) => {
    if (statErr) return send(res, req, 404, 'not found');
    if (stat.isDirectory()) file = path.join(file, 'index.html');
    fs.readFile(file, (readErr, data) => {
      if (readErr) return send(res, req, 404, 'not found');
      res.writeHead(200, {'Content-Type': 'application/octet-stream'});
      if (req.method === 'HEAD') return res.end();
      res.end(data);
    });
  });
});
server.listen(port, '127.0.0.1', () => {
  if (ready) fs.writeFileSync(ready, String(server.address().port), 'utf8');
});
`;
}
function __sura_http_routes_server_script() {
  return String.raw`
const http = require('http');
const fs = require('fs');
const routesPath = process.argv[2];
const port = Number(process.argv[3] || '8000');
const ready = process.argv[4] || '';
const routes = JSON.parse(fs.readFileSync(routesPath, 'utf8'));

function send(res, req, status, body, headers) {
  const finalHeaders = Object.assign({'Content-Type': 'text/plain; charset=utf-8'}, headers || {});
  res.writeHead(status, finalHeaders);
  if (req.method === 'HEAD') return res.end();
  res.end(body);
}
function requestUrl(req) {
  try { return new URL(req.url, 'http://127.0.0.1'); } catch (_) { return null; }
}
function requestQuery(url) {
  const out = {};
  for (const [key, value] of url.searchParams.entries()) {
    if (Object.prototype.hasOwnProperty.call(out, key)) {
      if (!Array.isArray(out[key])) out[key] = [out[key]];
      out[key].push(value);
    } else {
      out[key] = value;
    }
  }
  return out;
}
function routeFor(method, pathname) {
  const upper = method.toUpperCase();
  return routes[upper + ' ' + pathname] ?? routes['* ' + pathname] ?? routes[pathname] ?? routes['*'];
}
function headersFrom(spec) {
  const headers = {};
  if (!spec || typeof spec !== 'object' || Array.isArray(spec) || !spec.headers) return headers;
  for (const [key, value] of Object.entries(spec.headers)) headers[key] = String(value);
  return headers;
}
function handle(req, res, route, url, body) {
  const pathname = decodeURIComponent(url.pathname);
  const spec = route && typeof route === 'object' && !Array.isArray(route) ? route : {body: String(route ?? '')};
  const status = Number(spec.status || 200);
  const headers = headersFrom(spec);
  let responseBody = '';
  if (spec.echo === true) {
    headers['Content-Type'] = headers['Content-Type'] || 'application/json; charset=utf-8';
    responseBody = JSON.stringify({method: req.method, path: pathname, query: requestQuery(url), headers: req.headers, body});
  } else if (Object.prototype.hasOwnProperty.call(spec, 'json')) {
    headers['Content-Type'] = headers['Content-Type'] || 'application/json; charset=utf-8';
    responseBody = JSON.stringify(spec.json);
  } else if (Object.prototype.hasOwnProperty.call(spec, 'body')) {
    responseBody = String(spec.body ?? '');
  } else {
    responseBody = JSON.stringify(spec);
  }
  send(res, req, status, responseBody, headers);
}
const server = http.createServer((req, res) => {
  const url = requestUrl(req);
  if (!url) return send(res, req, 400, 'bad request');
  let pathname = '/';
  try {
    pathname = decodeURIComponent(url.pathname);
  } catch (_) {
    return send(res, req, 400, 'bad request');
  }
  const route = routeFor(req.method, pathname);
  if (route === undefined) {
    return send(res, req, 404, JSON.stringify({error: 'not found', method: req.method, path: pathname}), {'Content-Type': 'application/json; charset=utf-8'});
  }
  let body = '';
  req.setEncoding('utf8');
  req.on('data', chunk => {
    body += chunk;
    if (body.length > 1048576) req.destroy();
  });
  req.on('end', () => handle(req, res, route, url, body));
});
server.listen(port, '127.0.0.1', () => {
  if (ready) fs.writeFileSync(ready, String(server.address().port), 'utf8');
});
`;
}
function http_serve_static(dir, port = 8000) {
  port = __sura_http_server_port('http_serve_static', port);
  const fs = __sura_require_fs('http_serve_static');
  const p = __sura_require_path('http_serve_static');
  const root = p.resolve(String(dir));
  if (!fs.existsSync(root) || !fs.statSync(root).isDirectory()) throw new Error('http_serve_static(): directory not found: ' + String(dir));
  const temp = __sura_http_server_temp('http_serve_static');
  return __sura_http_server_spawn('http_serve_static', temp, __sura_http_static_server_script(), [root], port, 'http_server', {directory: root});
}
function http_serve_routes(routes, port = 8000) {
  port = __sura_http_server_port('http_serve_routes', port);
  if (!routes || typeof routes !== 'object' || Array.isArray(routes)) throw new Error('http_serve_routes(): routes must be a dictionary');
  const fs = __sura_require_fs('http_serve_routes');
  const temp = __sura_http_server_temp('http_serve_routes');
  fs.writeFileSync(temp.routes, JSON.stringify(routes), 'utf8');
  return __sura_http_server_spawn('http_serve_routes', temp, __sura_http_routes_server_script(), [temp.routes], port, 'http_routes_server', {routes_file: temp.routes});
}
function http_server_url(server) {
  if (!server || typeof server !== 'object' || Array.isArray(server)) throw new Error('http_server_url(): expected server dict');
  if (typeof server.url !== 'string') throw new Error('http_server_url(): server has no url');
  return server.url;
}
function http_server_stop(server) {
  const fs = __sura_fs;
  let pid = 0;
  if (typeof server === 'number') pid = Math.trunc(server);
  else if (server && typeof server === 'object') pid = Math.trunc(Number(server.pid || 0));
  else throw new Error('http_server_stop(): expected server dict or pid');
  if (pid <= 0) return false;
  let stopped = true;
  try {
    if (!__sura_process || !__sura_process.kill) throw new Error('process.kill is unavailable');
    __sura_process.kill(pid);
  } catch (_) {
    stopped = false;
  }
  if (fs && server && typeof server === 'object' && typeof server.temp_dir === 'string') {
    try { fs.rmSync(server.temp_dir, {recursive: true, force: true}); } catch (_) {}
  }
  return stopped;
}
'@)
$js.Add("function __sura_native_only(name) { throw new Error(name + '(): native interop is only available in the Sura native runtime, not the JavaScript target'); }")
$js.Add("function python_available() { return false; }")
$js.Add("function python_executable() { return ''; }")
$js.Add("function python_eval(code) { return __sura_native_only('python_eval'); }")
$js.Add("function python_call(module, fn, args = [], kwargs = {}) { return __sura_native_only('python_call'); }")
$js.Add("function python_call_json(module, fn, args = [], kwargs = {}) { return __sura_native_only('python_call_json'); }")
$js.Add("function ffi_load(path) { return __sura_native_only('ffi_load'); }")
$js.Add("function ffi_call(lib, symbol, signature, ...args) { return __sura_native_only('ffi_call'); }")
$js.Add("function plugin_load(path) { return __sura_native_only('plugin_load'); }")
$js.Add("function plugin_load_manifest(path) { return __sura_native_only('plugin_load_manifest'); }")
$js.Add("function plugin_call(pluginHandle, exportName, ...args) { return __sura_native_only('plugin_call'); }")
$js.Add("function plugin_info(pluginHandle) { return __sura_native_only('plugin_info'); }")
$js.Add("function plugin_unload(pluginHandle) { return __sura_native_only('plugin_unload'); }")
$js.Add("let __sura_async_next_id = 1;")
$js.Add("const __sura_async_tasks = new Map();")
$js.Add("function __sura_async_store(output = '', delayMs = 0) { delayMs = Math.max(0, Math.trunc(Number(delayMs) || 0)); const id = __sura_async_next_id++; __sura_async_tasks.set(id, {id, output: output === null || output === undefined ? '' : String(output), readyAt: Date.now() + delayMs}); return id; }")
$js.Add("function cmd_run(command) { throw new Error('cmd_run(): JS target does not execute shell commands'); }")
$js.Add("function cmd_run_checked(command) { throw new Error('cmd_run_checked(): JS target does not execute shell commands'); }")
$js.Add("function async_cmd(command) { throw new Error('async_cmd(): JS target does not execute shell commands'); }")
$js.Add("const task = async_cmd;")
$js.Add("function async_http_get(url) { return __sura_async_store(http_get(url)); }")
$js.Add("function async_http_request(spec) { return __sura_async_store(http_request(spec)); }")
$js.Add("function async_sleep(ms) { ms = __sura_int_arg('async_sleep', ms); if (ms < 0) throw new Error('async_sleep(): duration must be non-negative'); return __sura_async_store('', ms); }")
$js.Add("function __sura_async_known(id) { id = __sura_int_arg('async', id); return [id, __sura_async_tasks.get(id) || null]; }")
$js.Add("function __sura_async_ready_task(task) { return !!task && Date.now() >= task.readyAt; }")
$js.Add("function async_status(id) { const pair = __sura_async_known(id); id = pair[0]; const task = pair[1]; const ready = __sura_async_ready_task(task); return {id, known: !!task, ready, running: !!task && !ready}; }")
$js.Add("function async_ready(id) { return async_status(id).ready; }")
$js.Add("function async_pending() { return Array.from(__sura_async_tasks.keys()).sort((a, b) => a - b).map(async_status); }")
$js.Add("function async_forget(id) { const status = async_status(id); if (!status.known || !status.ready) return false; __sura_async_tasks.delete(status.id); return true; }")
$js.Add("function async_cleanup() { let removed = 0; for (const id of Array.from(__sura_async_tasks.keys())) { if (async_ready(id)) { __sura_async_tasks.delete(id); removed++; } } return removed; }")
$js.Add("function async_await(id) { const pair = __sura_async_known(id); id = pair[0]; const task = pair[1]; if (!task) throw new Error('async_await(): unknown task id'); const remaining = Math.max(0, task.readyAt - Date.now()); if (remaining > 0) sleep_ms(remaining); __sura_async_tasks.delete(id); return task.output; }")
$js.Add("function async_await_timeout(id, milliseconds, fallback = null) { const pair = __sura_async_known(id); id = pair[0]; const task = pair[1]; if (!task) throw new Error('async_await_timeout(): unknown task id'); milliseconds = __sura_int_arg('async_await_timeout', milliseconds); if (milliseconds < 0) throw new Error('async_await_timeout(): timeout must be non-negative'); if (__sura_async_ready_task(task)) return async_await(id); if (milliseconds > 0) sleep_ms(milliseconds); return __sura_async_ready_task(task) ? async_await(id) : fallback; }")
$js.Add("function async_ready_all(ids) { if (!Array.isArray(ids)) throw new Error('async_ready_all(): expected task id array'); return ids.every(id => async_ready(id)); }")
$js.Add("function async_any(ids, milliseconds = undefined, fallback = null) { if (!Array.isArray(ids) || ids.length === 0) throw new Error('async_any(): task id array must not be empty'); const hasTimeout = milliseconds !== undefined; if (hasTimeout) { milliseconds = __sura_int_arg('async_any', milliseconds); if (milliseconds < 0) throw new Error('async_any(): timeout must be non-negative'); } const deadline = hasTimeout ? Date.now() + milliseconds : null; while (true) { for (let index = 0; index < ids.length; index++) { const id = __sura_int_arg('async_any', ids[index]); const task = __sura_async_tasks.get(id); if (!task) throw new Error('async_any(): unknown task id'); if (__sura_async_ready_task(task)) { __sura_async_tasks.delete(id); return {id, index, output: task.output}; } } if (hasTimeout && (milliseconds === 0 || Date.now() >= deadline)) return fallback; sleep_ms(hasTimeout ? Math.min(10, Math.max(0, deadline - Date.now())) : 10); } }")
$js.Add("function async_all(ids) { if (!Array.isArray(ids)) throw new Error('async_all(): expected task id array'); return ids.map(id => async_await(id)); }")
$js.Add("function async_all_timeout(ids, milliseconds, fallback = null) { if (!Array.isArray(ids)) throw new Error('async_all_timeout(): expected task id array'); milliseconds = __sura_int_arg('async_all_timeout', milliseconds); if (milliseconds < 0) throw new Error('async_all_timeout(): timeout must be non-negative'); if (!async_ready_all(ids) && milliseconds > 0) sleep_ms(milliseconds); return async_ready_all(ids) ? async_all(ids) : fallback; }")
$js.Add("const await_timeout = async_await_timeout, await_any = async_any, await_all = async_all, await_all_timeout = async_all_timeout;")
$js.Add("function __sura_db_read_dict(path) { if (!file_exists(path)) return {}; const text = file_read(path); if (String(text).trim() === '') return {}; const parsed = json_parse(text); return parsed && typeof parsed === 'object' && !Array.isArray(parsed) ? parsed : {}; }")
$js.Add("function __sura_db_write(path, value) { const text = json_stringify(value); file_write(path, text); return text.length; }")
$js.Add("function db_set(path, key, value) { const data = __sura_db_read_dict(path); data[String(key)] = value; return __sura_db_write(path, data); }")
$js.Add("function db_get(path, key, fallback = null) { const data = __sura_db_read_dict(path); key = String(key); return Object.prototype.hasOwnProperty.call(data, key) ? data[key] : fallback; }")
$js.Add("function db_has(path, key) { return Object.prototype.hasOwnProperty.call(__sura_db_read_dict(path), String(key)); }")
$js.Add("function db_delete(path, key) { const data = __sura_db_read_dict(path); key = String(key); if (!Object.prototype.hasOwnProperty.call(data, key)) return false; delete data[key]; __sura_db_write(path, data); return true; }")
$js.Add("function db_keys(path) { return Object.keys(__sura_db_read_dict(path)).sort(); }")
$js.Add("function db_all(path) { return __sura_db_read_dict(path); }")
$js.Add("function __sura_db_read_rows(path, name) { if (!file_exists(path)) return []; const text = file_read(path); if (String(text).trim() === '') return []; const parsed = json_parse(text); if (Array.isArray(parsed)) return parsed; if (parsed && typeof parsed === 'object' && Array.isArray(parsed.rows)) return parsed.rows; throw new Error(name + '(): database file must contain an array or {rows: [...]}'); }")
$js.Add("function __sura_db_need_dict(name, value) { if (!value || typeof value !== 'object' || Array.isArray(value)) throw new Error(name + '(): argument must be a dict'); return value; }")
$js.Add("function __sura_db_row_matches(row, criteria) { if (!row || typeof row !== 'object' || Array.isArray(row)) return false; for (const key of Object.keys(criteria)) { if (!Object.prototype.hasOwnProperty.call(row, key) || !__sura_equal(row[key], criteria[key])) return false; } return true; }")
$js.Add("function db_insert(path, row) { row = __sura_db_need_dict('db_insert', row); const rows = __sura_db_read_rows(path, 'db_insert'); rows.push(row); __sura_db_write(path, rows); return rows.length; }")
$js.Add("function db_find(path, criteria) { criteria = __sura_db_need_dict('db_find', criteria); return __sura_db_read_rows(path, 'db_find').filter(row => __sura_db_row_matches(row, criteria)); }")
$js.Add("function db_count(path, criteria = undefined) { const rows = __sura_db_read_rows(path, 'db_count'); if (criteria === undefined) return rows.length; criteria = __sura_db_need_dict('db_count', criteria); return rows.filter(row => __sura_db_row_matches(row, criteria)).length; }")
$js.Add("function db_update(path, criteria, patch) { criteria = __sura_db_need_dict('db_update', criteria); patch = __sura_db_need_dict('db_update', patch); const rows = __sura_db_read_rows(path, 'db_update'); let count = 0; for (const row of rows) { if (!__sura_db_row_matches(row, criteria)) continue; Object.assign(row, patch); count++; } if (count > 0) __sura_db_write(path, rows); return count; }")
$js.Add("function db_remove(path, criteria) { criteria = __sura_db_need_dict('db_remove', criteria); const rows = __sura_db_read_rows(path, 'db_remove'); const kept = []; let removed = 0; for (const row of rows) { if (__sura_db_row_matches(row, criteria)) removed++; else kept.push(row); } if (removed > 0) __sura_db_write(path, kept); return removed; }")
$js.Add("function __sura_db_nonneg_option(options, key, fallback) { if (!Object.prototype.hasOwnProperty.call(options, key) || options[key] == null) return fallback; const value = Number(options[key]); if (!Number.isFinite(value) || value < 0 || Math.trunc(value) !== value) throw new Error('db_query(): option ' + key + ' must be a non-negative integer'); return value; }")
$js.Add("function db_query(path, criteria = {}, options = {}) { criteria = criteria == null ? {} : __sura_db_need_dict('db_query', criteria); options = options == null ? {} : __sura_db_need_dict('db_query', options); let rows = __sura_db_read_rows(path, 'db_query').filter(row => __sura_db_row_matches(row, criteria)); const sortPath = options.sort_by ?? options.sort ?? ''; if (sortPath !== '') { if (typeof sortPath !== 'string') throw new Error('db_query(): sort option must be a string'); const desc = !!(options.desc ?? options.descending); rows = rows.slice().sort((a, b) => { const av = __sura_path_get(a, sortPath, null); const bv = __sura_path_get(b, sortPath, null); if (__sura_equal(av, bv)) return 0; const less = desc ? __sura_collection_less(bv, av) : __sura_collection_less(av, bv); return less ? -1 : 1; }); } const offset = __sura_db_nonneg_option(options, 'offset', 0); const limit = __sura_db_nonneg_option(options, 'limit', Math.max(0, rows.length - offset)); return rows.slice(offset, offset + limit); }")
$js.Add("function __sura_num_array(name, arr) { if (!Array.isArray(arr)) throw new Error(name + '(): expected array'); return arr.map(x => { const n = Number(x); if (!Number.isFinite(n)) throw new Error(name + '(): vector values must be numbers'); return n; }); }")
$js.Add("function __sura_vector_pair(name, a, b) { a = __sura_num_array(name, a); b = __sura_num_array(name, b); if (a.length !== b.length) throw new Error(name + '(): vector lengths differ'); return [a, b]; }")
$js.Add("function vector_add(a, b) { [a, b] = __sura_vector_pair('vector_add', a, b); return a.map((x, i) => x + b[i]); }")
$js.Add("function vector_dot(a, b) { [a, b] = __sura_vector_pair('vector_dot', a, b); return a.reduce((sum, x, i) => sum + x * b[i], 0); }")
$js.Add("function vector_scale(arr, scalar) { arr = __sura_num_array('vector_scale', arr); scalar = Number(scalar); if (!Number.isFinite(scalar)) throw new Error('vector_scale(): scalar must be a number'); return arr.map(x => x * scalar); }")
$js.Add("function vector_norm(arr) { arr = __sura_num_array('vector_norm', arr); return Math.sqrt(arr.reduce((sum, x) => sum + x * x, 0)); }")
$js.Add("function vector_cosine(a, b) { const dot = vector_dot(a, b); const denom = vector_norm(a) * vector_norm(b); return denom === 0 ? 0 : dot / denom; }")
$js.Add("function vector_normalize(arr) { arr = __sura_num_array('vector_normalize', arr); const norm = vector_norm(arr); return norm === 0 ? arr.map(_ => 0) : arr.map(x => x / norm); }")
$js.Add("function vec3(x, y, z) { return [Number(x), Number(y), Number(z)]; }")
$js.Add("function __sura_vec3(name, value) { value = __sura_num_array(name, value); if (value.length !== 3) throw new Error(name + '(): expected 3D vector'); return value; }")
$js.Add("function vec3_add(a, b) { a = __sura_vec3('vec3_add', a); b = __sura_vec3('vec3_add', b); return [a[0] + b[0], a[1] + b[1], a[2] + b[2]]; }")
$js.Add("function vec3_sub(a, b) { a = __sura_vec3('vec3_sub', a); b = __sura_vec3('vec3_sub', b); return [a[0] - b[0], a[1] - b[1], a[2] - b[2]]; }")
$js.Add("function vec3_dot(a, b) { a = __sura_vec3('vec3_dot', a); b = __sura_vec3('vec3_dot', b); return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }")
$js.Add("function vec3_cross(a, b) { a = __sura_vec3('vec3_cross', a); b = __sura_vec3('vec3_cross', b); return [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]; }")
$js.Add("function vec3_scale(v, scalar) { v = __sura_vec3('vec3_scale', v); scalar = Number(scalar); if (!Number.isFinite(scalar)) throw new Error('vec3_scale(): scalar must be a number'); return [v[0] * scalar, v[1] * scalar, v[2] * scalar]; }")
$js.Add("function vec3_norm(v) { v = __sura_vec3('vec3_norm', v); return Math.sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }")
$js.Add("function vec3_normalize(v) { v = __sura_vec3('vec3_normalize', v); const norm = vec3_norm(v); return norm === 0 ? [0, 0, 0] : [v[0] / norm, v[1] / norm, v[2] / norm]; }")
$js.Add("function vec3_distance(a, b) { return vec3_norm(vec3_sub(a, b)); }")
$js.Add("function vec3_neg(v) { v = __sura_vec3('vec3_neg', v); return [-v[0], -v[1], -v[2]]; }")
$js.Add("function vec3_lerp(a, b, t) { a = __sura_vec3('vec3_lerp', a); b = __sura_vec3('vec3_lerp', b); t = Number(t); if (!Number.isFinite(t)) throw new Error('vec3_lerp(): t must be a number'); return [a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t]; }")
$js.Add("function vec3_midpoint(a, b) { a = __sura_vec3('vec3_midpoint', a); b = __sura_vec3('vec3_midpoint', b); return [(a[0] + b[0]) * 0.5, (a[1] + b[1]) * 0.5, (a[2] + b[2]) * 0.5]; }")
$js.Add("function vec3_project(v, onto) { v = __sura_vec3('vec3_project', v); onto = __sura_vec3('vec3_project', onto); const denom = vec3_dot(onto, onto); if (denom === 0) return [0, 0, 0]; const k = vec3_dot(v, onto) / denom; return vec3_scale(onto, k); }")
$js.Add("function vec3_reject(v, onto) { v = __sura_vec3('vec3_reject', v); onto = __sura_vec3('vec3_reject', onto); const p = vec3_project(v, onto); return [v[0] - p[0], v[1] - p[1], v[2] - p[2]]; }")
$js.Add("function vec3_reflect(v, normal) { v = __sura_vec3('vec3_reflect', v); normal = __sura_vec3('vec3_reflect', normal); const denom = vec3_dot(normal, normal); if (denom === 0) return v.slice(); const k = 2 * vec3_dot(v, normal) / denom; return [v[0] - normal[0] * k, v[1] - normal[1] * k, v[2] - normal[2] * k]; }")
$js.Add("function vec3_angle(a, b) { a = __sura_vec3('vec3_angle', a); b = __sura_vec3('vec3_angle', b); const denom = vec3_norm(a) * vec3_norm(b); if (denom === 0) return 0; const c = Math.max(-1, Math.min(1, vec3_dot(a, b) / denom)); return Math.acos(c); }")
$js.Add("function __sura_mat4(name, matrix) { if (!Array.isArray(matrix)) throw new Error(name + '(): expected 4x4 matrix'); if (matrix.length === 16) return matrix.map(Number); if (matrix.length === 4 && matrix.every(row => Array.isArray(row) && row.length === 4)) return matrix.flat().map(Number); throw new Error(name + '(): expected flat 16-value matrix or 4x4 nested matrix'); }")
$js.Add("function __sura_vec3_transform4_matrix(name, v, m) { v = __sura_vec3(name, v); let x = m[0] * v[0] + m[1] * v[1] + m[2] * v[2] + m[3]; let y = m[4] * v[0] + m[5] * v[1] + m[6] * v[2] + m[7]; let z = m[8] * v[0] + m[9] * v[1] + m[10] * v[2] + m[11]; const w = m[12] * v[0] + m[13] * v[1] + m[14] * v[2] + m[15]; if (Math.abs(w) > 1e-12 && Math.abs(w - 1) > 1e-12) { x /= w; y /= w; z /= w; } return [x, y, z]; }")
$js.Add("function vec3_transform4(v, matrix) { return __sura_vec3_transform4_matrix('vec3_transform4', v, __sura_mat4('vec3_transform4', matrix)); }")
$js.Add("function mat4_identity() { return [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]; }")
$js.Add("function mat4_translate(x, y, z) { return [1, 0, 0, Number(x), 0, 1, 0, Number(y), 0, 0, 1, Number(z), 0, 0, 0, 1]; }")
$js.Add("function mat4_scale(x, y, z) { return [Number(x), 0, 0, 0, 0, Number(y), 0, 0, 0, 0, Number(z), 0, 0, 0, 0, 1]; }")
$js.Add("function mat4_rotate_y(radians) { radians = Number(radians); const c = Math.cos(radians), s = Math.sin(radians); return [c, 0, s, 0, 0, 1, 0, 0, -s, 0, c, 0, 0, 0, 0, 1]; }")
$js.Add("function mat4_mul(a, b) { a = __sura_mat4('mat4_mul', a); b = __sura_mat4('mat4_mul', b); const out = new Array(16).fill(0); for (let row = 0; row < 4; row++) for (let col = 0; col < 4; col++) for (let k = 0; k < 4; k++) out[row * 4 + col] += a[row * 4 + k] * b[k * 4 + col]; return out; }")
$js.Add("function mesh_cube(size = 1, center = [0, 0, 0]) { size = Number(size); if (!Number.isFinite(size) || size <= 0) throw new Error('mesh_cube(): size must be a positive finite number'); const c = __sura_vec3('mesh_cube', center); const h = size * 0.5; const vertices = [[c[0]-h,c[1]-h,c[2]-h],[c[0]+h,c[1]-h,c[2]-h],[c[0]+h,c[1]+h,c[2]-h],[c[0]-h,c[1]+h,c[2]-h],[c[0]-h,c[1]-h,c[2]+h],[c[0]+h,c[1]-h,c[2]+h],[c[0]+h,c[1]+h,c[2]+h],[c[0]-h,c[1]+h,c[2]+h]]; const faces = [[0,2,1],[0,3,2],[4,5,6],[4,6,7],[0,1,5],[0,5,4],[3,6,2],[3,7,6],[1,2,6],[1,6,5],[0,4,7],[0,7,3]]; const edges = [[0,1],[1,2],[2,3],[3,0],[4,5],[5,6],[6,7],[7,4],[0,4],[1,5],[2,6],[3,7]]; return {kind: 'mesh', vertices, faces, edges}; }")
$js.Add("function __sura_mesh_vertices(name, mesh) { if (!mesh || typeof mesh !== 'object' || Array.isArray(mesh) || !Array.isArray(mesh.vertices)) throw new Error(name + '(): mesh must contain a vertices array'); return mesh.vertices; }")
$js.Add("function mesh_transform4(mesh, matrix) { const m = __sura_mat4('mesh_transform4', matrix); const vertices = __sura_mesh_vertices('mesh_transform4', mesh).map(v => __sura_vec3_transform4_matrix('mesh_transform4', v, m)); return {kind: 'mesh', vertices, faces: Array.isArray(mesh.faces) ? mesh.faces.map(face => face.slice()) : [], edges: Array.isArray(mesh.edges) ? mesh.edges.map(edge => edge.slice()) : []}; }")
$js.Add("function mesh_bounds(mesh) { const vertices = __sura_mesh_vertices('mesh_bounds', mesh); if (!vertices.length) throw new Error('mesh_bounds(): mesh must contain at least one vertex'); let min = __sura_vec3('mesh_bounds', vertices[0]).slice(), max = min.slice(); for (let i = 1; i < vertices.length; i++) { const v = __sura_vec3('mesh_bounds', vertices[i]); for (let axis = 0; axis < 3; axis++) { min[axis] = Math.min(min[axis], v[axis]); max[axis] = Math.max(max[axis], v[axis]); } } return {min, max, size: [max[0]-min[0], max[1]-min[1], max[2]-min[2]], center: [(min[0]+max[0])*0.5, (min[1]+max[1])*0.5, (min[2]+max[2])*0.5]}; }")
$js.Add("function mesh_face_normals(mesh) { const vertices = __sura_mesh_vertices('mesh_face_normals', mesh); if (!Array.isArray(mesh.faces)) throw new Error('mesh_face_normals(): mesh must contain a faces array'); return mesh.faces.map(face => { if (!Array.isArray(face) || face.length < 3) throw new Error('mesh_face_normals(): each face needs at least 3 indices'); const a = __sura_vec3('mesh_face_normals', vertices[face[0]]), b = __sura_vec3('mesh_face_normals', vertices[face[1]]), c = __sura_vec3('mesh_face_normals', vertices[face[2]]); const ux = b[0] - a[0], uy = b[1] - a[1], uz = b[2] - a[2]; const vx = c[0] - a[0], vy = c[1] - a[1], vz = c[2] - a[2]; const nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx; const len = Math.hypot(nx, ny, nz); return len === 0 ? [0, 0, 0] : [nx / len, ny / len, nz / len]; }); }")
$js.Add("function camera_project(point, camera, width = 1, height = 1) { point = __sura_vec3('camera_project', point); if (!camera || typeof camera !== 'object' || Array.isArray(camera)) throw new Error('camera_project(): arg 2 must be a camera dict'); width = Number(width); height = Number(height); if (width <= 0 || height <= 0) throw new Error('camera_project(): viewport must be positive'); const pos = __sura_vec3('camera_project', camera.position || [0, 0, -1]); const target = __sura_vec3('camera_project', camera.target || [0, 0, 0]); const up = __sura_vec3('camera_project', camera.up || [0, 1, 0]); const fov = Number(camera.fov_deg ?? 60); const aspect = Number(camera.aspect ?? (width / height)); const near = Number(camera.near ?? 0.001); if (fov <= 0 || fov >= 179 || aspect <= 0 || near <= 0) throw new Error('camera_project(): invalid camera fov/aspect/near'); const forward = vec3_normalize(vec3_sub(target, pos)); let right = vec3_cross(forward, up); if (vec3_norm(right) === 0) right = [1, 0, 0]; right = vec3_normalize(right); const trueUp = vec3_cross(right, forward); const rel = vec3_sub(point, pos); const depth = vec3_dot(rel, forward); let ndcX = 0, ndcY = 0; const visible = depth >= near; if (visible) { const f = 1 / Math.tan((fov * Math.PI / 180) * 0.5); ndcX = (vec3_dot(rel, right) * f) / (depth * aspect); ndcY = (vec3_dot(rel, trueUp) * f) / depth; } return {x: (ndcX + 1) * 0.5 * width, y: (1 - ndcY) * 0.5 * height, depth, visible, ndc: [ndcX, ndcY, depth]}; }")
$js.Add("function __sura_embedding_for_item(item, field, index) { if (Array.isArray(item)) return item; if (item && typeof item === 'object') { if (!Object.prototype.hasOwnProperty.call(item, field)) throw new Error('vector_search(): item ' + index + ' missing ' + JSON.stringify(field) + ' embedding'); if (!Array.isArray(item[field])) throw new Error('vector_search(): item ' + index + ' field ' + JSON.stringify(field) + ' must be an array'); return item[field]; } throw new Error('vector_search(): item ' + index + ' must be an embedding array or dict'); }")
$js.Add("function vector_search(query, rows, k = undefined, field = 'embedding') { query = __sura_num_array('vector_search', query); if (!Array.isArray(rows)) throw new Error('vector_search(): rows must be an array'); k = k === undefined ? rows.length : Math.trunc(Number(k)); if (k < 0) throw new Error('vector_search(): k must be non-negative'); field = String(field); if (!field) throw new Error('vector_search(): field must not be empty'); return rows.map((item, index) => ({index, score: vector_cosine(query, __sura_embedding_for_item(item, field, index)), item})).sort((a, b) => b.score - a.score).slice(0, k); }")
$js.Add("const vector3 = vec3, vector3_add = vec3_add, vector3_sub = vec3_sub, vector3_dot = vec3_dot, vector3_cross = vec3_cross, vector3_scale = vec3_scale, vector3_norm = vec3_norm, vector3_normalize = vec3_normalize, vector3_distance = vec3_distance, vector3_neg = vec3_neg, vector3_lerp = vec3_lerp, vector3_midpoint = vec3_midpoint, vector3_project = vec3_project, vector3_reject = vec3_reject, vector3_reflect = vec3_reflect, vector3_angle = vec3_angle, vector3_transform4 = vec3_transform4;")
$js.Add("const vec_add = vector_add, vec_dot = vector_dot, vec_scale = vector_scale, vec_norm = vector_norm, vec_cosine = vector_cosine, vec_normalize = vector_normalize, embedding_search = vector_search;")
$js.Add("function __sura_rag_item_text(item, textField, rank, name) { if (item && typeof item === 'object' && !Array.isArray(item)) { if (!Object.prototype.hasOwnProperty.call(item, textField)) throw new Error(name + '(): result ' + rank + ' missing ' + JSON.stringify(textField) + ' text field'); return String(item[textField]); } if (typeof item === 'string') return item; throw new Error(name + '(): result ' + rank + ' must be a dict or string'); }")
$js.Add("function rag_context(query, docs, k = 3, embeddingField = 'embedding', textField = 'text') { return vector_search(query, docs, k, embeddingField).map((hit, i) => __sura_rag_item_text(hit.item, String(textField), i + 1, 'rag_context')).join('\n\n'); }")
$js.Add("function rag_sources(query, docs, k = 3, embeddingField = 'embedding', textField = 'text', titleField = 'title') { return vector_search(query, docs, k, embeddingField).map((hit, i) => { const src = {rank: i + 1, item: hit.item, text: __sura_rag_item_text(hit.item, String(textField), i + 1, 'rag_sources'), index: hit.index, score: hit.score}; if (hit.item && typeof hit.item === 'object' && !Array.isArray(hit.item)) { if (Object.prototype.hasOwnProperty.call(hit.item, 'id')) src.id = hit.item.id; if (titleField && Object.prototype.hasOwnProperty.call(hit.item, titleField)) src.title = hit.item[titleField]; } return src; }); }")
$js.Add("function rag_messages(question, context, system = 'Answer using only the provided context. If the context is insufficient, say you do not know.') { return [{role: 'system', content: String(system)}, {role: 'user', content: 'Context:\n' + String(context) + '\n\nQuestion:\n' + String(question)}]; }")
$js.Add("function rag_prepare(question, query, docs, k = 3, embeddingField = 'embedding', textField = 'text', system = 'Answer using only the provided context. If the context is insufficient, say you do not know.', titleField = 'title') { const sources = rag_sources(query, docs, k, embeddingField, textField, titleField); const context = sources.map((src, i) => '[' + (i + 1) + ']' + (src.title !== undefined ? ' ' + src.title : src.id !== undefined ? ' ' + src.id : '') + '\n' + src.text).join('\n\n'); return {question: String(question), context, sources, messages: rag_messages(question, context, system)}; }")
$js.Add("function tensor_shape(tensor) { if (!Array.isArray(tensor)) throw new Error('tensor_shape(): tensor must be an array'); const dims = []; let cur = tensor; while (Array.isArray(cur)) { dims.push(cur.length); cur = cur.length ? cur[0] : undefined; } return dims; }")
$js.Add("function __sura_tensor_fill(dims, depth, value) { if (depth >= dims.length) return value; return Array.from({length: dims[depth]}, () => __sura_tensor_fill(dims, depth + 1, value)); }")
$js.Add("function __sura_tensor_dims(shape, name) { if (!Array.isArray(shape)) throw new Error(name + '(): shape must be an array'); return shape.map(x => { const n = Number(x); if (!Number.isInteger(n) || n < 0) throw new Error(name + '(): shape dimensions must be non-negative integers'); return n; }); }")
$js.Add("function tensor_zeros(shape) { return __sura_tensor_fill(__sura_tensor_dims(shape, 'tensor_zeros'), 0, 0); }")
$js.Add("function tensor_fill(shape, value) { return __sura_tensor_fill(__sura_tensor_dims(shape, 'tensor_fill'), 0, Number(value)); }")
$js.Add("function __sura_tensor_elementwise(name, a, b, op) { const aa = Array.isArray(a), bb = Array.isArray(b); if (aa && bb) { if (a.length !== b.length) throw new Error(name + '(): tensor shapes differ'); return a.map((x, i) => __sura_tensor_elementwise(name, x, b[i], op)); } if (aa) return a.map(x => __sura_tensor_elementwise(name, x, b, op)); if (bb) return b.map(y => __sura_tensor_elementwise(name, a, y, op)); a = Number(a); b = Number(b); if (!Number.isFinite(a) || !Number.isFinite(b)) throw new Error(name + '(): tensor leaves must be numbers'); return op(a, b); }")
$js.Add("function tensor_add(a, b) { return __sura_tensor_elementwise('tensor_add', a, b, (x, y) => x + y); }")
$js.Add("function tensor_mul(a, b) { return __sura_tensor_elementwise('tensor_mul', a, b, (x, y) => x * y); }")
$js.Add("function tensor_clip(tensor, min, max) { const low = Number(min), high = Number(max); if (!Number.isFinite(low) || !Number.isFinite(high)) throw new Error('tensor_clip(): min and max must be numbers'); if (low > high) throw new Error('tensor_clip(): min must be <= max'); const visit = x => { if (Array.isArray(x)) return x.map(visit); const n = Number(x); if (!Number.isFinite(n)) throw new Error('tensor_clip(): tensor leaves must be numbers'); return Math.min(high, Math.max(low, n)); }; return visit(tensor); }")
$js.Add("function tensor_flatten(tensor) { const out = []; const visit = x => { if (Array.isArray(x)) x.forEach(visit); else { const n = Number(x); if (!Number.isFinite(n)) throw new Error('tensor_flatten(): tensor leaves must be numbers'); out.push(n); } }; visit(tensor); return out; }")
$js.Add("function __sura_tensor_sum_count(name, tensor) { let sum = 0, count = 0; const visit = x => { if (Array.isArray(x)) x.forEach(visit); else { const n = Number(x); if (!Number.isFinite(n)) throw new Error(name + '(): tensor leaves must be numbers'); sum += n; count++; } }; visit(tensor); return {sum, count}; }")
$js.Add("function tensor_sum(tensor) { return __sura_tensor_sum_count('tensor_sum', tensor).sum; }")
$js.Add("function tensor_mean(tensor) { const result = __sura_tensor_sum_count('tensor_mean', tensor); return result.count === 0 ? null : result.sum / result.count; }")
$js.Add("function __sura_tensor_squared_diff_sum(name, tensor, mean) { let sumSq = 0; const visit = x => { if (Array.isArray(x)) x.forEach(visit); else { const n = Number(x); if (!Number.isFinite(n)) throw new Error(name + '(): tensor leaves must be numbers'); const diff = n - mean; sumSq += diff * diff; } }; visit(tensor); return sumSq; }")
$js.Add("function tensor_variance(tensor) { const result = __sura_tensor_sum_count('tensor_variance', tensor); if (result.count === 0) return null; const mean = result.sum / result.count; return __sura_tensor_squared_diff_sum('tensor_variance', tensor, mean) / result.count; }")
$js.Add("function tensor_std(tensor) { const variance = tensor_variance(tensor); return variance === null ? null : Math.sqrt(variance); }")
$js.Add("function __sura_tensor_min_max(name, tensor, wantMin) { let seen = false, result = 0; const visit = x => { if (Array.isArray(x)) x.forEach(visit); else { const n = Number(x); if (!Number.isFinite(n)) throw new Error(name + '(): tensor leaves must be numbers'); if (!seen) { result = n; seen = true; } else if (wantMin ? n < result : n > result) result = n; } }; visit(tensor); return seen ? result : null; }")
$js.Add("function tensor_min(tensor) { return __sura_tensor_min_max('tensor_min', tensor, true); }")
$js.Add("function tensor_max(tensor) { return __sura_tensor_min_max('tensor_max', tensor, false); }")
$js.Add("function __sura_tensor_arg_min_max(name, tensor, wantMin) { let seen = false, best = 0, bestIndex = 0, index = 0; const visit = x => { if (Array.isArray(x)) x.forEach(visit); else { const n = Number(x); if (!Number.isFinite(n)) throw new Error(name + '(): tensor leaves must be numbers'); if (!seen) { best = n; bestIndex = index; seen = true; } else if (wantMin ? n < best : n > best) { best = n; bestIndex = index; } index++; } }; visit(tensor); return seen ? bestIndex : null; }")
$js.Add("function tensor_argmin(tensor) { return __sura_tensor_arg_min_max('tensor_argmin', tensor, true); }")
$js.Add("function tensor_argmax(tensor) { return __sura_tensor_arg_min_max('tensor_argmax', tensor, false); }")
$js.Add("function __sura_tensor_empty_like(tensor) { return Array.isArray(tensor) ? tensor.map(__sura_tensor_empty_like) : []; }")
$js.Add("function tensor_zscore(tensor) { const result = __sura_tensor_sum_count('tensor_zscore', tensor); if (result.count === 0) return __sura_tensor_empty_like(tensor); const mean = result.sum / result.count; const variance = __sura_tensor_squared_diff_sum('tensor_zscore', tensor, mean) / result.count; const stddev = Math.sqrt(variance); const build = x => Array.isArray(x) ? x.map(build) : (stddev === 0 ? 0 : (Number(x) - mean) / stddev); return build(tensor); }")
$js.Add("function tensor_softmax(tensor) { let seen = false, max = 0; const scan = x => { if (Array.isArray(x)) x.forEach(scan); else { const n = Number(x); if (!Number.isFinite(n)) throw new Error('tensor_softmax(): tensor leaves must be numbers'); if (!seen || n > max) { max = n; seen = true; } } }; scan(tensor); if (!seen) return __sura_tensor_empty_like(tensor); let denom = 0; const sum = x => { if (Array.isArray(x)) x.forEach(sum); else denom += Math.exp(Number(x) - max); }; sum(tensor); const build = x => Array.isArray(x) ? x.map(build) : Math.exp(Number(x) - max) / denom; return build(tensor); }")
$js.Add("function tensor_transpose(matrix) { if (!Array.isArray(matrix) || !matrix.length || !Array.isArray(matrix[0])) throw new Error('tensor_transpose(): matrix must be a non-empty 2D array'); const cols = matrix[0].length; matrix.forEach(row => { if (!Array.isArray(row) || row.length !== cols) throw new Error('tensor_transpose(): rows must have equal length'); }); return Array.from({length: cols}, (_, c) => matrix.map(row => row[c])); }")
$js.Add("function tensor_matmul(a, b) { if (!Array.isArray(a) || !Array.isArray(b) || !a.length || !b.length || !Array.isArray(a[0]) || !Array.isArray(b[0])) throw new Error('tensor_matmul(): arguments must be non-empty 2D arrays'); const leftCols = a[0].length, rightRows = b.length, rightCols = b[0].length; if (leftCols !== rightRows) throw new Error('tensor_matmul(): inner dimensions do not match'); a.forEach(row => { if (!Array.isArray(row) || row.length !== leftCols) throw new Error('tensor_matmul(): left rows must have equal length'); }); b.forEach(row => { if (!Array.isArray(row) || row.length !== rightCols) throw new Error('tensor_matmul(): right rows must have equal length'); }); return a.map(row => Array.from({length: rightCols}, (_, c) => row.reduce((sum, x, k) => sum + Number(x) * Number(b[k][c]), 0))); }")
$js.Add("function __sura_schema_type_name(value) { if (value === null) return 'nil'; if (Array.isArray(value)) return 'array'; if (typeof value === 'boolean') return 'bool'; if (typeof value === 'number') return Number.isInteger(value) ? 'integer' : 'number'; if (typeof value === 'string') return 'string'; return 'dict'; }")
$js.Add("function __sura_schema_type(typeName) { typeName = String(typeName).toLowerCase(); if (typeName === 'any') return ''; if (typeName === 'num') return 'number'; if (typeName === 'int') return 'integer'; if (typeName === 'str') return 'string'; if (typeName === 'bool') return 'boolean'; if (typeName === 'dict') return 'object'; if (typeName === 'list') return 'array'; if (typeName === 'nil') return 'null'; return typeName; }")
$js.Add("function __sura_schema_type_matches(value, typeName) { const t = __sura_schema_type(typeName); if (t === '') return true; if (t === 'number') return typeof value === 'number'; if (t === 'integer') return typeof value === 'number' && Number.isInteger(value); if (t === 'string') return typeof value === 'string'; if (t === 'boolean') return typeof value === 'boolean'; if (t === 'array') return Array.isArray(value); if (t === 'object') return value !== null && typeof value === 'object' && !Array.isArray(value); if (t === 'null') return value === null; return false; }")
$js.Add("function __sura_schema_control_keys(schema) { return schema && typeof schema === 'object' && !Array.isArray(schema) && ['type','required','properties','items','enum','min','max','min_len','max_len','pattern','additional'].some(k => Object.prototype.hasOwnProperty.call(schema, k)); }")
$js.Add("function __sura_schema_child_path(path, key) { return /^[A-Za-z_][A-Za-z0-9_]*$/.test(String(key)) ? path + '.' + key : path + '[' + JSON.stringify(String(key)) + ']'; }")
$js.Add("function __sura_schema_validate_into(value, schema, path, errors) { if (typeof schema === 'string') { if (!__sura_schema_type_matches(value, schema)) errors.push(path + ': expected ' + schema + ', got ' + __sura_schema_type_name(value)); return; } if (!schema || typeof schema !== 'object' || Array.isArray(schema)) { errors.push(path + ': schema must be a type string or dict'); return; } if (!__sura_schema_control_keys(schema)) { if (value === null || typeof value !== 'object' || Array.isArray(value)) { errors.push(path + ': expected dict, got ' + __sura_schema_type_name(value)); return; } for (const key of Object.keys(schema)) { if (!Object.prototype.hasOwnProperty.call(value, key)) errors.push(__sura_schema_child_path(path, key) + ': missing required field'); else __sura_schema_validate_into(value[key], schema[key], __sura_schema_child_path(path, key), errors); } return; } if (Array.isArray(schema.enum) && !schema.enum.some(candidate => __eq(value, candidate))) errors.push(path + ': value is not in enum'); if (schema.type !== undefined) { const types = Array.isArray(schema.type) ? schema.type : [schema.type]; const expected = types.map(x => String(x)).join('|'); if (!types.some(t => typeof t === 'string' && __sura_schema_type_matches(value, t))) errors.push(path + ': expected ' + expected + ', got ' + __sura_schema_type_name(value)); } if (schema.min !== undefined && (typeof value !== 'number' || value < Number(schema.min))) errors.push(path + ': number is below min ' + schema.min); if (schema.max !== undefined && (typeof value !== 'number' || value > Number(schema.max))) errors.push(path + ': number is above max ' + schema.max); if (schema.min_len !== undefined && (value == null || value.length === undefined || value.length < Number(schema.min_len))) errors.push(path + ': length is below min_len ' + schema.min_len); if (schema.max_len !== undefined && (value == null || value.length === undefined || value.length > Number(schema.max_len))) errors.push(path + ': length is above max_len ' + schema.max_len); if (schema.pattern !== undefined && (typeof value !== 'string' || !(new RegExp(String(schema.pattern))).test(value))) errors.push(path + ': string does not match pattern'); if (schema.items !== undefined && Array.isArray(value)) value.forEach((item, i) => __sura_schema_validate_into(item, schema.items, path + '[' + i + ']', errors)); else if (schema.items !== undefined) errors.push(path + ': expected array for items check, got ' + __sura_schema_type_name(value)); const props = schema.properties; if (props !== undefined || schema.required !== undefined) { if (value === null || typeof value !== 'object' || Array.isArray(value)) errors.push(path + ': expected dict for properties check, got ' + __sura_schema_type_name(value)); else { if (Array.isArray(schema.required)) schema.required.forEach(key => { if (!Object.prototype.hasOwnProperty.call(value, key)) errors.push(__sura_schema_child_path(path, key) + ': missing required field'); }); if (props && typeof props === 'object' && !Array.isArray(props)) { Object.keys(props).forEach(key => { if (Object.prototype.hasOwnProperty.call(value, key)) __sura_schema_validate_into(value[key], props[key], __sura_schema_child_path(path, key), errors); }); if (schema.additional === false) Object.keys(value).forEach(key => { if (!Object.prototype.hasOwnProperty.call(props, key)) errors.push(__sura_schema_child_path(path, key) + ': unexpected field'); }); } } } }")
$js.Add("function schema_errors(value, schema) { const errors = []; __sura_schema_validate_into(value, schema, String.fromCharCode(36), errors); return errors; }")
$js.Add("function schema_validate(value, schema) { return schema_errors(value, schema).length === 0; }")
$js.Add("function __sura_schema_type_value(spec) { if (typeof spec === 'string') { const mapped = __sura_schema_type(spec); return mapped ? mapped : undefined; } if (Array.isArray(spec)) { const out = spec.map(__sura_schema_type).filter(Boolean); return out.length ? out : undefined; } return spec; }")
$js.Add("function schema_to_json_schema(schema, strict = true) { if (typeof schema === 'string') { const type = __sura_schema_type_value(schema); return type === undefined ? {} : {type}; } if (!schema || typeof schema !== 'object' || Array.isArray(schema)) return schema; const out = {}; if (!__sura_schema_control_keys(schema)) { out.type = 'object'; out.properties = {}; out.required = []; for (const key of Object.keys(schema)) { out.properties[key] = schema_to_json_schema(schema[key], strict); out.required.push(key); } if (strict) out.additionalProperties = false; return out; } for (const key of Object.keys(schema)) if (!['type','required','properties','items','enum','min','max','min_len','max_len','pattern','additional'].includes(key)) out[key] = schema[key]; if (schema.type !== undefined) { const type = __sura_schema_type_value(schema.type); if (type !== undefined) out.type = type; } if (schema.enum !== undefined) out.enum = schema.enum; if (schema.pattern !== undefined) out.pattern = schema.pattern; if (schema.min !== undefined) out.minimum = schema.min; if (schema.max !== undefined) out.maximum = schema.max; if (schema.min_len !== undefined) out.minLength = schema.min_len; if (schema.max_len !== undefined) out.maxLength = schema.max_len; if (schema.items !== undefined) out.items = schema_to_json_schema(schema.items, strict); if (schema.properties !== undefined) { if (schema.properties && typeof schema.properties === 'object' && !Array.isArray(schema.properties)) { out.properties = {}; const required = []; for (const key of Object.keys(schema.properties)) { out.properties[key] = schema_to_json_schema(schema.properties[key], strict); required.push(key); } if (schema.required === undefined && strict) out.required = required; } else out.properties = schema.properties; if (out.type === undefined) out.type = 'object'; } if (schema.required !== undefined) out.required = schema.required; if (schema.additional !== undefined) out.additionalProperties = schema.additional; else if (strict && schema.properties !== undefined && out.additionalProperties === undefined) out.additionalProperties = false; return out; }")
$js.Add("function llm_message(role, content) { return {role: String(role), content: String(content)}; }")
$js.Add("function llm_messages(systemOrUser, user = undefined) { if (user !== undefined && String(systemOrUser) !== '') return [llm_message('system', systemOrUser), llm_message('user', user)]; return [llm_message('user', user === undefined ? systemOrUser : user)]; }")
$js.Add("function llm_request(model, messages, temperature = 0.2) { if (!Array.isArray(messages)) throw new Error('llm_request(): messages must be an array'); return {model: String(model), messages, temperature: Number(temperature)}; }")
$js.Add("function llm_request_json(model, messages, temperature = 0.2) { return JSON.stringify(llm_request(model, messages, temperature)); }")
$js.Add("function llm_response_schema(name, schema, strict = true) { name = String(name); if (!name) throw new Error('llm_response_schema(): name must not be empty'); return {type: 'json_schema', json_schema: {name, strict: !!strict, schema: schema_to_json_schema(schema, !!strict)}}; }")
$js.Add("function llm_request_schema(model, messages, schema, temperature = 0.2, name = 'sura_response', strict = true) { const req = llm_request(model, messages, temperature); req.response_format = llm_response_schema(name, schema, strict); return req; }")
$js.Add("function llm_request_schema_json(model, messages, schema, temperature = 0.2, name = 'sura_response', strict = true) { return JSON.stringify(llm_request_schema(model, messages, schema, temperature, name, strict)); }")
$js.Add("function llm_extract_text(response) { if (typeof response === 'string') response = JSON.parse(response); const choice = response && Array.isArray(response.choices) ? response.choices[0] : null; if (!choice) return ''; if (choice.message && choice.message.content !== undefined) return String(choice.message.content); if (choice.text !== undefined) return String(choice.text); return ''; }")
$js.Add("function llm_extract_json(response, schema = undefined) { const value = JSON.parse(llm_extract_text(response)); if (schema !== undefined) { const errors = schema_errors(value, schema); if (errors.length) throw new Error('llm_extract_json(): schema validation failed: ' + errors.join(' | ')); } return value; }")
$js.Add("function llm_usage(response) { if (typeof response === 'string') response = JSON.parse(response); const usage = response && typeof response === 'object' ? (response.usage && typeof response.usage === 'object' ? response.usage : response) : {}; const pick = (...keys) => { for (const key of keys) if (typeof usage[key] === 'number') return usage[key]; return 0; }; const prompt = pick('prompt_tokens', 'input_tokens'); const completion = pick('completion_tokens', 'output_tokens'); const input = pick('input_tokens', 'prompt_tokens'); const output = pick('output_tokens', 'completion_tokens'); let total = pick('total_tokens'); if (total === 0 && (prompt !== 0 || completion !== 0)) total = prompt + completion; return {prompt_tokens: prompt, completion_tokens: completion, input_tokens: input, output_tokens: output, total_tokens: total}; }")
$js.Add("function llm_cost(response, pricing) { if (!pricing || typeof pricing !== 'object' || Array.isArray(pricing)) throw new Error('llm_cost(): pricing must be a dict'); const usage = llm_usage(response); const pick = (...keys) => { for (const key of keys) if (typeof pricing[key] === 'number') return pricing[key]; return 0; }; const inputPrice = pick('input_per_million', 'prompt_per_million'); const outputPrice = pick('output_per_million', 'completion_per_million'); const inputCost = usage.input_tokens * inputPrice / 1000000; const outputCost = usage.output_tokens * outputPrice / 1000000; const out = {input_tokens: usage.input_tokens, output_tokens: usage.output_tokens, input_per_million: inputPrice, output_per_million: outputPrice, input_cost: inputCost, output_cost: outputCost, total_cost: inputCost + outputCost}; if (typeof pricing.currency === 'string') out.currency = pricing.currency; return out; }")
$js.Add("function llm_budget(response, pricing, limit) { limit = Number(limit); if (!Number.isFinite(limit) || limit < 0) throw new Error('llm_budget(): limit must be non-negative'); const out = llm_cost(response, pricing); out.limit = limit; out.remaining = limit - out.total_cost; out.within_budget = out.total_cost <= limit; out.over_budget = out.total_cost > limit; return out; }")
$js.Add("function llm_chat(endpoint, apiKey, model, messages, temperature = 0.2) { endpoint = String(endpoint); apiKey = String(apiKey); if (/[\r\n]/.test(endpoint) || /[\r\n]/.test(apiKey) || endpoint.includes(String.fromCharCode(34)) || apiKey.includes(String.fromCharCode(34))) throw new Error('llm_chat(): endpoint and api key must not contain quotes or newlines'); llm_request_json(model, messages, temperature); if (endpoint.startsWith('file://')) return file_read(decodeURIComponent(endpoint.slice(7))); throw new Error('llm_chat(): JS target supports file:// mock chat responses'); }")
$js.Add("function llm_chat_request(endpoint, apiKey, request) { endpoint = String(endpoint); apiKey = String(apiKey); if (/[\r\n]/.test(endpoint) || /[\r\n]/.test(apiKey) || endpoint.includes(String.fromCharCode(34)) || apiKey.includes(String.fromCharCode(34))) throw new Error('llm_chat_request(): endpoint and api key must not contain quotes or newlines'); if (typeof request !== 'string' && (!request || typeof request !== 'object' || Array.isArray(request))) throw new Error('llm_chat_request(): request must be a dict or JSON string'); if (typeof request !== 'string') JSON.stringify(request); if (endpoint.startsWith('file://')) return file_read(decodeURIComponent(endpoint.slice(7))); throw new Error('llm_chat_request(): JS target supports file:// mock chat responses'); }")
$js.Add("function __sura_llm_normalize_tool_call(raw) { const out = {}; if (!raw || typeof raw !== 'object') return out; if (raw.id !== undefined) out.id = String(raw.id); if (raw.type !== undefined) out.type = String(raw.type); const fn = raw.function && typeof raw.function === 'object' ? raw.function : raw; const name = fn.name; const args = fn.arguments; if (name !== undefined) out.name = String(name); if (args !== undefined) { if (typeof args === 'string') { out.raw_arguments = args; try { Object.assign(out, JSON.parse(args)); out.arguments = JSON.parse(args); } catch (_) { out.arguments = args; } } else { out.arguments = args; if (args && typeof args === 'object' && !Array.isArray(args)) Object.assign(out, args); } } return out; }")
$js.Add("function llm_tool_calls(response) { if (typeof response === 'string') response = JSON.parse(response); const out = []; if (!response || typeof response !== 'object') return out; if (Array.isArray(response.tool_calls)) return response.tool_calls.map(__sura_llm_normalize_tool_call); const choices = Array.isArray(response.choices) ? response.choices : []; for (const choice of choices) { const container = choice && choice.message && typeof choice.message === 'object' ? choice.message : choice; if (container && Array.isArray(container.tool_calls)) container.tool_calls.forEach(call => out.push(__sura_llm_normalize_tool_call(call))); if (container && container.function_call) out.push(__sura_llm_normalize_tool_call(container.function_call)); } return out; }")
$js.Add("function llm_tool_result(callOrId, result) { const id = callOrId && typeof callOrId === 'object' ? String(callOrId.id ?? callOrId.tool_call_id ?? '') : String(callOrId); return {role: 'tool', tool_call_id: id, content: typeof result === 'string' ? result : JSON.stringify(result)}; }")
$js.Add("function sse_parse(text) { const events = []; let eventName = ''; let dataLines = []; let id = ''; let retry = ''; let seen = false; const flush = () => { if (!seen) return; const ev = {}; if (eventName !== '') ev.event = eventName; if (dataLines.length) ev.data = dataLines.join('\n'); if (id !== '') ev.id = id; if (retry !== '') ev.retry = retry; events.push(ev); eventName = ''; dataLines = []; id = ''; retry = ''; seen = false; }; const lines = String(text).replace(/\r\n/g, '\n').replace(/\r/g, '\n').split('\n'); for (const line of lines) { if (line === '') { flush(); continue; } if (line.startsWith(':')) continue; const colon = line.indexOf(':'); const field = colon >= 0 ? line.slice(0, colon) : line; let value = colon >= 0 ? line.slice(colon + 1) : ''; if (value.startsWith(' ')) value = value.slice(1); seen = true; if (field === 'event') eventName = value; else if (field === 'data') dataLines.push(value); else if (field === 'id') id = value; else if (field === 'retry') retry = value; } flush(); return events; }")
$js.Add("function sse_data(text, parseJson = false) { const out = []; for (const ev of sse_parse(text)) { if (!Object.prototype.hasOwnProperty.call(ev, 'data')) continue; if (ev.data === '[DONE]') continue; out.push(parseJson ? JSON.parse(ev.data) : ev.data); } return out; }")
$js.Add("function __sura_llm_stream_chunk_text(chunk) { if (!chunk || typeof chunk !== 'object') return ''; const choices = Array.isArray(chunk.choices) ? chunk.choices : []; const choice = choices.length ? choices[0] : null; if (!choice || typeof choice !== 'object') return ''; if (choice.delta && choice.delta.content !== undefined) return String(choice.delta.content); if (choice.message && choice.message.content !== undefined) return String(choice.message.content); if (choice.text !== undefined) return String(choice.text); return ''; }")
$js.Add("function llm_stream_text(sseOrChunks) { const chunks = typeof sseOrChunks === 'string' ? sse_data(sseOrChunks, true) : sseOrChunks; if (!Array.isArray(chunks)) throw new Error('llm_stream_text(): expected SSE text or array of chunks'); return chunks.map(__sura_llm_stream_chunk_text).join(''); }")
$js.Add("function __sura_tool_known(name) { return ['http_get', 'http_request', 'shell'].includes(String(name)); }")
$js.Add("function __sura_tool_is_http(name) { return String(name) === 'http_get' || String(name) === 'http_request'; }")
$js.Add("function tool_validate(spec) { if (!spec || typeof spec !== 'object' || Array.isArray(spec) || typeof spec.name !== 'string' || !__sura_tool_known(spec.name)) return false; if (__sura_tool_is_http(spec.name)) { if (typeof spec.url !== 'string') return false; if (!/^(https?:\/\/|file:\/\/)/.test(spec.url)) return false; if (/[\r\n]/.test(spec.url) || spec.url.includes(String.fromCharCode(34))) return false; } if (spec.name === 'http_request') { const bodySources = (spec.body !== undefined ? 1 : 0) + (spec.json !== undefined ? 1 : 0) + (spec.form !== undefined ? 1 : 0); if (bodySources > 1) return false; if (spec.method !== undefined && typeof spec.method !== 'string') return false; const method = String(spec.method || (bodySources > 0 ? 'POST' : 'GET')).toUpperCase(); if (!/^[A-Z]+$/.test(method)) return false; if (spec.query !== undefined && (!spec.query || typeof spec.query !== 'object' || Array.isArray(spec.query))) return false; if (spec.form !== undefined && !__sura_form_params_valid(spec.form)) return false; if (spec.headers !== undefined && (!spec.headers || typeof spec.headers !== 'object' || Array.isArray(spec.headers))) return false; if (spec.timeout !== undefined && (!(typeof spec.timeout === 'number') || spec.timeout <= 0 || spec.timeout > 3600)) return false; if (spec.url && spec.url.startsWith('file://') && (method !== 'GET' || bodySources > 0 || (spec.query && Object.keys(spec.query).length))) return false; } if (spec.name === 'shell' && (typeof spec.command !== 'string' || spec.command === '' || /[\r\n]/.test(spec.command))) return false; return true; }")
$js.Add("function tool_spec(name, args) { if (!args || typeof args !== 'object' || Array.isArray(args)) throw new Error('tool_spec(): arg 2 must be a dict'); const spec = Object.assign({name: String(name)}, args); if (!tool_validate(spec)) throw new Error('tool_spec(): invalid spec'); return spec; }")
$js.Add("function tool_schema(name) { name = String(name); if (!__sura_tool_known(name)) throw new Error('tool_schema(): unknown tool ' + JSON.stringify(name)); const schema = {name, kind: 'tool', required: [], fields: {}}; if (name === 'http_get') { schema.required.push('url'); schema.fields.url = 'string:http-url|file-url'; } else if (name === 'http_request') { schema.required.push('url'); Object.assign(schema.fields, {url: 'string:http-url|file-url', query: 'dict:query-params', method: 'string:http-method', headers: 'dict:string-values', body: 'string|json-value', json: 'json-value', form: 'dict:form-params', content_type: 'string:mime-type', timeout: 'number:seconds'}); } else if (name === 'shell') { schema.required.push('command'); schema.fields.command = 'string:shell-command'; } return schema; }")
$js.Add("function tool_list() { return ['http_get', 'http_request', 'shell']; }")
$js.Add("function __sura_llm_tool_field_schema(field, spec) { spec = String(spec); const out = {description: spec}; if (spec.includes('number')) out.type = 'number'; else if (spec.includes('dict')) { out.type = 'object'; out.additionalProperties = field === 'headers' ? {type: 'string'} : true; } else if (spec.includes('json-value')) out.type = ['string', 'number', 'boolean', 'object', 'array', 'null']; else out.type = 'string'; if (field === 'method') out.enum = ['GET', 'POST', 'PUT', 'PATCH', 'DELETE']; return out; }")
$js.Add("function __sura_llm_tool_description(name) { if (name === 'http_get') return 'Fetch a URL and return the response body as text.'; if (name === 'http_request') return 'Send an HTTP request with method, URL, headers, query, body, JSON, and timeout fields.'; if (name === 'shell') return 'Run a shell command when an explicit tool policy allows shell execution.'; return 'Sura automation tool.'; }")
$js.Add("function __sura_llm_openai_tool_schema(name) { name = String(name); const schema = tool_schema(name); const properties = {}; for (const field of Object.keys(schema.fields)) properties[field] = __sura_llm_tool_field_schema(field, schema.fields[field]); return {type: 'function', function: {name, description: __sura_llm_tool_description(name), parameters: {type: 'object', properties, required: schema.required.slice(), additionalProperties: false}}}; }")
$js.Add("function llm_tools(names = undefined) { if (names === undefined || names === null) names = tool_list(); else if (typeof names === 'string') names = [names]; if (!Array.isArray(names)) throw new Error('llm_tools(): expected nil, string, or array of tool names'); const seen = new Set(); const out = []; for (const raw of names) { const name = String(raw); if (!name || seen.has(name)) continue; seen.add(name); out.push(__sura_llm_openai_tool_schema(name)); } return out; }")
$js.Add("const llm_tool_schemas = llm_tools;")
$js.Add("function llm_request_tools(model, messages, toolNames, temperature = 0.2) { const req = llm_request(model, messages, temperature); req.tools = llm_tools(toolNames); return req; }")
$js.Add("function llm_request_tools_json(model, messages, toolNames, temperature = 0.2) { return JSON.stringify(llm_request_tools(model, messages, toolNames, temperature)); }")
$js.Add("function llm_request_tools_schema(model, messages, toolNames, schema, temperature = 0.2, name = 'sura_response', strict = true) { const req = llm_request_tools(model, messages, toolNames, temperature); req.response_format = llm_response_schema(name, schema, strict); return req; }")
$js.Add("function llm_request_tools_schema_json(model, messages, toolNames, schema, temperature = 0.2, name = 'sura_response', strict = true) { return JSON.stringify(llm_request_tools_schema(model, messages, toolNames, schema, temperature, name, strict)); }")
$js.Add("function __sura_array_has_case(list, value) { return Array.isArray(list) && list.some(x => String(x).toLowerCase() === String(value).toLowerCase()); }")
$js.Add("function tool_allowed(spec, policy) { if (!tool_validate(spec) || !policy || typeof policy !== 'object' || Array.isArray(policy)) return false; if (Array.isArray(policy.tools) && !policy.tools.includes(spec.name)) return false; if (__sura_tool_is_http(spec.name)) { if (Array.isArray(policy.url_prefixes) && !policy.url_prefixes.some(prefix => String(spec.url).startsWith(String(prefix)))) return false; const hasBody = spec.body !== undefined || spec.json !== undefined || spec.form !== undefined; const method = String(spec.method || (hasBody ? 'POST' : 'GET')).toUpperCase(); if (Array.isArray(policy.http_methods) && !__sura_array_has_case(policy.http_methods, method)) return false; } if (spec.name === 'shell' && !policy.allow_shell) return false; return true; }")
$js.Add("function tool_call(spec) { if (!tool_validate(spec)) throw new Error('tool_call(): invalid spec'); if (spec.name === 'http_get') return http_get(spec.url); if (spec.name === 'http_request') return http_request(spec); throw new Error('tool_call(): JS target does not execute shell specs'); }")
$js.Add("function tool_call_policy(spec, policy) { if (!tool_allowed(spec, policy)) throw new Error('tool_call_policy(): blocked by policy'); return tool_call(spec); }")
$js.Add("function llm_run_tools(response, policy) { return llm_tool_calls(response).map(call => llm_tool_result(call, tool_call_policy(call, policy))); }")
$js.Add("function llm_next_messages(messages, response, policy) { if (!Array.isArray(messages)) throw new Error('llm_next_messages(): messages must be an array'); const parsed = typeof response === 'string' ? JSON.parse(response) : response; const out = messages.slice(); let assistant = null; if (parsed && typeof parsed === 'object' && !Array.isArray(parsed)) { if (parsed.message && typeof parsed.message === 'object') assistant = Object.assign({}, parsed.message); const choices = Array.isArray(parsed.choices) ? parsed.choices : []; const first = choices.length && choices[0] && typeof choices[0] === 'object' ? choices[0] : null; if (!assistant && first && first.message && typeof first.message === 'object') assistant = Object.assign({}, first.message); if (!assistant && first && first.text !== undefined) assistant = {role: 'assistant', content: String(first.text)}; if (!assistant && Array.isArray(parsed.tool_calls)) assistant = {role: 'assistant', tool_calls: parsed.tool_calls}; if (!assistant && parsed.content !== undefined) assistant = {role: 'assistant', content: String(parsed.content)}; } if (!assistant) assistant = {role: 'assistant'}; if (assistant.role === undefined) assistant.role = 'assistant'; out.push(assistant); llm_run_tools(parsed, policy).forEach(msg => out.push(msg)); return out; }")
$js.Add("function llm_next_request(model, messages, response, policy, toolNames, temperature = 0.2) { return llm_request_tools(model, llm_next_messages(messages, response, policy), toolNames, temperature); }")
$js.Add("function llm_next_request_json(model, messages, response, policy, toolNames, temperature = 0.2) { return JSON.stringify(llm_next_request(model, messages, response, policy, toolNames, temperature)); }")
$js.Add("function llm_next_schema_request(model, messages, response, policy, toolNames, schema, temperature = 0.2, name = 'sura_response', strict = true) { return llm_request_tools_schema(model, llm_next_messages(messages, response, policy), toolNames, schema, temperature, name, strict); }")
$js.Add("function llm_next_schema_request_json(model, messages, response, policy, toolNames, schema, temperature = 0.2, name = 'sura_response', strict = true) { return JSON.stringify(llm_next_schema_request(model, messages, response, policy, toolNames, schema, temperature, name, strict)); }")
$js.Add("function __sura_require_path(name) { if (!__sura_path) throw new Error(name + '(): path is unavailable'); return __sura_path; }")
$js.Add("function path_join(...parts) { return __sura_require_path('path_join').join(...parts.map(x => String(x))); }")
$js.Add("function path_basename(path) { return __sura_require_path('path_basename').basename(String(path)); }")
$js.Add("function path_dirname(path) { return __sura_require_path('path_dirname').dirname(String(path)); }")
$js.Add("function path_ext(path) { return __sura_require_path('path_ext').extname(String(path)); }")
$js.Add("function path_stem(path) { return __sura_require_path('path_stem').basename(String(path), path_ext(path)); }")
$js.Add("function path_normalize(path) { return __sura_require_path('path_normalize').normalize(String(path)); }")
$js.Add("function path_abs(path) { return __sura_require_path('path_abs').resolve(String(path)); }")
$js.Add("function path_relative(path, base = undefined) { const p = __sura_require_path('path_relative'); return p.relative(base === undefined ? p.resolve('.') : String(base), String(path)); }")
$js.Add("function stream_from(x) { return {type: 'stream', index: 0, data: Array.isArray(x) ? x : String(x).split(/\r?\n/)}; }")
$js.Add("function __stream_state(s, name) { if (!s || s.type !== 'stream' || !Array.isArray(s.data) || typeof s.index !== 'number') throw new Error(name + '(): invalid stream'); return s; }")
$js.Add('function __sura_deep_clone(value) { if (Array.isArray(value)) return value.map(__sura_deep_clone); if (value && typeof value === "object") { const out = {}; for (const key of Object.keys(value)) out[key] = __sura_deep_clone(value[key]); return out; } return value; }')
$js.Add('function __sura_path_tokens(path) { path = String(path); if (path === "" || path === "$") return []; const text = path.replace(/^\$\.?/, ""); const re = /(?:^|\.)([^.\[\]]+)|\[(\d+)\]|\[["'']([^"'']+)["'']\]/g; const tokens = []; let m; while ((m = re.exec(text)) !== null) { if (m[2] !== undefined) tokens.push({index: Number(m[2])}); else tokens.push({key: m[1] ?? m[3]}); } return tokens; }')
$js.Add('function __sura_path_get(obj, path, fallback = null) { path = String(path); if (path === "" || path === "$") return obj; const text = path.replace(/^\$\.?/, ""); const re = /(?:^|\.)([^.\[\]]+)|\[(\d+)\]|\[["'']([^"'']+)["'']\]/g; const tokens = []; let m; while ((m = re.exec(text)) !== null) tokens.push(m[1] ?? m[2] ?? m[3]); let cur = obj; for (const token of tokens) { if (cur == null || !Object.prototype.hasOwnProperty.call(Object(cur), token)) return fallback; cur = cur[token]; } return cur; }')
$js.Add('function __sura_path_has(obj, path) { path = String(path); if (path === "" || path === "$") return true; const text = path.replace(/^\$\.?/, ""); const re = /(?:^|\.)([^.\[\]]+)|\[(\d+)\]|\[["'']([^"'']+)["'']\]/g; const tokens = []; let m; while ((m = re.exec(text)) !== null) tokens.push(m[1] ?? m[2] ?? m[3]); let cur = obj; for (const token of tokens) { if (cur == null || !Object.prototype.hasOwnProperty.call(Object(cur), token)) return false; cur = cur[token]; } return true; }')
$js.Add('function json_path(value, path, fallback = null) { return __sura_path_get(value, path, fallback); }')
$js.Add('function json_has_path(value, path) { return __sura_path_has(value, path); }')
$js.Add('function json_merge_patch(target, patch) { if (!patch || typeof patch !== "object" || Array.isArray(patch)) return __sura_deep_clone(patch); const out = target && typeof target === "object" && !Array.isArray(target) ? __sura_deep_clone(target) : {}; for (const key of Object.keys(patch)) { if (patch[key] == null) delete out[key]; else out[key] = json_merge_patch(out[key], patch[key]); } return out; }')
$js.Add('function json_delete_path(value, path) { path = String(path); if (path === "" || path === "$") return null; const text = path.replace(/^\$\.?/, ""); const re = /(?:^|\.)([^.\[\]]+)|\[(\d+)\]|\[["'']([^"'']+)["'']\]/g; const tokens = []; let m; while ((m = re.exec(text)) !== null) tokens.push(m[1] ?? m[2] ?? m[3]); const out = __sura_deep_clone(value); let cur = out; for (let i = 0; i + 1 < tokens.length; i++) { const token = tokens[i]; if (cur == null || !Object.prototype.hasOwnProperty.call(Object(cur), token)) return out; cur = cur[token]; } const last = tokens[tokens.length - 1]; if (cur != null && Object.prototype.hasOwnProperty.call(Object(cur), last)) { if (Array.isArray(cur) && /^\d+$/.test(String(last))) cur.splice(Number(last), 1); else delete cur[last]; } return out; }')
$js.Add('function json_set_path(value, path, newValue) { const tokens = __sura_path_tokens(path); if (!tokens.length) return __sura_deep_clone(newValue); let out = __sura_deep_clone(value); if (tokens[0].index !== undefined) { if (!Array.isArray(out)) out = []; } else if (!out || typeof out !== "object" || Array.isArray(out)) out = {}; let cur = out; for (let i = 0; i < tokens.length; i++) { const token = tokens[i]; const last = i + 1 === tokens.length; if (token.index !== undefined) { if (!Array.isArray(cur)) return out; while (cur.length <= token.index) cur.push(null); if (last) { cur[token.index] = __sura_deep_clone(newValue); break; } const next = tokens[i + 1]; if (next.index !== undefined) { if (!Array.isArray(cur[token.index])) cur[token.index] = []; } else if (!cur[token.index] || typeof cur[token.index] !== "object" || Array.isArray(cur[token.index])) cur[token.index] = {}; cur = cur[token.index]; } else { if (!cur || typeof cur !== "object" || Array.isArray(cur)) return out; if (last) { cur[token.key] = __sura_deep_clone(newValue); break; } const next = tokens[i + 1]; if (next.index !== undefined) { if (!Array.isArray(cur[token.key])) cur[token.key] = []; } else if (!cur[token.key] || typeof cur[token.key] !== "object" || Array.isArray(cur[token.key])) cur[token.key] = {}; cur = cur[token.key]; } } return out; }')
$js.Add('const dict_get_path = json_path;')
$js.Add("function __sura_need_dict(name, value) { if (!value || typeof value !== 'object' || Array.isArray(value)) throw new Error(name + '(): arg 1 must be a dict'); return value; }")
$js.Add("function __sura_dict_keys(value) { return Object.keys(value).sort(); }")
$js.Add("function dict_keys(value) { return __sura_dict_keys(__sura_need_dict('dict_keys', value)); }")
$js.Add("function dict_values(value) { value = __sura_need_dict('dict_values', value); return __sura_dict_keys(value).map(key => value[key]); }")
$js.Add("function dict_items(value) { value = __sura_need_dict('dict_items', value); return __sura_dict_keys(value).map(key => ({key, value: value[key]})); }")
$js.Add("function dict_merge(...items) { if (!items.length) throw new Error('dict_merge(): expected 1+ arg(s), got 0'); const out = {}; items.forEach((item, i) => { if (!item || typeof item !== 'object' || Array.isArray(item)) throw new Error('dict_merge(): arg ' + (i + 1) + ' must be a dict'); Object.assign(out, item); }); return out; }")
$js.Add("function __sura_dict_key_list(name, keys) { if (!Array.isArray(keys)) throw new Error(name + '(): arg 2 must be an array'); return keys.map(key => { if (typeof key !== 'string') throw new Error(name + '(): keys must be strings'); return key; }); }")
$js.Add("function dict_pick(value, keys) { value = __sura_need_dict('dict_pick', value); const out = {}; for (const key of __sura_dict_key_list('dict_pick', keys)) if (Object.prototype.hasOwnProperty.call(value, key)) out[key] = value[key]; return out; }")
$js.Add("function dict_omit(value, keys) { value = __sura_need_dict('dict_omit', value); const omit = new Set(__sura_dict_key_list('dict_omit', keys)); const out = {}; for (const key of Object.keys(value)) if (!omit.has(key)) out[key] = value[key]; return out; }")
$js.Add('function __sura_need_array(name, value) { if (!Array.isArray(value)) throw new Error(name + "(): expected array"); return value; }')
$js.Add('function jsonl_parse(text) { const out = []; const lines = String(text).replace(/\r\n/g, "\n").replace(/\r/g, "\n").split("\n"); for (const line of lines) { if (line.trim() === "") continue; out.push(JSON.parse(line)); } return out; }')
$js.Add('function jsonl_stringify(rows, trailing_newline = false) { rows = __sura_need_array("jsonl_stringify", rows); let out = rows.map(row => JSON.stringify(row)).join("\n"); if (trailing_newline && out !== "") out += "\n"; return out; }')
$js.Add('function __sura_csv_parse_rows(text) { const quote = String.fromCharCode(34); const rows = []; let row = []; let field = ""; let inQuotes = false; let fieldStarted = false; let justEndedRow = true; const endField = () => { row.push(field); field = ""; fieldStarted = false; }; const endRow = () => { endField(); rows.push(row); row = []; justEndedRow = true; }; text = String(text); for (let i = 0; i < text.length; i++) { const ch = text[i]; if (inQuotes) { if (ch === quote) { if (i + 1 < text.length && text[i + 1] === quote) { field += quote; i++; } else inQuotes = false; } else field += ch; fieldStarted = true; justEndedRow = false; continue; } if (ch === quote && !fieldStarted) { inQuotes = true; fieldStarted = true; justEndedRow = false; } else if (ch === ",") { endField(); justEndedRow = false; } else if (ch === "\n") { endRow(); } else if (ch === "\r") { if (i + 1 < text.length && text[i + 1] === "\n") i++; endRow(); } else { field += ch; fieldStarted = true; justEndedRow = false; } } if (inQuotes) throw new Error("csv_parse(): unterminated quoted field"); if (fieldStarted || row.length || !justEndedRow) { endField(); rows.push(row); } return rows; }')
$js.Add('function csv_parse(text, has_header = false) { const rows = __sura_csv_parse_rows(text); if (!has_header) return rows; if (!rows.length) return []; const header = rows[0]; return rows.slice(1).map(cells => { const item = {}; header.forEach((key, i) => item[key] = i < cells.length ? cells[i] : ""); return item; }); }')
$js.Add('function __sura_csv_escape_cell(value) { const quote = String.fromCharCode(34); const text = value === null || value === undefined ? "" : String(value); if (!/[,"\r\n]/.test(text)) return text; return quote + text.replace(/"/g, quote + quote) + quote; }')
$js.Add('function __sura_csv_append_row(lines, cells) { lines.push(cells.map(__sura_csv_escape_cell).join(",")); }')
$js.Add('function csv_stringify(rows, headers = undefined) { rows = __sura_need_array("csv_stringify", rows); if (headers !== undefined) headers = __sura_need_array("csv_stringify", headers).map(String); else { const set = new Set(); for (const row of rows) if (row && typeof row === "object" && !Array.isArray(row)) Object.keys(row).forEach(key => set.add(key)); headers = Array.from(set).sort(); } const lines = []; if (headers.length) __sura_csv_append_row(lines, headers); for (const row of rows) { if (Array.isArray(row)) __sura_csv_append_row(lines, row); else if (row && typeof row === "object") { if (!headers.length) throw new Error("csv_stringify(): dict rows require headers"); __sura_csv_append_row(lines, headers.map(key => Object.prototype.hasOwnProperty.call(row, key) ? row[key] : "")); } else throw new Error("csv_stringify(): rows must contain arrays or dicts"); } return lines.join("\n"); }')
$js.Add('function __sura_ini_unquote(text) { if (text.length < 2) return text; const quote = text[0]; if ((quote !== String.fromCharCode(34) && quote !== String.fromCharCode(39)) || text[text.length - 1] !== quote) return text; let out = ""; for (let i = 1; i + 1 < text.length; i++) { const ch = text[i]; if (ch === "\\" && i + 1 < text.length - 1) { const next = text[++i]; if (next === "n") out += "\n"; else if (next === "r") out += "\r"; else if (next === "t") out += "\t"; else out += next; } else out += ch; } return out; }')
$js.Add('function ini_parse(text) { const root = {}; let current = root; const lines = String(text).replace(/\r\n/g, "\n").replace(/\r/g, "\n").split("\n"); for (let i = 0; i < lines.length; i++) { const work = lines[i].trim(); if (work === "" || work.startsWith("#") || work.startsWith(";")) continue; if (work.startsWith("[") && work.endsWith("]")) { const section = work.slice(1, -1).trim(); if (!section) throw new Error("ini_parse(): empty section name at line " + (i + 1)); if (Object.prototype.hasOwnProperty.call(root, section) && (root[section] === null || typeof root[section] !== "object" || Array.isArray(root[section]))) throw new Error("ini_parse(): section conflicts with scalar key " + section); if (!Object.prototype.hasOwnProperty.call(root, section)) root[section] = {}; current = root[section]; continue; } const eq = work.indexOf("="); const colon = work.indexOf(":"); const candidates = [eq, colon].filter(x => x >= 0); if (!candidates.length) throw new Error("ini_parse(): expected key=value at line " + (i + 1)); const sep = Math.min(...candidates); const key = work.slice(0, sep).trim(); if (!key) throw new Error("ini_parse(): empty key at line " + (i + 1)); current[key] = __sura_ini_unquote(work.slice(sep + 1).trim()); } return root; }')
$js.Add('function __sura_ini_key_safe(key) { return String(key) !== "" && !/[:=\[\]\r\n]/.test(String(key)); }')
$js.Add('function __sura_ini_escape_value(value) { let text = value === null || value === undefined ? "" : String(value); const quote = text === "" || /^\s|\s$/.test(text) || /[#;=\[\]\r\n]/.test(text); if (!quote) return text; return String.fromCharCode(34) + text.replace(/\\/g, "\\\\").replace(/"/g, "\\\"").replace(/\n/g, "\\n").replace(/\r/g, "\\r").replace(/\t/g, "\\t") + String.fromCharCode(34); }')
$js.Add('function __sura_ini_append_assignments(lines, obj) { for (const key of Object.keys(obj).filter(k => !(obj[k] && typeof obj[k] === "object" && !Array.isArray(obj[k]))).sort()) { if (!__sura_ini_key_safe(key)) throw new Error("ini_stringify(): invalid key " + key); lines.push(key + "=" + __sura_ini_escape_value(obj[key])); } }')
$js.Add('function ini_stringify(value) { value = __sura_need_dict("ini_stringify", value); const lines = []; __sura_ini_append_assignments(lines, value); const sections = Object.keys(value).filter(k => value[k] && typeof value[k] === "object" && !Array.isArray(value[k])).sort(); for (const section of sections) { if (!__sura_ini_key_safe(section)) throw new Error("ini_stringify(): invalid section " + section); if (lines.length) lines.push(""); lines.push("[" + section + "]"); __sura_ini_append_assignments(lines, value[section]); } return lines.join("\n"); }')
$js.Add('function __sura_collection_key_text(value) { if (value === null || value === undefined) return "nil"; if (typeof value === "string") return value; if (Array.isArray(value) || typeof value === "object") return JSON.stringify(value); return String(value); }')
$js.Add('function __sura_collection_less(left, right) { if (typeof left === "number" && typeof right === "number") return left < right; if (typeof left === "string" && typeof right === "string") return left < right; const lt = type(left), rt = type(right); if (lt !== rt) return lt < rt; return __sura_collection_key_text(left) < __sura_collection_key_text(right); }')
$js.Add('function pluck(rows, path, fallback = null) { rows = __sura_need_array("pluck", rows); return rows.map(row => __sura_path_get(row, path, fallback)); }')
$js.Add('function count_by(rows, path) { rows = __sura_need_array("count_by", rows); const out = {}; for (const row of rows) { const key = __sura_collection_key_text(__sura_path_get(row, path, null)); out[key] = (out[key] || 0) + 1; } return out; }')
$js.Add('function group_by(rows, path) { rows = __sura_need_array("group_by", rows); const out = {}; for (const row of rows) { const key = __sura_collection_key_text(__sura_path_get(row, path, null)); if (!Array.isArray(out[key])) out[key] = []; out[key].push(row); } return out; }')
$js.Add('function sort_by(rows, path, descending = false) { rows = __sura_need_array("sort_by", rows); rows.sort((a, b) => { const av = __sura_path_get(a, path, null); const bv = __sura_path_get(b, path, null); if (__eq(av, bv)) return 0; const less = descending ? __sura_collection_less(bv, av) : __sura_collection_less(av, bv); return less ? -1 : 1; }); return rows; }')
$js.Add('const array_pluck = pluck, array_count_by = count_by, array_group_by = group_by, array_sort_by = sort_by;')
$js.Add('function __sura_template_text(value) { if (value === null || value === undefined) return ""; if (typeof value === "string") return value; if (Array.isArray(value) || typeof value === "object") return JSON.stringify(value); return String(value); }')
$js.Add('function template_render(text, data, missing = "") { text = String(text); let out = "", pos = 0; while (pos < text.length) { const brace = text.indexOf("{{", pos); const bracket = text.indexOf("[[", pos); let open = -1, closeToken = ""; if (brace >= 0 && (bracket < 0 || brace < bracket)) { open = brace; closeToken = "}}"; } else if (bracket >= 0) { open = bracket; closeToken = "]]"; } if (open < 0) { out += text.slice(pos); break; } out += text.slice(pos, open); const close = text.indexOf(closeToken, open + 2); if (close < 0) throw new Error("template_render(): unterminated placeholder"); const path = text.slice(open + 2, close).trim(); if (!path) throw new Error("template_render(): empty placeholder"); out += __sura_template_text(__sura_path_get(data, path, missing)); pos = close + 2; } return out; }')
$js.Add('function __sura_equal(a, b) { return JSON.stringify(a) === JSON.stringify(b); }')
$js.Add('function __sura_match(row, criteria) { if (!row || typeof row !== "object" || Array.isArray(row)) return false; for (const key of Object.keys(criteria)) { if (!Object.prototype.hasOwnProperty.call(row, key) || !__sura_equal(row[key], criteria[key])) return false; } return true; }')
$js.Add("function stream_next(s) { s = __stream_state(s, 'stream_next'); return s.index >= s.data.length ? null : s.data[s.index++]; }")
$js.Add("function stream_take(s, count) { s = __stream_state(s, 'stream_take'); count = Math.max(0, Math.trunc(Number(count))); const available = Math.max(0, s.data.length - s.index); const end = s.index + Math.min(count, available); const out = s.data.slice(s.index, end); s.index = end; return out; }")
$js.Add("function stream_batch(s, size) { s = __stream_state(s, 'stream_batch'); size = Math.trunc(Number(size)); if (size <= 0) throw new Error('stream_batch(): size must be positive'); const out = []; while (s.index < s.data.length) { const end = Math.min(s.index + size, s.data.length); out.push(s.data.slice(s.index, end)); s.index = end; } return out; }")
$js.Add("function stream_map(s, path, fallback = null) { s = __stream_state(s, 'stream_map'); const out = []; while (s.index < s.data.length) out.push(__sura_path_get(s.data[s.index++], path, fallback)); s.index = s.data.length; return stream_from(out); }")
$js.Add("function stream_filter(s, criteria) { s = __stream_state(s, 'stream_filter'); const out = []; while (s.index < s.data.length) { const row = s.data[s.index++]; if (__sura_match(row, criteria)) out.push(row); } s.index = s.data.length; return stream_from(out); }")
$js.Add("function stream_window(s, size, step = 1) { s = __stream_state(s, 'stream_window'); size = Math.trunc(Number(size)); step = Math.trunc(Number(step)); if (size <= 0) throw new Error('stream_window(): size must be positive'); if (step <= 0) throw new Error('stream_window(): step must be positive'); const out = []; for (let start = s.index; start + size <= s.data.length; start += step) out.push(s.data.slice(start, start + size)); s.index = s.data.length; return out; }")
$js.Add("function stream_skip(s, count) { s = __stream_state(s, 'stream_skip'); count = Math.max(0, Math.trunc(Number(count))); const available = Math.max(0, s.data.length - s.index); const next = s.index + Math.min(count, available); const skipped = next - s.index; s.index = next; return skipped; }")
$js.Add("function stream_count(s) { s = __stream_state(s, 'stream_count'); return Math.max(0, s.data.length - s.index); }")
$js.Add("function stream_collect(s) { s = __stream_state(s, 'stream_collect'); const out = s.data.slice(s.index); s.index = s.data.length; return out; }")
$js.Add("function stream_join(s, sep = '') { return stream_collect(s).map(x => String(x)).join(sep); }")
$js.Add("function __sura_stream_number(name, item, hasPath, path) { const value = hasPath ? __sura_path_get(item, path, null) : item; if (typeof value !== 'number' || !Number.isFinite(value)) throw new Error(name + '(): stream value must be a number, got ' + String(value)); return value; }")
$js.Add("function stream_sum(s, path = undefined) { s = __stream_state(s, 'stream_sum'); const hasPath = path !== undefined; let sum = 0; while (s.index < s.data.length) sum += __sura_stream_number('stream_sum', s.data[s.index++], hasPath, path); s.index = s.data.length; return sum; }")
$js.Add("function stream_avg(s, path = undefined) { s = __stream_state(s, 'stream_avg'); const hasPath = path !== undefined; let sum = 0, count = 0; while (s.index < s.data.length) { sum += __sura_stream_number('stream_avg', s.data[s.index++], hasPath, path); count++; } s.index = s.data.length; return count === 0 ? null : sum / count; }")
$js.Add("function stream_lines(path) { if (!__sura_fs) throw new Error('stream_lines(): fs is unavailable'); return stream_from(__sura_fs.readFileSync(String(path), 'utf8').replace(/\r\n/g, '\n').replace(/\r$/, '')); }")
$js.Add("function text_chunks(text, maxChars = 800, overlap = 0) { text = String(text); maxChars = __sura_int_arg('text_chunks', maxChars); overlap = __sura_int_arg('text_chunks', overlap); if (maxChars <= 0) throw new Error('text_chunks(): arg 2 must be a positive integer'); if (overlap < 0) throw new Error('text_chunks(): arg 3 must be a non-negative integer'); if (overlap >= maxChars) throw new Error('text_chunks(): overlap must be smaller than max_chars'); const chars = Array.from(text); const out = []; if (!chars.length) return out; const step = maxChars - overlap; for (let start = 0; start < chars.length;) { const end = Math.min(chars.length, start + maxChars); out.push(chars.slice(start, end).join('')); if (end === chars.length) break; start += step; } return out; }")
$js.Add("const text_chunk = text_chunks;")
$js.Add("const array = { len: length, length, size: length, slice: (arr, start, end = undefined) => arr.slice(start, end), contains, index_of: (arr, item) => arr.indexOf(item), push, pop, clone: arr => arr.slice(), sort: arr => arr.slice().sort((a, b) => a < b ? -1 : a > b ? 1 : 0), reverse: arr => arr.slice().reverse(), concat: (...items) => [].concat(...items), sum: array_sum, avg: array_avg, min: array_min, max: array_max, unique: array_unique, flatten: array_flatten, range: array_range, chunk: array_chunk, chunks: array_chunk, zip: array_zip, repeat: array_repeat };")
$js.Add("const set = { union: set_union, intersection: set_intersection, difference: set_difference, symmetric_difference: set_symmetric_difference, symdiff: set_symmetric_difference, is_subset: set_is_subset, subset: set_is_subset, is_superset: set_is_superset, superset: set_is_superset };")
$js.Add("const sign = x => Number(x) > 0 ? 1 : (Number(x) < 0 ? -1 : 0);")
$js.Add("const math = { sqrt, sin, cos, tan, floor, ceil, round, abs, sign, pow, min, max, pi: Math.PI, clamp: (x, lo, hi) => Math.min(Math.max(x, lo), hi) };")
$js.Add("const string = { len: length, length, size: length, upper, lower, contains, split, join, trim, starts_with: (x, prefix) => String(x).startsWith(prefix), ends_with: (x, suffix) => String(x).endsWith(suffix), lines: string_lines, words: string_words, repeat: string_repeat, pad_left: string_pad_left, pad_right: string_pad_right, chunks: text_chunks };")
$js.Add("const json = { parse: json_parse, try_parse: json_try_parse, stringify: json_stringify, pretty: json_pretty, path: __sura_path_get, get_path: json_path, has_path: json_has_path, merge_patch: json_merge_patch, delete_path: json_delete_path, set_path: json_set_path, schema_validate, schema_errors, schema_to_json_schema, to_json_schema: schema_to_json_schema, pluck, count_by, group_by, sort_by, template_render, render: template_render, jsonl_parse, jsonl_stringify, sse_parse, sse_data, csv_parse, csv_stringify, ini_parse, ini_stringify };")
$js.Add("const dict = { keys: dict_keys, values: dict_values, items: dict_items, merge: dict_merge, pick: dict_pick, omit: dict_omit, get_path: dict_get_path };")
$js.Add("const os = { wait, sleep_ms, clock, cwd: () => __sura_process && __sura_process.cwd ? __sura_process.cwd() : '', argv, argc, script_name, env_get, env_require, env_set, env_load, home_dir, temp_dir, path_separator, name: os_name, os_name, is_windows, which, cmd_exists, cmd_quote, cmd_join, run: cmd_run, run_checked: cmd_run_checked };")
$js.Add("const cli = { parse: cli_parse, cli_parse, argv };")
$js.Add("const fs = { read: file_read, write: file_write, read_json: file_read_json, write_json: file_write_json, read_bytes: file_read_bytes, write_bytes: file_write_bytes, sha256: file_sha256, append: file_append, exists: file_exists, delete: file_delete, remove: file_delete, remove_tree: file_remove_tree, delete_tree: file_remove_tree, list: file_list, walk: file_walk, glob: file_glob, mkdir, cwd: () => __sura_process && __sura_process.cwd ? __sura_process.cwd() : '', join: path_join, basename: path_basename, dirname: path_dirname, ext: path_ext, stem: path_stem, normalize: path_normalize, abs: path_abs, relative: path_relative, is_dir: file_is_dir, is_file: file_is_file, info: file_info, size: file_size, copy: file_copy, move: file_move, lines: file_lines };")
$js.Add("const path = { join: path_join, basename: path_basename, dirname: path_dirname, ext: path_ext, stem: path_stem, normalize: path_normalize, abs: path_abs, relative: path_relative };")
$js.Add("const crypto = { sha256, file_sha256, hmac_sha256, file_hmac_sha256, random_bytes: crypto_random_bytes, random_hex: crypto_random_hex, constant_time_eq, hex_encode, hex_decode, base64_encode, base64_decode, base64_url_encode, base64_url_decode, url_encode, url_decode };")
$js.Add("const regex = { match: regex_match, replace: regex_replace, find_all: regex_find_all, escape: regex_escape, quote: regex_escape, capture: regex_capture, captures: regex_captures, split: regex_split, regex_match, regex_replace, regex_find_all, regex_escape, regex_capture, regex_captures, regex_split };")
$js.Add("const datetime = { now: datetime_now, parse: datetime_parse, format: datetime_format, utc_format: datetime_utc_format, parts: datetime_parts, add: datetime_add, diff: datetime_diff, timestamp };")
$js.Add("const log = { set_file: log_set_file, set_json: log_set_json, set_level: log_set_level, get_level: log_get_level, level: log_level, event: log_event, debug: log_debug, info: log_info, warn: log_warn, error: log_error };")
$js.Add("const console = { log: console_log, print: console_print, write: console_write, write_line: console_write_line, writeln: console_write_line, println: console_println, line: console_line, info: console_info, debug: console_debug, warn: console_warn, warning: console_warn, error: console_error, exception: console_exception, raw: console_raw, flush: console_flush, json: console_json, inspect: console_inspect, hrtime: console_hrtime, beep: console_beep, clear: console_clear, assert: console_assert, time: console_time, time_log: console_time_log, timeLog: console_time_log, time_end: console_time_end, timeEnd: console_time_end, time_stamp: console_time_stamp, timeStamp: console_time_stamp, count: console_count, count_reset: console_count_reset, countReset: console_count_reset, table: console_table, dir: console_dir, dirxml: console_dirxml, trace: console_trace, group: console_group, group_collapsed: console_group_collapsed, groupCollapsed: console_group_collapsed, group_end: console_group_end, groupEnd: console_group_end, profile: console_profile, profile_end: console_profile_end, profileEnd: console_profile_end, style: console_style, color: console_color, colour: console_color, strip_ansi: console_strip_ansi, stripAnsi: console_strip_ansi, set_color: console_set_color, setColor: console_set_color, set_colour: console_set_color, setColour: console_set_color, reset_color: console_reset_color, resetColor: console_reset_color, reset_colour: console_reset_color, resetColour: console_reset_color, is_tty: console_is_tty, isTTY: console_is_tty, width: console_width, height: console_height, size: console_size, status: console_status, input, read_line: console_read_line, readline: console_readline, readLine: console_read_line, prompt: console_prompt };")
$js.Add("const test = { assert, eq: assert_eq, ne: assert_ne, neq: assert_ne, contains: assert_contains, not_contains: assert_not_contains, match: assert_match, type: assert_type, len: assert_len, between: assert_between, approx: assert_approx, check, check_eq, check_match, summary: test_summary, report: test_report };")
$js.Add("Object.assign(crypto, { query_build, query_parse, form_build, form_parse, url_parse, url_build, auth_bearer, auth_basic, headers_merge, headers_get, headers_has, headers_redact, cookie_parse, cookie_build, cookie_get, content_type: http_content_type, charset: http_charset, is_json: http_is_json, status_ok: http_status_ok, status_text: http_status_text, status_retryable: http_status_retryable, retry_after: http_retry_after, backoff_delays: http_backoff_delays });")
$js.Add("const db = { set: db_set, get: db_get, has: db_has, delete: db_delete, keys: db_keys, all: db_all, insert: db_insert, find: db_find, count: db_count, update: db_update, remove: db_remove, query: db_query };")
$js.Add("const http = { get: http_get, json: http_json, post: http_post, request: http_request, request_full: http_request_full, request_retry: http_request_retry, request_json: http_request_json, request_json_checked: http_request_json_checked, request_retry_json: http_request_retry_json, request_retry_json_checked: http_request_retry_json_checked, serve_static: http_serve_static, serve_routes: http_serve_routes, server_url: http_server_url, server_stop: http_server_stop, url_parse, url_build, query_build, query_parse, form_build, form_parse, auth_bearer, auth_basic, headers_merge, headers_get, headers_has, headers_redact, cookie_parse, cookie_build, cookie_get, content_type: http_content_type, charset: http_charset, is_json: http_is_json, status_ok: http_status_ok, status_text: http_status_text, status_retryable: http_status_retryable, retry_after: http_retry_after, backoff_delays: http_backoff_delays };")
$js.Add("const async = { cmd: async_cmd, ready: async_ready, http_get: async_http_get, http_request: async_http_request, sleep: async_sleep, status: async_status, pending: async_pending, forget: async_forget, cleanup: async_cleanup, await: async_await, await_timeout: async_await_timeout, ready_all: async_ready_all, any: async_any, all: async_all, all_timeout: async_all_timeout };")
$js.Add("const python = { available: python_available, executable: python_executable, eval: python_eval, call: python_call, call_json: python_call_json };")
$js.Add("const ffi = { load: ffi_load, call: ffi_call };")
$js.Add("const plugin = { load: plugin_load, load_manifest: plugin_load_manifest, call: plugin_call, info: plugin_info, unload: plugin_unload };")
$js.Add("const vector = { add: vector_add, dot: vector_dot, scale: vector_scale, norm: vector_norm, cosine: vector_cosine, normalize: vector_normalize, search: vector_search, vec3, vector3, add3: vec3_add, sub3: vec3_sub, dot3: vec3_dot, cross: vec3_cross, scale3: vec3_scale, norm3: vec3_norm, normalize3: vec3_normalize, distance3: vec3_distance, neg3: vec3_neg, lerp3: vec3_lerp, midpoint3: vec3_midpoint, project3: vec3_project, reject3: vec3_reject, reflect3: vec3_reflect, angle3: vec3_angle, transform4: vec3_transform4, vec3_add, vec3_sub, vec3_dot, vec3_cross, vec3_scale, vec3_norm, vec3_normalize, vec3_distance, vec3_neg, vec3_lerp, vec3_midpoint, vec3_project, vec3_reject, vec3_reflect, vec3_angle, vec3_transform4 };")
$js.Add("const graphics3d = { identity: mat4_identity, mat4_identity, translate: mat4_translate, mat4_translate, scale: mat4_scale, mat4_scale, rotate_y: mat4_rotate_y, mat4_rotate_y, mul: mat4_mul, mat4_mul, cube: mesh_cube, mesh_cube, transform: mesh_transform4, mesh_transform4, bounds: mesh_bounds, mesh_bounds, face_normals: mesh_face_normals, mesh_face_normals, project: camera_project, camera_project };")
$js.Add("const g3d = graphics3d;")
$js.Add("const rag = { context: rag_context, sources: rag_sources, prepare: rag_prepare, messages: rag_messages };")
$js.Add("const tensor = { shape: tensor_shape, zeros: tensor_zeros, fill: tensor_fill, add: tensor_add, mul: tensor_mul, clip: tensor_clip, flatten: tensor_flatten, sum: tensor_sum, mean: tensor_mean, variance: tensor_variance, std: tensor_std, min: tensor_min, max: tensor_max, argmin: tensor_argmin, argmax: tensor_argmax, zscore: tensor_zscore, softmax: tensor_softmax, transpose: tensor_transpose, matmul: tensor_matmul };")
$js.Add("const tool = { call: tool_call, spec: tool_spec, validate: tool_validate, schema: tool_schema, allowed: tool_allowed, call_policy: tool_call_policy, list: tool_list };")
$js.Add("const llm = { message: llm_message, messages: llm_messages, rag_messages, request: llm_request, request_json: llm_request_json, response_schema: llm_response_schema, request_schema: llm_request_schema, request_schema_json: llm_request_schema_json, tools: llm_tools, tool_schemas: llm_tool_schemas, request_tools: llm_request_tools, request_tools_json: llm_request_tools_json, request_tools_schema: llm_request_tools_schema, request_tools_schema_json: llm_request_tools_schema_json, extract_text: llm_extract_text, extract_json: llm_extract_json, usage: llm_usage, cost: llm_cost, budget: llm_budget, chat: llm_chat, chat_request: llm_chat_request, stream_text: llm_stream_text, tool_calls: llm_tool_calls, tool_result: llm_tool_result, run_tools: llm_run_tools, next_messages: llm_next_messages, next_request: llm_next_request, next_request_json: llm_next_request_json, next_schema_request: llm_next_schema_request, next_schema_request_json: llm_next_schema_request_json };")
$js.Add("const stream = { from: stream_from, next: stream_next, take: stream_take, batch: stream_batch, map: stream_map, filter: stream_filter, window: stream_window, skip: stream_skip, count: stream_count, collect: stream_collect, join: stream_join, sum: stream_sum, avg: stream_avg, lines: stream_lines };")
$js.Add("")

function Is-JsPredefinedName {
    param([string]$Name)
    return $Name -in @(
        "nil", "true", "false", "this", "super", "Math", "JSON", "Array", "Object", "String", "Number",
        "array", "set", "math", "string", "json", "dict", "os", "random", "fs", "path", "cli", "crypto",
        "regex", "datetime", "log", "console", "test", "db", "http", "async", "python", "ffi", "plugin", "vector",
        "graphics3d", "g3d", "rag", "tensor", "tool", "llm", "stream"
    )
}

function Get-JsRecoveredCommentStatement {
    param([string]$Text)
    $t = $Text.Trim()
    if (-not ($t.StartsWith("#") -or $t.StartsWith("//"))) { return $null }
    foreach ($pattern in @(
        '([A-Za-z_][A-Za-z0-9_]*\s*(?::\s*[^=]+?)?\s+is\s+.+)$',
        '([A-Za-z_][A-Za-z0-9_]*(?:\[[^\]]+\]|\.[A-Za-z_][A-Za-z0-9_]*)*\s*(?:\+=|-=|\*=|/=|%=)\s*.+)$'
    )) {
        if ($t -match $pattern) { return $Matches[1].Trim() }
    }
    return $null
}

function As-JsAstArray {
    param($Items)
    if ($null -eq $Items) { return @() }
    if ($Items -is [System.Array]) { return @($Items) }
    if ($Items -is [System.Collections.IEnumerable] -and -not ($Items -is [string]) -and -not ($Items -is [pscustomobject])) {
        return @($Items)
    }
    return @($Items)
}

function Read-JsAstReport {
    param([string]$Path)
    $astText = [System.IO.File]::ReadAllText((Resolve-Path -LiteralPath $Path).Path, [System.Text.Encoding]::UTF8)
    $astReport = $astText | ConvertFrom-Json
    if ($astReport.schema -ne "sura.ast.v1") {
        throw "unsupported AST JSON schema: $($astReport.schema)"
    }
    return $astReport
}

function Export-JsAstReport {
    param([string]$SourcePath, [string]$EnginePath)
    $tmp = Join-Path ([System.IO.Path]::GetTempPath()) ("sura_js_ast_import_" + [System.Guid]::NewGuid().ToString("N") + ".json")
    & $EnginePath --ast-json --out $tmp $SourcePath | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "AST JSON export failed for JS import: $SourcePath"
    }
    return Read-JsAstReport $tmp
}

function Expand-JsAstImports {
    param(
        $AstReport,
        [Parameter(Mandatory=$true)][string]$AstJsonPath,
        [Parameter(Mandatory=$true)][hashtable]$State,
        [string]$EnginePath = ""
    )

    $sourcePath = [string]$AstReport.source
    $basePath = if (-not [string]::IsNullOrWhiteSpace($sourcePath) -and (Test-Path -LiteralPath $sourcePath)) {
        (Resolve-Path -LiteralPath $sourcePath).Path
    } else {
        (Resolve-Path -LiteralPath $AstJsonPath).Path
    }
    if ($State.ContainsKey($basePath)) {
        if ($State[$basePath] -eq "active") {
            throw "cyclic JS AST import: $basePath"
        }
        return [pscustomobject]@{ node = "SuraBlock"; line = 0; body = @() }
    }

    $State[$basePath] = "active"
    $body = New-Object System.Collections.Generic.List[object]
    foreach ($stmt in @(As-JsAstArray ($AstReport.ast.body))) {
        if ([string]$stmt.node -eq "IMPORT") {
            if ([string]::IsNullOrWhiteSpace($EnginePath)) {
                $EnginePath = Resolve-JsEngine $Engine
            }
            $importPath = Unescape-SuraImportPath ([string]$stmt.path)
            $importResolved = Resolve-JsImportPath -ImportPath $importPath -ImporterPath $basePath
            $childReport = Export-JsAstReport -SourcePath $importResolved -EnginePath $EnginePath
            $childBlock = Expand-JsAstImports -AstReport $childReport -AstJsonPath $importResolved -State $State -EnginePath $EnginePath
            foreach ($child in @(As-JsAstArray ($childBlock.body))) {
                $body.Add($child) | Out-Null
            }
        } else {
            $body.Add($stmt) | Out-Null
        }
    }
    $State[$basePath] = "done"
    return [pscustomobject]@{ node = "SuraBlock"; line = $AstReport.ast.line; body = @($body.ToArray()) }
}

function Convert-JsAstStringValue {
    param($Value)
    if ($null -eq $Value) { return "null" }
    return ($Value | ConvertTo-Json -Compress)
}

function Convert-JsAstTemplateText {
    param([string]$Text)
    return $Text.Replace('\', '\\').Replace('`', '\`').Replace('${', '\${')
}

function Convert-JsAstStringOrInterpolation {
    param($Value)
    if ($null -eq $Value) { return "null" }
    $text = [string]$Value
    if ($text.IndexOf('{') -lt 0 -or $text.IndexOf('}') -lt 0) {
        return Convert-JsAstStringValue $Value
    }

    $parts = New-Object System.Collections.Generic.List[string]
    $cursor = 0
    $converted = 0
    while ($cursor -lt $text.Length) {
        $open = $text.IndexOf('{', $cursor)
        if ($open -lt 0) {
            $parts.Add((Convert-JsAstTemplateText $text.Substring($cursor))) | Out-Null
            break
        }
        $close = $text.IndexOf('}', $open + 1)
        if ($close -lt 0) {
            return Convert-JsAstStringValue $Value
        }

        if ($open -gt $cursor) {
            $parts.Add((Convert-JsAstTemplateText $text.Substring($cursor, $open - $cursor))) | Out-Null
        }

        $exprText = $text.Substring($open + 1, $close - $open - 1).Trim()
        if ([string]::IsNullOrWhiteSpace($exprText)) {
            $parts.Add((Convert-JsAstTemplateText $text.Substring($open, $close - $open + 1))) | Out-Null
        } elseif ($exprText -notmatch '[A-Za-z_]') {
            $parts.Add((Convert-JsAstTemplateText $text.Substring($open, $close - $open + 1))) | Out-Null
        } else {
            try {
                $exprForJs = $exprText.Replace('\"', '"')
                $parts.Add('${' + (Convert-SuraExpression $exprForJs) + '}') | Out-Null
                $converted++
            } catch {
                return Convert-JsAstStringValue $Value
            }
        }
        $cursor = $close + 1
    }

    if ($converted -eq 0) {
        return Convert-JsAstStringValue $Value
    }
    return '`' + [string]::Join("", [string[]]$parts.ToArray()) + '`'
}

function Convert-JsAstName {
    param([string]$Name)
    if ($Name -eq "self") { return "this" }
    return $Name
}

function Get-JsAstClassInternalName {
    param([string]$Name)
    return "__SuraAstClass_$Name"
}

function Resolve-JsAstClassConstructorName {
    param([string]$Name)
    if ($script:jsAstClassInternalNames.ContainsKey($Name)) {
        return [string]$script:jsAstClassInternalNames[$Name]
    }
    return $Name
}

function Convert-JsAstParams {
    param($Params, $Defaults)
    $names = @(As-JsAstArray $Params)
    $defaultsArray = @(As-JsAstArray $Defaults)
    $out = New-Object System.Collections.Generic.List[string]
    for ($i = 0; $i -lt $names.Count; $i++) {
        $name = [string]$names[$i]
        $default = if ($i -lt $defaultsArray.Count) { $defaultsArray[$i] } else { $null }
        if ($null -ne $default) {
            $out.Add("$name = $(Convert-JsAstExpr $default)") | Out-Null
        } else {
            $out.Add($name) | Out-Null
        }
    }
    return [string]::Join(", ", [string[]]$out.ToArray())
}

function Convert-JsAstArgs {
    param($InputArgs)
    $out = New-Object System.Collections.Generic.List[string]
    foreach ($arg in @(As-JsAstArray $InputArgs)) {
        if ($null -ne $arg) { $out.Add((Convert-JsAstExpr $arg)) | Out-Null }
    }
    return [string]::Join(", ", [string[]]$out.ToArray())
}

function Convert-JsAstExpr {
    param($Node)
    if ($null -eq $Node) { return "null" }

    switch ([string]$Node.node) {
        "NUM_LIT" {
            return [Convert]::ToString($Node.value, [System.Globalization.CultureInfo]::InvariantCulture)
        }
        "STR_LIT" {
            return Convert-JsAstStringOrInterpolation ($Node.value)
        }
        "BOOL_LIT" {
            if ([bool]$Node.value) { return "true" }
            return "false"
        }
        "NIL_LIT" {
            return "null"
        }
        "IDENT" {
            return Convert-JsAstName ([string]$Node.name)
        }
        "BIN_OP" {
            $op = [string]$Node.op
            if ($op -eq "+") {
                return "__sura_add($(Convert-JsAstExpr ($Node.left)), $(Convert-JsAstExpr ($Node.right)))"
            }
            if ($op -eq "in") {
                return "__sura_in($(Convert-JsAstExpr ($Node.left)), $(Convert-JsAstExpr ($Node.right)))"
            }
            if ($op -eq "and") { $op = "&&" }
            elseif ($op -eq "or") { $op = "||" }
            return "($(Convert-JsAstExpr ($Node.left)) $op $(Convert-JsAstExpr ($Node.right)))"
        }
        "UNARY_OP" {
            $op = [string]$Node.op
            if ($op -eq "not") { $op = "!" }
            return "($op$(Convert-JsAstExpr ($Node.operand)))"
        }
        "DOT_ACCESS" {
            $op = if ([bool]$Node.optional) { "?." } else { "." }
            return "$(Convert-JsAstExpr ($Node.obj))$op$($Node.prop)"
        }
        "INDEX" {
            return "$(Convert-JsAstExpr ($Node.obj))[$(Convert-JsAstExpr ($Node.key))]"
        }
        "CALL" {
            return "$($Node.name)($(Convert-JsAstArgs ($Node.args)))"
        }
        "METHOD_CALL" {
            $obj = Convert-JsAstExpr ($Node.obj)
            $method = [string]$Node.method
            $args = Convert-JsAstArgs ($Node.args)
            $callOp = if ([bool]$Node.optional) { "?." } else { "." }
            $moduleReceiver = $false
            if ($null -ne $Node.obj -and [string]$Node.obj.node -eq "IDENT") {
                $receiverName = [string]$Node.obj.name
                $moduleReceiver = $receiverName -in @("array", "string", "dict", "set", "math", "path", "os", "cli", "json", "fs", "regex", "datetime", "crypto", "db", "log", "console", "http", "async", "test", "random", "python", "ffi", "plugin", "vector", "graphics3d", "g3d", "rag", "tensor", "stream", "tool", "llm")
            }
            if (-not $moduleReceiver -and $method -in @("len", "length", "size") -and [string]::IsNullOrWhiteSpace($args)) { return "$obj.length" }
            if ($method -eq "upper" -and [string]::IsNullOrWhiteSpace($args)) { return "$obj.toUpperCase()" }
            if ($method -eq "lower" -and [string]::IsNullOrWhiteSpace($args)) { return "$obj.toLowerCase()" }
            if ($method -eq "contains") { return "$obj$($callOp)includes($args)" }
            if ($method -eq "index_of") { return "$obj$($callOp)indexOf($args)" }
            if ($method -eq "starts_with") { return "$obj$($callOp)startsWith($args)" }
            if ($method -eq "ends_with") { return "$obj$($callOp)endsWith($args)" }
            if ($method -eq "sub") { return "$obj$($callOp)slice($args)" }
            return "$obj$callOp$method($args)"
        }
        "SUPER_CALL" {
            $args = Convert-JsAstArgs ($Node.args)
            if ([string]$Node.method -eq "init") { return "super($args)" }
            return "super.$($Node.method)($args)"
        }
        "ARRAY_LIT" {
            $items = New-Object System.Collections.Generic.List[string]
            foreach ($item in @(As-JsAstArray ($Node.elements))) {
                $items.Add((Convert-JsAstExpr $item)) | Out-Null
            }
            return "[" + [string]::Join(", ", [string[]]$items.ToArray()) + "]"
        }
        "DICT_LIT" {
            $items = New-Object System.Collections.Generic.List[string]
            foreach ($entry in @(As-JsAstArray ($Node.entries))) {
                $items.Add("$(Convert-JsAstStringValue ($entry.key)): $(Convert-JsAstExpr ($entry.value))") | Out-Null
            }
            return "{" + [string]::Join(", ", [string[]]$items.ToArray()) + "}"
        }
        "NEW_EXPR" {
            return "new $(Resolve-JsAstClassConstructorName ([string]$Node.class_name))($(Convert-JsAstArgs ($Node.args)))"
        }
        "TERNARY" {
            return "($(Convert-JsAstExpr ($Node.cond)) ? $(Convert-JsAstExpr ($Node.then)) : $(Convert-JsAstExpr ($Node.else)))"
        }
        "FUNC_EXPR" {
            $bodyLines = New-Object System.Collections.Generic.List[string]
            Add-JsAstStmt ($Node.body) $bodyLines "  "
            return "function($(Convert-JsAstParams ($Node.params) ($Node.defaults))) {`n$([string]::Join([Environment]::NewLine, [string[]]$bodyLines.ToArray()))`n}"
        }
        "STR_INTERP" {
            $parts = New-Object System.Collections.Generic.List[string]
            foreach ($part in @(As-JsAstArray ($Node.parts))) {
                if ($null -ne $part -and [string]$part.node -eq "STR_LIT") {
                    $text = Convert-JsAstTemplateText ([string]$part.value)
                    $parts.Add($text) | Out-Null
                } else {
                    $parts.Add('${' + (Convert-JsAstExpr $part) + '}') | Out-Null
                }
            }
            return '`' + [string]::Join("", [string[]]$parts.ToArray()) + '`'
        }
        default {
            throw "unsupported AST JSON expression node: $($Node.node)"
        }
    }
}

function Add-JsAstStmt {
    param(
        $Node,
        [Parameter(Mandatory=$true)]$Out,
        [string]$Indent = ""
    )
    if ($null -eq $Node) { return }

    switch ([string]$Node.node) {
        "SuraBlock" {
            foreach ($stmt in @(As-JsAstArray ($Node.body))) {
                Add-JsAstStmt $stmt $Out $Indent
            }
        }
        "USE" {
            $Out.Add("${Indent}// use $($Node.lib)") | Out-Null
        }
        "IMPORT" {
            $Out.Add("${Indent}// import $($Node.path)") | Out-Null
        }
        "GLOBAL_DECL" {
            $Out.Add("${Indent}// global $(($Node.names -join ', '))") | Out-Null
        }
        "ASSIGN" {
            $name = [string]$Node.name
            $expr = Convert-JsAstExpr ($Node.value)
            if ($script:declared.ContainsKey($name)) {
                $Out.Add("${Indent}$name = $expr;") | Out-Null
            } else {
                $script:declared[$name] = $true
                $Out.Add("${Indent}var $name = $expr;") | Out-Null
            }
        }
        "IN_PLACE" {
            $Out.Add("${Indent}$($Node.name) $($Node.op)= $(Convert-JsAstExpr ($Node.value));") | Out-Null
        }
        "DOT_ASSIGN" {
            $Out.Add("${Indent}$(Convert-JsAstName ([string]$Node.obj_name)).$($Node.prop) = $(Convert-JsAstExpr ($Node.value));") | Out-Null
        }
        "INDEX_ASSIGN" {
            $Out.Add("${Indent}$($Node.name)[$(Convert-JsAstExpr ($Node.key))] = $(Convert-JsAstExpr ($Node.value));") | Out-Null
        }
        "EXPR_STMT" {
            $Out.Add("${Indent}$(Convert-JsAstExpr ($Node.expr));") | Out-Null
        }
        "CMD" {
            $cmd = [string]$Node.cmd
            if ($cmd -eq "random" -and @(As-JsAstArray ($Node.args)).Count -ge 2) {
                $Out.Add("${Indent}random($(Convert-JsAstArgs ($Node.args)));") | Out-Null
            } else {
                $Out.Add("${Indent}$cmd($(Convert-JsAstArgs ($Node.args)));") | Out-Null
            }
        }
        "RETURN" {
            if ($null -eq $Node.value) { $Out.Add("${Indent}return;") | Out-Null }
            else { $Out.Add("${Indent}return $(Convert-JsAstExpr ($Node.value));") | Out-Null }
        }
        "BREAK" {
            $Out.Add("${Indent}break;") | Out-Null
        }
        "CONTINUE" {
            $Out.Add("${Indent}continue;") | Out-Null
        }
        "THROW" {
            if ($null -eq $Node.msg) { $Out.Add("${Indent}throw null;") | Out-Null }
            else { $Out.Add("${Indent}throw $(Convert-JsAstExpr ($Node.msg));") | Out-Null }
        }
        "IF" {
            $Out.Add("${Indent}if ($(Convert-JsAstExpr ($Node.cond))) {") | Out-Null
            Add-JsAstStmt ($Node.then_block) $Out ($Indent + "  ")
            if ($null -ne $Node.else_block) {
                $Out.Add("${Indent}} else {") | Out-Null
                Add-JsAstStmt ($Node.else_block) $Out ($Indent + "  ")
            }
            $Out.Add("${Indent}}") | Out-Null
        }
        "WHILE" {
            $Out.Add("${Indent}while ($(Convert-JsAstExpr ($Node.cond))) {") | Out-Null
            Add-JsAstStmt ($Node.body) $Out ($Indent + "  ")
            $Out.Add("${Indent}}") | Out-Null
        }
        "REPEAT" {
            $loopVar = "__sura_ast_i$script:loopCounter"
            $limitVar = "__sura_ast_repeat_limit$script:loopCounter"
            $script:loopCounter++
            $Out.Add("${Indent}for (let $loopVar = 0, $limitVar = $(Convert-JsAstExpr ($Node.count)); $loopVar < $limitVar; $loopVar++) {") | Out-Null
            Add-JsAstStmt ($Node.body) $Out ($Indent + "  ")
            $Out.Add("${Indent}}") | Out-Null
        }
        "FOR" {
            $name = [string]$Node.var
            $script:declared[$name] = $true
            $endVar = "__sura_ast_for_end$script:loopCounter"
            $stepVar = "__sura_ast_for_step$script:loopCounter"
            $script:loopCounter++
            $stepExpr = if ($null -eq $Node.step) { "1" } else { Convert-JsAstExpr ($Node.step) }
            $Out.Add("${Indent}for (let $name = $(Convert-JsAstExpr ($Node.from)), $endVar = $(Convert-JsAstExpr ($Node.to)), $stepVar = __sura_for_step($stepExpr); $stepVar > 0 ? $name <= $endVar : $name >= $endVar; $name += $stepVar) {") | Out-Null
            Add-JsAstStmt ($Node.body) $Out ($Indent + "  ")
            $Out.Add("${Indent}}") | Out-Null
        }
        "FOREACH" {
            $name = [string]$Node.var
            $name2 = [string]$Node.var2
            $script:declared[$name] = $true
            if ([string]::IsNullOrWhiteSpace($name2)) {
                $Out.Add("${Indent}for (const $name of __sura_iter($(Convert-JsAstExpr ($Node.collection)))) {") | Out-Null
            } else {
                $itemsVar = "__sura_ast_for_items$script:loopCounter"
                $entriesVar = "__sura_ast_for_entries$script:loopCounter"
                $script:loopCounter++
                $script:declared[$name2] = $true
                $Out.Add("${Indent}const $itemsVar = $(Convert-JsAstExpr ($Node.collection));") | Out-Null
                $Out.Add("${Indent}const $entriesVar = __sura_entries($itemsVar);") | Out-Null
                $Out.Add("${Indent}for (const [$name, $name2] of $entriesVar) {") | Out-Null
            }
            Add-JsAstStmt ($Node.body) $Out ($Indent + "  ")
            $Out.Add("${Indent}}") | Out-Null
        }
        "FUNC_DEF" {
            $name = [string]$Node.name
            $script:declared[$name] = $true
            $Out.Add("${Indent}function $name($(Convert-JsAstParams ($Node.params) ($Node.defaults))) {") | Out-Null
            Add-JsAstStmt ($Node.body) $Out ($Indent + "  ")
            $Out.Add("${Indent}}") | Out-Null
        }
        "CLASS_DEF" {
            $name = [string]$Node.name
            $parent = [string]$Node.parent
            $internalName = Get-JsAstClassInternalName $name
            $script:jsAstClassInternalNames[$name] = $internalName
            $script:declared[$name] = $true
            $script:declared[$internalName] = $true
            $parentInternal = Resolve-JsAstClassConstructorName $parent
            $header = if ([string]::IsNullOrWhiteSpace($parent)) { "class $internalName {" } else { "class $internalName extends $parentInternal {" }
            $Out.Add("${Indent}$header") | Out-Null
            $methods = @(As-JsAstArray ($Node.methods))
            $hasConstructor = $false
            foreach ($method in $methods) {
                $methodName = [string]$method.name
                if ($methodName -eq "init") {
                    $methodName = "constructor"
                    $hasConstructor = $true
                }
                $Out.Add("$Indent  $methodName($(Convert-JsAstParams ($method.params) ($method.defaults))) {") | Out-Null
                Add-JsAstStmt ($method.body) $Out ($Indent + "    ")
                $Out.Add("$Indent  }") | Out-Null
            }
            $fields = @(As-JsAstArray ($Node.fields))
            if (-not $hasConstructor -and $fields.Count -gt 0) {
                $Out.Add("$Indent  constructor() {") | Out-Null
                foreach ($field in $fields) {
                    $default = if ($null -eq $field.default) { "null" } else { Convert-JsAstExpr ($field.default) }
                    $Out.Add("$Indent    this.$($field.name) = $default;") | Out-Null
                }
                $Out.Add("$Indent  }") | Out-Null
            }
            $Out.Add("${Indent}}") | Out-Null
            $Out.Add("${Indent}function $name(...args) { return new $internalName(...args); }") | Out-Null
        }
        "ENUM_DEF" {
            $script:declared[[string]$Node.name] = $true
            $Out.Add("${Indent}const $($Node.name) = {") | Out-Null
            foreach ($member in @(As-JsAstArray ($Node.members))) {
                $value = if ($null -eq $member.value) { Convert-JsAstStringValue ($member.name) } else { Convert-JsAstExpr ($member.value) }
                $Out.Add("$Indent  $($member.name): $value,") | Out-Null
            }
            $Out.Add("${Indent}};") | Out-Null
        }
        "MATCH" {
            $id = $script:jsMatchCounter
            $subject = "__sura_ast_match$id"
            $matched = "__sura_ast_match_matched$id"
            $script:jsMatchCounter++
            $Out.Add("${Indent}{") | Out-Null
            $Out.Add("$Indent  const $subject = $(Convert-JsAstExpr ($Node.subject));") | Out-Null
            $Out.Add("$Indent  let $matched = false;") | Out-Null
            foreach ($arm in @(As-JsAstArray ($Node.arms))) {
                $condition = if ([bool]$arm.is_wildcard) {
                    "!$matched"
                } elseif ([bool]$arm.is_range) {
                    "(!$matched && $subject >= $(Convert-JsAstExpr ($arm.pattern)) && $subject <= $(Convert-JsAstExpr ($arm.range_end)))"
                } else {
                    "(!$matched && __eq($subject, $(Convert-JsAstExpr ($arm.pattern))))"
                }
                $Out.Add("$Indent  if ($condition) {") | Out-Null
                if (-not [bool]$arm.is_wildcard) {
                    $Out.Add("$Indent    $matched = true;") | Out-Null
                }
                Add-JsAstStmt ($arm.body) $Out ($Indent + "    ")
                $Out.Add("$Indent  }") | Out-Null
            }
            $Out.Add("${Indent}}") | Out-Null
        }
        "TRY" {
            $catchName = if ([string]::IsNullOrWhiteSpace([string]$Node.catch_var)) { "__sura_error" } else { [string]$Node.catch_var }
            $script:declared[$catchName] = $true
            $Out.Add("${Indent}try {") | Out-Null
            Add-JsAstStmt ($Node.try_block) $Out ($Indent + "  ")
            if ($null -ne $Node.catch_block) {
                $Out.Add("${Indent}} catch ($catchName) {") | Out-Null
                Add-JsAstStmt ($Node.catch_block) $Out ($Indent + "  ")
            }
            if ($null -ne $Node.finally_block) {
                $Out.Add("${Indent}} finally {") | Out-Null
                Add-JsAstStmt ($Node.finally_block) $Out ($Indent + "  ")
            }
            $Out.Add("${Indent}}") | Out-Null
        }
        default {
            throw "unsupported AST JSON statement node: $($Node.node)"
        }
    }
}

if ($AstJson) {
    $astJsonPath = (Resolve-Path -LiteralPath $Source).Path
    $astReport = Read-JsAstReport $astJsonPath
    $expandedAst = Expand-JsAstImports -AstReport $astReport -AstJsonPath $astJsonPath -State @{} -EnginePath ""
    $js.Add("// AST JSON input: sura.ast.v1")
    Add-JsAstStmt $expandedAst $js ""
    Set-Content -LiteralPath $Out -Value ($js -join [Environment]::NewLine) -Encoding UTF8
    Write-Host "[OK] wrote $Out"
    return
}

$rawSourceLines = @(Expand-JsSourceLines -Path $Source -State @{})
$optionalRoots = [ordered]@{}
foreach ($raw in $rawSourceLines) {
    foreach ($m in [regex]::Matches($raw, '\b([A-Za-z_][A-Za-z0-9_]*)\?\.')) {
        $name = $m.Groups[1].Value
        if (-not (Is-JsPredefinedName $name) -and -not $script:declared.ContainsKey($name)) {
            $optionalRoots[$name] = $true
        }
    }
}
foreach ($name in $optionalRoots.Keys) {
    $script:declared[$name] = $true
    $js.Add("var $name = null;")
}
if ($optionalRoots.Count -gt 0) { $js.Add("") }

foreach ($raw in $rawSourceLines) {
    $t = $raw.Trim()
    if ($t -eq "") { $js.Add(""); continue }
    if ($t.StartsWith("#") -or $t.StartsWith("//")) {
        $js.Add("// " + $t.TrimStart("#").TrimStart("/").Trim())
        $recovered = Get-JsRecoveredCommentStatement $t
        if (-not $recovered) { continue }
        $t = $recovered
    }

    try {
        if ($t -match '^use\s+[A-Za-z_][A-Za-z0-9_]*$') {
            $js.Add("// $t")
        } elseif ($t -match '^global\s+[A-Za-z_][A-Za-z0-9_]*(?:\s*,\s*[A-Za-z_][A-Za-z0-9_]*)*$') {
            $js.Add("// $t")
        } elseif ($t -eq "end") {
            if ((Get-JsBlockTop) -eq "when-arm") {
                $js.Add("}")
                Pop-JsBlock | Out-Null
                if ((Get-JsBlockTop) -eq "when") {
                    $js.Add("}")
                    Pop-JsBlock | Out-Null
                    if ($script:jsWhenStack.Count -gt 0) {
                        $script:jsWhenStack.RemoveAt($script:jsWhenStack.Count - 1)
                    }
                }
            } elseif ((Get-JsBlockTop) -eq "match-arm") {
                $js.Add("}")
                Pop-JsBlock | Out-Null
                if ((Get-JsBlockTop) -eq "match") {
                    $js.Add("}")
                    Pop-JsBlock | Out-Null
                    if ($script:jsMatchStack.Count -gt 0) {
                        $script:jsMatchStack.RemoveAt($script:jsMatchStack.Count - 1)
                    }
                }
            } elseif ((Get-JsBlockTop) -eq "enum") {
                $js.Add("};")
                Pop-JsBlock | Out-Null
            } elseif ((Get-JsBlockTop) -eq "func-expr-assign") {
                $js.Add("};")
                Pop-JsBlock | Out-Null
            } elseif ((Get-JsBlockTop) -eq "struct") {
                $structInfo = Get-JsStructTop
                if ($null -ne $structInfo -and -not $structInfo.UserInit) {
                    foreach ($line in (Add-JsStructAutoConstructor -StructInfo $structInfo)) {
                        $js.Add($line)
                    }
                }
                $js.Add("}")
                if ($script:jsStructStack.Count -gt 0) {
                    $script:jsStructStack.RemoveAt($script:jsStructStack.Count - 1)
                }
                Pop-JsBlock | Out-Null
                if ($null -ne $structInfo) {
                    $js.Add("function $($structInfo.Name)(...args) { return new $($structInfo.InternalName)(...args); }")
                }
            } else {
                $js.Add("}")
                Pop-JsBlock | Out-Null
            }
        } elseif ((Get-JsBlockTop) -eq "enum") {
            if ($t -notmatch '^([A-Za-z_][A-Za-z0-9_]*)(?:\s+is\s+(.+))?,?$') {
                throw "unsupported enum member: $t"
            }
            $memberName = $Matches[1]
            $memberValue = if ($Matches[2]) { Convert-SuraExpression $Matches[2] } else { '"' + $memberName + '"' }
            $js.Add("  ${memberName}: $memberValue,")
        } elseif ((Get-JsBlockTop) -eq "struct" -and $t -match '^([A-Za-z_][A-Za-z0-9_]*)(?:\s+is\s+(.+))?,?$') {
            $structInfo = Get-JsStructTop
            if ($null -eq $structInfo) { throw "struct field outside struct block" }
            $structInfo.Fields.Add([pscustomobject]@{ Name = $Matches[1]; Default = $Matches[2] }) | Out-Null
        } elseif ($t -match '^else\s+then(?:\s+(.+))?$') {
            $elseInline = $Matches[1]
            if ($null -ne (Get-JsWhenTop)) {
                foreach ($line in (Start-JsWhenArm -Condition "true" -Else)) {
                    $js.Add($line)
                }
                if ($elseInline) {
                    $js.Add((Convert-SimpleStatement $elseInline -Inline))
                }
            } elseif ($elseInline) {
                $js.Add("} else { $(Convert-SimpleStatement $elseInline -Inline)")
            } else {
                $js.Add("} else {")
            }
        } elseif ($t -eq "else") {
            $js.Add("} else {")
        } elseif ($t -match '^elif\s+(.+?)\s+then(?:\s+(.+))?$') {
            $elifCond = $Matches[1]
            $elifInline = $Matches[2]
            if ($elifInline) {
                $js.Add("} else if ($(Convert-SuraExpression $elifCond)) { $(Convert-SimpleStatement $elifInline -Inline)")
            } else {
                $js.Add("} else if ($(Convert-SuraExpression $elifCond)) {")
            }
        } elseif ($t -match '^match\s+(.+)$') {
            $id = $script:jsMatchCounter
            $subject = "__sura_match$id"
            $matched = "__sura_match_matched$id"
            $script:jsMatchCounter++
            $js.Add("{")
            $js.Add("const $subject = $(Convert-SuraExpression $Matches[1]);")
            $js.Add("let $matched = false;")
            Push-JsBlock "match"
            $script:jsMatchStack.Add([pscustomobject]@{ Subject = $subject; Matched = $matched; Arms = 0; HasWildcard = $false }) | Out-Null
        } elseif ($t -match '^when\s+(.+?)\s+then(?:\s+(.+))?$') {
            $matchTop = Get-JsMatchTop
            if ($null -eq $matchTop) { throw "when arm outside match block" }
            $pattern = $Matches[1].Trim()
            $inline = $Matches[2]
            $isElse = ($pattern -eq "_")
            $condition = $(if ($isElse) { "true" } else { "__eq($($matchTop.Subject), $(Convert-SuraExpression $pattern))" })
            foreach ($line in (Start-JsMatchArm -Condition $condition -Else:$isElse)) {
                $js.Add($line)
            }
            if ($inline) {
                $js.Add((Convert-SimpleStatement $inline -Inline))
            }
        } elseif ($null -ne (Get-JsMatchTop) -and $t -match '^when\s+(.+?)$' -and $t -notmatch '\s+do$') {
            $matchTop = Get-JsMatchTop
            $pattern = $Matches[1].Trim()
            $isElse = ($pattern -eq "_")
            $condition = $(if ($isElse) { "true" } else { "__eq($($matchTop.Subject), $(Convert-SuraExpression $pattern))" })
            foreach ($line in (Start-JsMatchArm -Condition $condition -Else:$isElse)) {
                $js.Add($line)
            }
        } elseif ($t -match '^when\s+(.+)\s+do$') {
            $id = $script:jsWhenCounter
            $subject = "__sura_when$id"
            $matched = "__sura_when_matched$id"
            $script:jsWhenCounter++
            $js.Add("{")
            $js.Add("const $subject = $(Convert-SuraExpression $Matches[1]);")
            $js.Add("let $matched = false;")
            Push-JsBlock "when"
            $script:jsWhenStack.Add([pscustomobject]@{ Subject = $subject; Matched = $matched; Arms = 0; HasElse = $false }) | Out-Null
        } elseif ($t -match '^is\s+(.+)\s+then(?:\s+(.+))?$') {
            $when = Get-JsWhenTop
            if ($null -eq $when) { throw "is arm outside when block" }
            foreach ($line in (Start-JsWhenArm -Condition "__eq($($when.Subject), $(Convert-SuraExpression $Matches[1]))")) {
                $js.Add($line)
            }
            if ($Matches[2]) {
                $js.Add((Convert-SimpleStatement $Matches[2] -Inline))
            }
        } elseif ($t -match '^in\s+(.+)\s+(?:to|~)\s+(.+)\s+then(?:\s+(.+))?$') {
            $when = Get-JsWhenTop
            if ($null -eq $when) { throw "in arm outside when block" }
            $subject = $when.Subject
            $condition = "($subject >= $(Convert-SuraExpression $Matches[1]) && $subject <= $(Convert-SuraExpression $Matches[2]))"
            foreach ($line in (Start-JsWhenArm -Condition $condition)) {
                $js.Add($line)
            }
            if ($Matches[3]) {
                $js.Add((Convert-SimpleStatement $Matches[3] -Inline))
            }
        } elseif ($t -eq "try") {
            $js.Add("try {")
            Push-JsBlock "try"
        } elseif ($t -match '^catch(?:\s+([A-Za-z_][A-Za-z0-9_]*))?$') {
            $errName = $(if ($Matches[1]) { $Matches[1] } else { "__sura_error" })
            $script:declared[$errName] = $true
            $js.Add("} catch ($errName) {")
        } elseif ($t -match '^finally(?:\s+do)?$') {
            $js.Add("} finally {")
        } elseif ($t -match '^if\s+(.+)\s+then\s+(.+)$') {
            $js.Add("if ($(Convert-SuraExpression $Matches[1])) { $(Convert-SimpleStatement $Matches[2] -Inline) }")
        } elseif ($t -match '^if\s+(.+)\s+then$') {
            $js.Add("if ($(Convert-SuraExpression $Matches[1])) {")
            Push-JsBlock "if"
        } elseif ($t -match '^while\s+(.+)\s+do$') {
            $js.Add("while ($(Convert-SuraExpression $Matches[1])) {")
            Push-JsBlock "while"
        } elseif ($t -match '^repeat\s+(.+)\s+do$') {
            $loopVar = "__sura_i$script:loopCounter"
            $limitVar = "__sura_repeat_limit$script:loopCounter"
            $script:loopCounter++
            $js.Add("for (let $loopVar = 0, $limitVar = $(Convert-SuraExpression $Matches[1]); $loopVar < $limitVar; $loopVar++) {")
            Push-JsBlock "repeat"
        } elseif ($t -match '^for\s+([A-Za-z_][A-Za-z0-9_]*)\s+in\s+(.+?)\s+(?:to|~)\s+(.+?)\s+step\s+(.+)\s+do$') {
            $name = $Matches[1]
            $endVar = "__sura_for_end$script:loopCounter"
            $stepVar = "__sura_for_step$script:loopCounter"
            $script:loopCounter++
            $script:declared[$name] = $true
            $js.Add("for (let $name = $(Convert-SuraExpression $Matches[2]), $endVar = $(Convert-SuraExpression $Matches[3]), $stepVar = __sura_for_step($(Convert-SuraExpression $Matches[4])); $stepVar > 0 ? $name <= $endVar : $name >= $endVar; $name += $stepVar) {")
            Push-JsBlock "for"
        } elseif ($t -match '^for\s+([A-Za-z_][A-Za-z0-9_]*)\s+in\s+(.+)\s+(?:to|~)\s+(.+)\s+do$') {
            $name = $Matches[1]
            $endVar = "__sura_for_end$script:loopCounter"
            $script:loopCounter++
            $script:declared[$name] = $true
            $js.Add("for (let $name = $(Convert-SuraExpression $Matches[2]), $endVar = $(Convert-SuraExpression $Matches[3]); $name <= $endVar; $name++) {")
            Push-JsBlock "for"
        } elseif ($t -match '^for\s+([A-Za-z_][A-Za-z0-9_]*)\s*,\s*([A-Za-z_][A-Za-z0-9_]*)\s+in\s+(.+)\s+do$') {
            $indexName = $Matches[1]
            $itemName = $Matches[2]
            $itemsVar = "__sura_for_items$script:loopCounter"
            $entriesVar = "__sura_for_entries$script:loopCounter"
            $script:loopCounter++
            $script:declared[$indexName] = $true
            $script:declared[$itemName] = $true
            $js.Add("const $itemsVar = $(Convert-SuraExpression $Matches[3]);")
            $js.Add("const $entriesVar = __sura_entries($itemsVar);")
            $js.Add("for (const [$indexName, $itemName] of $entriesVar) {")
            Push-JsBlock "for"
        } elseif ($t -match '^for\s+([A-Za-z_][A-Za-z0-9_]*)\s+in\s+(.+)\s+do$') {
            $name = $Matches[1]
            $script:declared[$name] = $true
            $js.Add("for (const $name of __sura_iter($(Convert-SuraExpression $Matches[2]))) {")
            Push-JsBlock "for"
        } elseif ($t -match '^([A-Za-z_][A-Za-z0-9_]*)\s*(?::\s*[^=]+?)?\s+is\s+func\s*\(([^)]*)\)\s*(?:->\s*.+?)?\s+do$') {
            $name = $Matches[1]
            $params = Convert-JsParamList $Matches[2]
            if ($script:declared.ContainsKey($name)) {
                $js.Add("$name = function($params) {")
            } else {
                $script:declared[$name] = $true
                $js.Add("var $name = function($params) {")
            }
            Push-JsBlock "func-expr-assign"
        } elseif ($t -match '^struct\s+([A-Za-z_][A-Za-z0-9_]*)\s+do$') {
            $name = $Matches[1]
            $internalName = "__SuraStruct_$name"
            $script:declared[$name] = $true
            $script:declared[$internalName] = $true
            $js.Add("class $internalName {")
            Push-JsBlock "struct"
            $script:jsStructStack.Add([pscustomobject]@{
                Name = $name
                InternalName = $internalName
                Fields = (New-Object System.Collections.Generic.List[object])
                UserInit = $false
            }) | Out-Null
        } elseif ($t -match '^enum\s+([A-Za-z_][A-Za-z0-9_]*)\s+do$') {
            $script:declared[$Matches[1]] = $true
            $js.Add("const $($Matches[1]) = {")
            Push-JsBlock "enum"
        } elseif ($t -match '^class\s+([A-Za-z_][A-Za-z0-9_]*)(?:\s+extends\s+([A-Za-z_][A-Za-z0-9_]*))?\s+do$') {
            $script:declared[$Matches[1]] = $true
            if ($Matches[2]) {
                $js.Add("class $($Matches[1]) extends $($Matches[2]) {")
            } else {
                $js.Add("class $($Matches[1]) {")
            }
            Push-JsBlock "class"
        } elseif ($t -match '^func\s+([A-Za-z_][A-Za-z0-9_]*)\s*\(([^)]*)\)\s*(?:->\s*.+?)?\s+do$') {
            if ((Get-JsBlockTop) -eq "class" -or (Get-JsBlockTop) -eq "struct") {
                if ((Get-JsBlockTop) -eq "struct" -and $Matches[1] -eq "init") {
                    $structInfo = Get-JsStructTop
                    if ($null -ne $structInfo) { $structInfo.UserInit = $true }
                }
                $methodName = $(if ($Matches[1] -eq "init") { "constructor" } else { $Matches[1] })
                $js.Add("$methodName($(Convert-JsParamList $Matches[2])) {")
                Push-JsBlock "method"
            } else {
                $script:declared[$Matches[1]] = $true
                $js.Add("function $($Matches[1])($(Convert-JsParamList $Matches[2])) {")
                Push-JsBlock "func"
            }
        } else {
            $js.Add((Convert-SimpleStatement $t))
        }
    } catch {
        if ($AllowUnsupported) {
            $js.Add("// unsupported: $t")
        } else {
            Write-Error "${Source}: unsupported Sura-to-JS construct: $t`n$($_.Exception.Message)"
            exit 1
        }
    }
}

Set-Content -LiteralPath $Out -Value ($js -join [Environment]::NewLine) -Encoding UTF8
Write-Host "[OK] wrote $Out"
