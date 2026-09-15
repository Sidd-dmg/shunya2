param(
    [ValidateSet("build", "upload", "monitor", "clean")]
    [string]$Action = "build",
    [string]$Port = ""
)

$Python = Get-Command python -ErrorAction SilentlyContinue
if (-not $Python) { $Python = Get-Command py -ErrorAction SilentlyContinue }
if (-not $Python) {
    Write-Error "Python 3 is required. Install Python and retry."
    exit 1
}

$Script = Join-Path $PSScriptRoot "shunya_build.py"
$Forward = @($Script, "display", $Action)
if ($Port) { $Forward += @("--port", $Port) }
& $Python.Source @Forward
exit $LASTEXITCODE
