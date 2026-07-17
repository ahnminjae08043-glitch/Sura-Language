param(
    [Parameter(Mandatory=$true)][string[]]$Path
)

$failed = $false
$blockClose = '^\s*end\b'
$risky = @(
    @{ Name = "shell execution"; Pattern = '\b(async_cmd|cmd_run(?:_checked)?|task)\s*\(|\b(os\.run(?:_checked)?|os\.cmd|async\.cmd)\s*\(' },
    @{ Name = "network access"; Pattern = '(http_(get|post|json|request(_retry_json_checked|_json_checked|_retry_json|_full|_retry|_json)?|serve_static|serve_routes)|http\.(get|post|json|request(_retry_json_checked|_json_checked|_retry_json|_full|_retry|_json)?|serve_static|serve_routes))\s*\(' },
    @{ Name = "file deletion"; Pattern = '\b(file_delete|file_remove_tree|remove_tree)\s*\(|\bfs\.(delete|remove|remove_tree|delete_tree)\s*\(' },
    @{ Name = "python bridge"; Pattern = 'python_[A-Za-z_]*\s*\(' }
)
$sensitiveHeaderSource = '(?i)\b(auth_bearer|auth_basic|headers_merge)\s*\(|\bhttp\.(auth_bearer|auth_basic|headers_merge)\s*\(|\b(authorization|proxy-authorization|cookie|set-cookie|x-api-key|api-key|x-auth-token|x-csrf-token|x-xsrf-token|token|secret|api[-_]?key)\b'
$headerOutput = '^\s*(print(?:_n)?\b|log_(debug|info|warn|error)\s*\(|log\.(debug|info|warn|error|event)\s*\()'

foreach ($p in $Path) {
    $files = Get-ChildItem -LiteralPath $p -Recurse -File -Filter *.sura -ErrorAction SilentlyContinue
    if ((Test-Path -LiteralPath $p -PathType Leaf) -and $p.EndsWith(".sura")) {
        $files = @(Get-Item -LiteralPath $p)
    }
    foreach ($file in $files) {
        $depth = 0
        $lineNo = 0
        $sensitiveHeaderVars = New-Object 'System.Collections.Generic.HashSet[string]'
        foreach ($line in Get-Content -LiteralPath $file.FullName) {
            $lineNo++
            if ($line -match $blockClose) { $depth-- }
            if ($depth -lt 0) {
                Write-Host "[lint] unmatched end at $($file.FullName):$lineNo"
                $failed = $true
                $depth = 0
            }
            foreach ($rule in $risky) {
                if ($line -match $rule.Pattern) {
                    Write-Host "[lint] risky $($rule.Name) at $($file.FullName):$lineNo"
                }
            }
            $referencesSensitiveHeaders = $false
            foreach ($name in $sensitiveHeaderVars) {
                if ($line -match "\b$([regex]::Escape($name))\b") {
                    $referencesSensitiveHeaders = $true
                    break
                }
            }
            if ($line -match '^\s*([A-Za-z_][A-Za-z0-9_]*)\s+(?:is|=)\s*(.+)$') {
                $name = $Matches[1]
                $value = $Matches[2]
                if ($value -match 'headers_redact') {
                    [void]$sensitiveHeaderVars.Remove($name)
                } elseif (($value -match $sensitiveHeaderSource) -or $referencesSensitiveHeaders) {
                    [void]$sensitiveHeaderVars.Add($name)
                }
            }
            if ($line -notmatch 'headers_redact' -and
                $line -match $headerOutput -and
                (($line -match $sensitiveHeaderSource) -or $referencesSensitiveHeaders)) {
                Write-Host "[lint] risky unredacted sensitive headers at $($file.FullName):$lineNo (use headers_redact before logging)"
            }
            $trim = $line.Trim()
            $opensBlock =
                ($trim -match '^if\b.*\bthen\s*$') -or
                ($trim -match '^(while|for|foreach|repeat)\b.*\bdo\s*$') -or
                ($trim -match '^func\b.*\bdo\s*$') -or
                ($trim -match '^(class|enum|try)\b')
            if ($opensBlock) { $depth++ }
        }
        if ($depth -ne 0) {
            Write-Host "[lint] unclosed block depth $depth in $($file.FullName)"
            $failed = $true
        }
    }
}

if ($failed) { exit 1 }
Write-Host "[OK] lint passed"
