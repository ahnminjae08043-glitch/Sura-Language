param(
    [string]$Engine = ".\SuraLanguage.exe"
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path -LiteralPath $Engine)) {
    throw "Sura engine not found: $Engine"
}

$enginePath = (Resolve-Path -LiteralPath $Engine).Path
$temp = Join-Path ([System.IO.Path]::GetTempPath()) ("sura_ast_json_" + [System.Guid]::NewGuid().ToString("N"))

try {
    New-Item -ItemType Directory -Force -Path $temp | Out-Null
    $script = Join-Path $temp "ast_fixture.sura"
    $json = Join-Path $temp "ast.json"
    $source = @'
enum Mode do
  READY is 1
  DONE is 2
end

class Point do
  func init(x, y) do
    self.x is x
    self.y is y
  end
  func dist() -> int do
    return self.x + self.y
  end
end

func bare() do
  return
end

func typed_value() -> int do
  return 3
end

func typed_decimal(value: double) -> double do
  return value
end

point is new Point(1, 2)
value is point.dist()
data is {name: "Ari", score: 42}
items is [1, 2, 3]
items[1] += 4
data.score += 1

match items[0]
when 1 then print "one"
when _ then print "other"
end
'@
    [System.IO.File]::WriteAllText($script, $source, [System.Text.Encoding]::UTF8)

    $fileOut = & $enginePath --ast-json --out $json $script 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) {
        Write-Output $fileOut
        throw "--ast-json --out failed"
    }
    if (-not (Test-Path -LiteralPath $json)) {
        throw "expected AST JSON output file"
    }

    $raw = [System.IO.File]::ReadAllText($json, [System.Text.Encoding]::UTF8)
    $report = $raw | ConvertFrom-Json
    if ($report.schema -ne "sura.ast.v1" -or
        $report.type_errors -ne 0 -or
        $report.ast.node -ne "SuraBlock" -or
        $report.ast.body.Count -lt 7) {
        $report | ConvertTo-Json -Depth 12
        throw "unexpected AST JSON envelope"
    }

    $requiredNodes = @(
        "ENUM_DEF",
        "CLASS_DEF",
        "FUNC_DEF",
        "RETURN",
        "NEW_EXPR",
        "METHOD_CALL",
        "DICT_LIT",
        "ARRAY_LIT",
        "INDEX_ASSIGN",
        "DOT_ASSIGN",
        "BIN_OP",
        "MATCH",
        "CMD"
    )
    foreach ($node in $requiredNodes) {
        if ($raw -notmatch ('"node":"' + [regex]::Escape($node) + '"')) {
            throw "AST JSON missing node kind: $node"
        }
    }
    if ($raw -notmatch '"value":null') {
        throw "AST JSON should preserve bare return as a null return value"
    }
    if ($raw -notmatch '"return_type":\{"present":true' -or $raw -notmatch '"source_name":"int"') {
        throw "AST JSON should preserve function return type annotations"
    }
    if ($raw -notmatch '"source_name":"double"') {
        throw "AST JSON should preserve the original type annotation spelling"
    }
    if ($raw -notmatch '"fields":\[' -or $raw -notmatch '"methods":\[') {
        throw "AST JSON should expose class fields and methods arrays"
    }

    $stdoutLines = & $enginePath --ast-json $script 2>&1
    $stdoutCode = $LASTEXITCODE
    $stdoutText = ($stdoutLines | ForEach-Object { "$_" }) -join "`n"
    if ($stdoutCode -ne 0 -or
        $stdoutText -notmatch '"schema":"sura\.ast\.v1"' -or
        $stdoutText -notmatch '"ast":\{"node":"SuraBlock"') {
        Write-Output $stdoutText
        throw "--ast-json stdout mode failed"
    }

    "ast_json_smoke: PASS"
}
finally {
    if (Test-Path -LiteralPath $temp) {
        Remove-Item -LiteralPath $temp -Recurse -Force
    }
}

# Verified passing; state the exit code rather than inheriting it.
exit 0
