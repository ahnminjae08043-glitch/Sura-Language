param(
    [Parameter(Mandatory = $true)][string]$Header,
    [string]$Out = "",
    [string]$Lib = "PATH_TO_LIBRARY",
    [string]$Prefix = "",
    [string]$Json = ""
)

$ErrorActionPreference = "Stop"
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)

function Trim-Text([string]$Text) {
    if ($null -eq $Text) { return "" }
    return $Text.Trim()
}

function Sura-String([string]$Text) {
    return ConvertTo-Json -Compress -InputObject $Text
}

function Remove-CNoise([string]$Text) {
    $Text = [regex]::Replace($Text, '(?s)/\*.*?\*/', '')
    $Text = [regex]::Replace($Text, '(?m)//.*$', '')
    $Text = [regex]::Replace($Text, '(?m)^\s*#.*$', '')
    $previous = $null
    while ($previous -ne $Text) {
        $previous = $Text
        $Text = [regex]::Replace($Text, '(?s)\b(?:class|struct)\s+[A-Za-z_][A-Za-z0-9_]*(?:\s*:[^{;]*)?\s*\{.*?\}\s*;', '')
    }
    return $Text
}

function Clean-CType([string]$Text) {
    $Text = Trim-Text ([regex]::Replace($Text, '\s+', ' '))
    $Text = [regex]::Replace($Text, '\s*\*\s*', '*')
    $Text = [regex]::Replace($Text, '\s*&\s*', '&')
    $Text = [regex]::Replace($Text, '\b(?:extern|static|inline|constexpr|volatile|register)\b\s*', '')
    return Trim-Text ([regex]::Replace($Text, '\s+', ' '))
}

function Normalize-SignatureType([string]$Text) {
    $Text = (Clean-CType $Text).ToLowerInvariant()
    if ($Text -eq "char const*") { $Text = "const char*" }
    if ($Text.StartsWith("std::")) { $Text = $Text.Substring(5) }
    return $Text
}

function Test-TypeWord([string]$Text) {
    $words = @(
        "const", "char", "double", "float", "void", "int", "long", "short",
        "signed", "unsigned", "size_t", "ssize_t", "std::size_t", "bool", "_bool",
        "int8_t", "uint8_t", "int16_t", "uint16_t", "int32_t", "uint32_t",
        "int64_t", "uint64_t"
    )
    return $words -contains $Text.ToLowerInvariant()
}

function Convert-SignatureType {
    param([string]$Raw, [hashtable]$Aliases = $null)
    $t = Normalize-SignatureType $Raw
    for ($i = 0; $Aliases -and $i -lt 8; $i++) {
        if (-not $Aliases.ContainsKey($t)) { break }
        $next = Normalize-SignatureType ([string]$Aliases[$t])
        if ($next -eq $t) { break }
        $t = $next
    }
    if ($t.Contains("&")) { return @{ Supported = $false; Type = $t } }
    if ($t -eq "void" -or $t -eq "double" -or $t -eq "float" -or $t -eq "char*" -or $t -eq "const char*") {
        return @{ Supported = $true; Type = $t }
    }
    if ($t.Contains("*")) { return @{ Supported = $false; Type = $t } }
    $intTypes = @(
        "int", "signed int", "unsigned int",
        "short", "short int", "unsigned short", "unsigned short int",
        "long", "long int", "unsigned long", "unsigned long int",
        "long long", "long long int", "unsigned long long", "unsigned long long int",
        "size_t", "ssize_t", "bool", "_bool",
        "int8_t", "uint8_t", "int16_t", "uint16_t", "int32_t", "uint32_t",
        "int64_t", "uint64_t"
    )
    if ($intTypes -contains $t) { return @{ Supported = $true; Type = "int" } }
    return @{ Supported = $false; Type = $t }
}

