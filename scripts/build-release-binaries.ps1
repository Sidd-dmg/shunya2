param(
    [string]$OutputRoot = "",
    [switch]$SkipBuild
)

$Python = Get-Command python -ErrorAction SilentlyContinue
if (-not $Python) { $Python = Get-Command py -ErrorAction SilentlyContinue }
if (-not $Python) {
    Write-Error "Python 3 is required. Install Python and retry."
    exit 1
}

$Script = Join-Path $PSScriptRoot "shunya_build.py"
$Forward = @($Script, "release")
if ($SkipBuild) { $Forward += "--skip-build" }
if ($OutputRoot) { $Forward += @("--output", $OutputRoot) }
& $Python.Source @Forward
exit $LASTEXITCODE
