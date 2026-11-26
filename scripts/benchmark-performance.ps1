param(
    [string]$Exe = (Join-Path $PSScriptRoot '../build/win-clang-release/pixel_sim.exe'),
    [string]$BaselineExe,
    [switch]$GpuFluid,
    [switch]$BaselineGpuFluid,
    [ValidateRange(1, 10000)][int]$Ticks = 60,
    [ValidateRange(1, 20)][int]$Repetitions = 3,
    [ValidateRange(32, 4096)][int]$Width = 480,
    [ValidateRange(32, 4096)][int]$Height = 270,
    [ValidateSet('hydro','lab','zerog','volcano','electric','explosives')]
    [string[]]$Presets = @('hydro','lab','zerog'),
    [ValidateRange(0, 1000)][double]$MaxRegressionPercent = 10,
    [string]$OutPath = (Join-Path $PSScriptRoot '../build/performance-comparison.json')
)

$ErrorActionPreference = 'Stop'
$currentExe = (Resolve-Path -LiteralPath $Exe).Path
$referenceExe = if ($BaselineExe) { (Resolve-Path -LiteralPath $BaselineExe).Path } else { $null }

function Measure-Simulation([string]$Executable, [string]$Preset, [bool]$UseGpu) {
    # Run sequentially: simultaneous benchmarks distort CPU timings.
    $benchmarkArgs = @('--width', $Width, '--height', $Height, '--preset', $Preset, '--benchmark', $Ticks)
    if ($UseGpu) { $benchmarkArgs += '--gpu-fluid' }
    $lines = & $Executable @benchmarkArgs
    if ($LASTEXITCODE -ne 0) { throw "Benchmark failed: $Executable, $Preset" }
    $output = $lines -join "`n"
    $match = [regex]::Match($output, 'Media/tick:\s+([0-9]+(?:\.[0-9]+)?)')
    if (-not $match.Success) { throw "Missing benchmark timing: $Preset" }
    return [double]::Parse($match.Groups[1].Value, [Globalization.CultureInfo]::InvariantCulture)
}

function Get-Median([double[]]$Samples) {
    $orderedSamples = @($Samples | Sort-Object)
    $middle = [int][Math]::Floor($orderedSamples.Count / 2)
    if ($orderedSamples.Count % 2) { return $orderedSamples[$middle] }
    return ($orderedSamples[$middle - 1] + $orderedSamples[$middle]) / 2
}

$results = foreach ($preset in $Presets) {
    $baselineSamples = [Collections.Generic.List[double]]::new()
    $currentSamples = [Collections.Generic.List[double]]::new()
    for ($repeat = 0; $repeat -lt $Repetitions; ++$repeat) {
        # Alternate order to reduce systematic effects from CPU frequency and
        # background activity. Never execute benchmark processes in parallel.
        $order = if ($repeat % 2) { @('current', 'baseline') } else { @('baseline', 'current') }
        foreach ($variant in $order) {
            if ($variant -eq 'baseline' -and -not $referenceExe) { continue }
            Write-Host "$variant`: $preset, $Ticks ticks, repetition $($repeat + 1)/$Repetitions"
            if ($variant -eq 'baseline') {
                $baselineSamples.Add((Measure-Simulation $referenceExe $preset $BaselineGpuFluid.IsPresent))
            } else {
                $currentSamples.Add((Measure-Simulation $currentExe $preset $GpuFluid.IsPresent))
            }
        }
    }
    $baselineMs = if ($referenceExe) { Get-Median $baselineSamples.ToArray() } else { $null }
    $currentMs = Get-Median $currentSamples.ToArray()
    [pscustomobject]@{
        Preset = $preset
        BaselineMs = $baselineMs
        CurrentMs = $currentMs
        BaselineSamplesMs = @($baselineSamples)
        CurrentSamplesMs = @($currentSamples)
        Speedup = if ($baselineMs) { $baselineMs / $currentMs } else { $null }
        Regression = if ($baselineMs) { $currentMs -gt $baselineMs * (1 + $MaxRegressionPercent / 100) } else { $false }
    }
}

$report = [ordered]@{
    Timestamp = (Get-Date).ToUniversalTime().ToString('o')
    CPU = (Get-CimInstance Win32_Processor | Select-Object -First 1 -ExpandProperty Name)
    Width = $Width; Height = $Height; Ticks = $Ticks; Repetitions = $Repetitions
    Aggregation = 'Median; alternating baseline/current run order'
    Measurement = 'Headless simulation plus CPU rendering; not GUI FPS'
    MomentumBackend = if ($GpuFluid) { 'GPU (hidden OpenGL context)' } else { 'CPU' }
    BaselineMomentumBackend = if ($BaselineGpuFluid) { 'GPU (hidden OpenGL context)' } else { 'CPU' }
    Executable = $currentExe
    ExecutableSha256 = (Get-FileHash -LiteralPath $currentExe -Algorithm SHA256).Hash
    BaselineExecutable = $referenceExe
    BaselineSha256 = if ($referenceExe) { (Get-FileHash -LiteralPath $referenceExe -Algorithm SHA256).Hash } else { $null }
    MaxRegressionPercent = $MaxRegressionPercent
    Results = @($results)
}
$reportDirectory = Split-Path -Parent ([IO.Path]::GetFullPath($OutPath))
New-Item -ItemType Directory -Path $reportDirectory -Force | Out-Null
$report | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $OutPath -Encoding UTF8
$results | Format-Table Preset, BaselineMs, CurrentMs, Speedup, Regression
if ($results | Where-Object Regression) { exit 1 }