function Get-CTypeAliases {
    param([string]$Content)
    $aliases = @{}
    $content = Remove-CNoise $Content
    foreach ($match in [regex]::Matches($content, '\btypedef\s+([^;{}()]+?)\s+([A-Za-z_][A-Za-z0-9_]*)\s*;')) {
        $alias = Normalize-SignatureType $match.Groups[2].Value
        $target = Normalize-SignatureType $match.Groups[1].Value
        if ($alias -and $target -and $alias -ne $target) { $aliases[$alias] = $target }
    }
    foreach ($match in [regex]::Matches($content, '\busing\s+([A-Za-z_][A-Za-z0-9_]*)\s*=\s*([^;{}()]+?)\s*;')) {
        $alias = Normalize-SignatureType $match.Groups[1].Value
        $target = Normalize-SignatureType $match.Groups[2].Value
        if ($alias -and $target -and $alias -ne $target) { $aliases[$alias] = $target }
    }

    $resolved = @{}
    foreach ($entry in $aliases.GetEnumerator()) {
        $converted = Convert-SignatureType $entry.Value $aliases
        if ($converted.Supported) { $resolved[$entry.Key] = $converted.Type }
    }
    return $resolved
}

function Convert-SuraIdentifier {
    param([string]$Text, [string]$Fallback)
    if ([string]::IsNullOrWhiteSpace($Text)) { $Text = $Fallback }
    $Text = [regex]::Replace($Text, '[^A-Za-z0-9_]', '_')
    if ($Text -notmatch '^[A-Za-z_]') { $Text = "${Fallback}_$Text" }
    $reserved = @("and", "as", "break", "catch", "class", "continue", "do", "elif", "else", "end", "false", "for", "func", "if", "import", "in", "is", "nil", "not", "or", "repeat", "return", "super", "then", "this", "throw", "true", "try", "while")
    if ($reserved -contains $Text) { return "c_$Text" }
    return $Text
}

function Convert-CConstantValue {
    param([string]$Raw)
    $text = Trim-Text ([regex]::Replace($Raw, '/\*.*?\*/', ''))
    $text = Trim-Text ([regex]::Replace($text, '//.*$', ''))
    while ($text -match '^\(([^()]+)\)$') {
        $text = Trim-Text $Matches[1]
    }
    if ([string]::IsNullOrWhiteSpace($text)) { return $null }

    $culture = [System.Globalization.CultureInfo]::InvariantCulture
    if ($text -match '^([+-]?)(0[xX][0-9A-Fa-f]+)([uUlL]*)$') {
        try {
            $sign = if ($Matches[1] -eq "-") { -1 } else { 1 }
            $hex = $Matches[2].Substring(2)
            $value = [Convert]::ToInt64($hex, 16) * $sign
            return $value.ToString($culture)
        } catch {
            return $null
        }
    }
    if ($text -match '^[+-]?[0-9]+[uUlL]*$') {
        return [regex]::Replace($text, '[uUlL]+$', '')
    }
    if ($text -match '^[+-]?(?:[0-9]+\.[0-9]*|\.[0-9]+)[fFlL]?$') {
        $value = [regex]::Replace($text, '[fFlL]$', '')
        if ($value.StartsWith(".")) { $value = "0$value" }
        if ($value.StartsWith("-.")) { $value = "-0" + $value.Substring(1) }
        if ($value.StartsWith("+")) { $value = $value.Substring(1) }
        return $value
    }
    return $null
}

function Convert-CStringConstantValue {
    param([string]$Raw)
    $text = Trim-Text ([regex]::Replace($Raw, '/\*.*?\*/', ''))
    $text = Trim-Text ([regex]::Replace($text, '//.*$', ''))
    while ($text -match '^\(([^()]+)\)$') {
        $text = Trim-Text $Matches[1]
    }
    if ($text.Length -lt 2 -or $text[0] -ne '"' -or $text[$text.Length - 1] -ne '"') {
        return $null
    }

    $out = New-Object System.Text.StringBuilder
    for ($i = 1; $i -lt $text.Length - 1; $i++) {
        $ch = $text[$i]
        if ($ch -eq "`r" -or $ch -eq "`n") { return $null }
        if ($ch -eq "\") {
            $i++
            if ($i -ge $text.Length - 1) { return $null }
            switch ($text[$i]) {
                "n" { $null = $out.Append("`n") }
                "t" { $null = $out.Append("`t") }
                '"' { $null = $out.Append('"') }
                "\" { $null = $out.Append("\") }
                default { return $null }
            }
        } elseif ($ch -eq '"') {
            return $null
        } else {
            $null = $out.Append($ch)
        }
    }
    return $out.ToString()
}

