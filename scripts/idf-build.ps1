param(
    [Parameter(Mandatory = $true)]
    [ValidateSet("hub", "sensor", "demo")]
    [string]$Target,

    [ValidateSet("build", "upload", "monitor", "clean", "bootlog")]
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
$Forward = @($Script, "idf", $Target, $Action)
if ($Port) { $Forward += @("--port", $Port) }
& $Python.Source @Forward
exit $LASTEXITCODE
