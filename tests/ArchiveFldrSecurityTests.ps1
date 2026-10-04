# ArchiveFldr security smoke tests. Run from a Windows Developer PowerShell.
# These tests validate the password transport contract without logging secrets.
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

function Require-Text($path, $text) {
    $content = Get-Content -Raw (Join-Path $root $path)
    if ($content -notmatch [regex]::Escape($text)) {
        throw "Missing security contract '$text' in $path"
    }
}

Require-Text 'src/ArchiveSecurity.h' 'SecureZeroMemory'
Require-Text 'src/ArchiveSecureString.h' 'VirtualLock'
Require-Text 'src/ArchiveFldrCompressMain.cpp' '--password-stdin'

$forbidden = @('--password "', '--password ''')
    $content = Get-Content -Raw (Join-Path $root $file)
    foreach ($pattern in $forbidden) {
        if ($content.Contains($pattern)) { throw "Password appears on a command line in $file" }
    }
}

Write-Host 'ArchiveFldr security source checks passed.' -ForegroundColor Green
Write-Host 'Run the Windows runtime tests before release: queue overflow, cancellation, worker crash, and manager shutdown.'