function Parse-CConstants {
    param([string]$Content, [string]$Prefix)
    $constants = New-Object System.Collections.Generic.List[object]
    $withoutBlocks = [regex]::Replace($Content, '(?s)/\*.*?\*/', '')
    foreach ($line in ($withoutBlocks -split "`r?`n")) {
        if ($line -match '\\\s*$') { continue }
        $m = [regex]::Match($line, '^\s*#\s*define\s+([A-Za-z_][A-Za-z0-9_]*)\s+(.+?)\s*$')
        if (-not $m.Success) { continue }
        $value = Convert-CConstantValue $m.Groups[2].Value
        $kind = "number"
        $literal = $value
        if ($null -eq $value) {
            $value = Convert-CStringConstantValue $m.Groups[2].Value
            if ($null -eq $value) { continue }
            $kind = "string"
            $literal = Sura-String $value
        }
        $symbol = $m.Groups[1].Value
        $constants.Add([pscustomobject][ordered]@{
            symbol = $symbol
            name = Convert-SuraIdentifier ($Prefix + $symbol) "c_const"
            kind = $kind
            value = $value
            literal = $literal
        })
    }
    return $constants.ToArray()
}

function Split-CParams([string]$ArgsText) {
    $out = New-Object System.Collections.Generic.List[string]
    $current = New-Object System.Text.StringBuilder
    $depth = 0
    foreach ($ch in $ArgsText.ToCharArray()) {
        if ($ch -eq '(' -or $ch -eq '[') { $depth++ }
        elseif (($ch -eq ')' -or $ch -eq ']') -and $depth -gt 0) { $depth-- }
        if ($ch -eq ',' -and $depth -eq 0) {
            $part = Trim-Text $current.ToString()
            if ($part) { $out.Add($part) }
            $null = $current.Clear()
        } else {
            $null = $current.Append($ch)
        }
    }
    $last = Trim-Text $current.ToString()
    if ($last) { $out.Add($last) }
    return @($out)
}

function Parse-CEnumConstants {
    param([string]$Content, [string]$Prefix)
    $constants = New-Object System.Collections.Generic.List[object]
    $withoutBlocks = [regex]::Replace($Content, '(?s)/\*.*?\*/', '')
    $withoutComments = [regex]::Replace($withoutBlocks, '(?m)//.*$', '')
    $enumPattern = '(?s)\benum(?:\s+class)?(?:\s+[A-Za-z_][A-Za-z0-9_]*)?\s*\{(.*?)\}\s*(?:[A-Za-z_][A-Za-z0-9_]*)?\s*;'
    foreach ($match in [regex]::Matches($withoutComments, $enumPattern)) {
        [Int64]$nextValue = 0
        $canInferNext = $true
        foreach ($rawEntry in @(Split-CParams $match.Groups[1].Value)) {
            $entry = Trim-Text ([regex]::Replace($rawEntry, '^\s*\[\[[^\]]+\]\]\s*', ''))
            if (-not $entry) { continue }
            $m = [regex]::Match($entry, '^([A-Za-z_][A-Za-z0-9_]*)(?:\s*=\s*(.+))?$')
            if (-not $m.Success) {
                $canInferNext = $false
                continue
            }

            $symbol = $m.Groups[1].Value
            if ($m.Groups[2].Success) {
                $literal = Convert-CConstantValue $m.Groups[2].Value
                if ($null -eq $literal -or $literal.Contains(".")) {
                    $canInferNext = $false
                    continue
                }
                try {
                    $current = [Int64]::Parse($literal, [System.Globalization.CultureInfo]::InvariantCulture)
                } catch {
                    $canInferNext = $false
                    continue
                }
            } elseif ($canInferNext) {
                $current = $nextValue
            } else {
                continue
            }

            $constants.Add([pscustomobject][ordered]@{
                symbol = $symbol
                name = Convert-SuraIdentifier ($Prefix + $symbol) "c_const"
                kind = "number"
                value = $current.ToString([System.Globalization.CultureInfo]::InvariantCulture)
                literal = $current.ToString([System.Globalization.CultureInfo]::InvariantCulture)
            })
            if ($current -eq [Int64]::MaxValue) {
                $canInferNext = $false
            } else {
                $nextValue = $current + 1
                $canInferNext = $true
            }
        }
    }
    return $constants.ToArray()
}

