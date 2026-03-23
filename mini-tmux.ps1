param(
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$ArgsFromCaller
)

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $scriptDir) {
    $scriptDir = (Get-Location).Path
}

$linuxLikePath = $scriptDir -replace '\\', '/'
$wslDir = (wsl wslpath -a "$linuxLikePath").Trim()
if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($wslDir)) {
    Write-Error "Failed to resolve WSL path for $scriptDir"
    exit 1
}

$bashCommand = "cd '$wslDir' && ./mini-tmux `"`$@`""
wsl bash -lc $bashCommand -- @ArgsFromCaller
exit $LASTEXITCODE