function Parse-CParam {
    param([string]$Raw, [int]$Index, [hashtable]$Aliases)
    $text = Trim-Text $Raw
    $eq = $text.IndexOf("=")
    if ($eq -ge 0) { $text = Trim-Text $text.Substring(0, $eq) }
    $text = [regex]::Replace($text, '\[[^\]]*\]', '*')
    if ([string]::IsNullOrWhiteSpace($text) -or $text -eq "void") { return $null }

    $typeText = $text
    $paramName = "a$Index"
    $m = [regex]::Match($text, '^(.*?)([A-Za-z_][A-Za-z0-9_]*)\s*$')
    if ($m.Success) {
        $prefixText = Trim-Text $m.Groups[1].Value
        $last = $m.Groups[2].Value
        if ($prefixText -and -not ((Test-TypeWord $last) -and $prefixText -notmatch '\*')) {
            $typeText = $prefixText
            $paramName = $last
        }
    }

    $converted = Convert-SignatureType $typeText $Aliases
    if (-not $converted.Supported -or $converted.Type -eq "void") {
        return @{ Error = "unsupported parameter type '$((Clean-CType $typeText))'" }
    }
    return @{
        Name = Convert-SuraIdentifier $paramName "a$Index"
        Type = $converted.Type
    }
}

function Test-StringType([string]$Type) {
    return $Type -eq "char*" -or $Type -eq "const char*"
}

function Test-FloatType([string]$Type) {
    return $Type -eq "double" -or $Type -eq "float"
}

function Get-FfiShapeSkipReason($Function) {
    $hasStringArg = $false
    $hasFloatArg = $false
    $hasNonFloatArg = $false
    foreach ($param in @($Function.params)) {
        if (Test-StringType $param.type) { $hasStringArg = $true }
        if (Test-FloatType $param.type) { $hasFloatArg = $true } else { $hasNonFloatArg = $true }
    }
    $returnFloat = Test-FloatType $Function.return_type
    $returnString = Test-StringType $Function.return_type
    if ($hasStringArg -and ($hasFloatArg -or $returnFloat)) {
        return "string and double/float arguments cannot be mixed in this FFI mode"
    }
    if ($returnString -and $hasFloatArg) {
        return "char* returns cannot be combined with double/float arguments in this FFI mode"
    }
    if ($hasFloatArg -and $hasNonFloatArg) {
        return "double/float arguments cannot be mixed with integer or string arguments in this FFI mode"
    }
    if ($hasFloatArg -and -not ($returnFloat -or $Function.return_type -eq "void")) {
        return "double/float arguments require a double/float or void return in this FFI mode"
    }
    if ($returnFloat -and $hasNonFloatArg) {
        return "double/float returns require only double/float arguments in this FFI mode"
    }
    return ""
}

function Parse-CHeader {
    param([string]$Content, [string]$Prefix)
    $aliases = Get-CTypeAliases $Content
    $content = Remove-CNoise $Content
    $pattern = '(?ms)(?:extern\s+(?:"C"\s+)?)?(?:(?:__declspec\s*\([^)]*\)|\[\[[^\]]+\]\]|[A-Z_][A-Z0-9_]*|static|inline|constexpr)\s+)*((?:const\s+)?(?:signed\s+|unsigned\s+)?(?:(?:std::)?size_t|ssize_t|u?int(?:8|16|32|64)_t|long\s+long|long|short|int|double|float|void|char|bool|_Bool|[A-Za-z_][A-Za-z0-9_:]*)\s*(?:const\s*)?\*?)\s+([A-Za-z_][A-Za-z0-9_]*)\s*\(([^;{}]*)\)\s*(?:noexcept(?:\s*\([^)]*\))?\s*)?(?:throw\s*\([^)]*\)\s*)?(?:__attribute__\s*\(\([^)]*\)\)\s*)?;'
    $functions = New-Object System.Collections.Generic.List[object]
    foreach ($match in [regex]::Matches($content, $pattern)) {
        $retRaw = $match.Groups[1].Value
        $convertedReturn = Convert-SignatureType $retRaw $aliases
        $fn = [ordered]@{
            return_type = $convertedReturn.Type
            symbol = $match.Groups[2].Value
            wrapper = Convert-SuraIdentifier ($Prefix + $match.Groups[2].Value) "c_func"
            params = @()
            skip_reason = ""
        }
        if (-not $convertedReturn.Supported) {
            $fn["skip_reason"] = "unsupported return type '$((Clean-CType $retRaw))'"
        }
        $rawParams = @(Split-CParams $match.Groups[3].Value)
        if ($rawParams.Count -gt 4) {
            $fn["skip_reason"] = "ffi_call supports at most 4 arguments"
        }
        if (-not $fn["skip_reason"]) {
            $params = New-Object System.Collections.Generic.List[object]
            for ($i = 0; $i -lt $rawParams.Count; $i++) {
                $parsed = Parse-CParam $rawParams[$i] $i $aliases
                if ($null -eq $parsed) { continue }
                if ($parsed.Error) {
                    $fn["skip_reason"] = $parsed.Error
                    break
                }
                $params.Add([pscustomobject][ordered]@{ name = $parsed.Name; type = $parsed.Type })
            }
            $fn["params"] = @($params.ToArray())
        }
        if (-not $fn["skip_reason"]) {
            $fn["skip_reason"] = Get-FfiShapeSkipReason ([pscustomobject]$fn)
        }
        $functions.Add([pscustomobject]$fn)
    }
    return $functions.ToArray()
}

if (-not (Test-Path -LiteralPath $Header)) {
    throw "header not found: $Header"
}
if (-not $Out) {
    $Out = [System.IO.Path]::ChangeExtension($Header, ".ffi.sura")
}

$content = [System.IO.File]::ReadAllText((Resolve-Path -LiteralPath $Header).Path)
$safePrefix = if ($Prefix) { Convert-SuraIdentifier $Prefix "c_" } else { "" }
$constants = @((Parse-CConstants $content $safePrefix) + (Parse-CEnumConstants $content $safePrefix))
$functions = @(Parse-CHeader $content $safePrefix)

$lines = New-Object System.Collections.Generic.List[string]
$lines.Add("// Generated by tools/bindgen_c.ps1 from $Header")
$lines.Add("// ffi_call supports 0..4 numeric C ABI arguments plus numeric, void, and char* returns; simple #define and enum constants are emitted as Sura values.")
if ($constants.Count -gt 0) {
    foreach ($constant in $constants) {
        $lines.Add("$($constant.name) is $($constant.literal)")
    }
    $lines.Add("")
}
$lines.Add("lib is ffi_load($(Sura-String ($Lib)))")
$lines.Add("")

$emitted = 0
$skipped = 0
foreach ($fn in $functions) {
    if ($fn.skip_reason) {
        $skipped++
        $lines.Add("// skipped $($fn.symbol): $($fn.skip_reason)")
        $lines.Add("")
        continue
    }
    $emitted++
    $paramNames = @($fn.params | ForEach-Object { $_.name })
    $paramTypes = @($fn.params | ForEach-Object { $_.type })
    $signature = "$($fn.return_type)($($paramTypes -join ','))"
    $lines.Add("func $($fn.wrapper)($($paramNames -join ', ')) do")
    $callArgs = if ($paramNames.Count -eq 0) { "" } else { ", " + ($paramNames -join ", ") }
    $lines.Add("  return ffi_call(lib, $(Sura-String ($fn.symbol)), $(Sura-String ($signature))$callArgs)")
    $lines.Add("end")
    $lines.Add("")
}

[System.IO.File]::WriteAllText($Out, ($lines -join "`n"), $utf8NoBom)

if ($Json) {
    $report = [ordered]@{
        schema = "sura.bind_c.v1"
        passed = ($emitted -gt 0)
        header = $Header
        output = $Out
        library = $Lib
        prefix = $safePrefix
        emitted = $emitted
        skipped = $skipped
        emitted_constants = $constants.Count
        constants = $constants
        functions = $functions
    }
    [System.IO.File]::WriteAllText($Json, ($report | ConvertTo-Json -Depth 10), $utf8NoBom)
}

Write-Output "[OK] wrote $Out with $emitted binding(s), $skipped skipped, $($constants.Count) constant(s)"
if ($emitted -eq 0) {
    throw "no supported C ABI prototypes found"
}
