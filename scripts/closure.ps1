<#
.SYNOPSIS
  Run the complete Degraded Mode Fabric closure matrix.

.DESCRIPTION
  Builds and tests every supported configuration, installs the package, builds and
  runs an independent downstream consumer outside the source tree against the
  installed prefix, runs the shipped tools and examples, and optionally verifies
  that a fresh clone of the committed sources reproduces closure.

  No step is given an artificial timeout. A hang is a defect to diagnose, not a
  step to abandon.
#>
[CmdletBinding()]
param(
  [ValidateSet('all', 'release', 'debug', 'asan', 'install', 'fresh')]
  [string] $Stage = 'all',

  [switch] $SkipTests,
  [switch] $KeepArtifacts
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$results = [System.Collections.Generic.List[string]]::new()

function Record([string] $line) {
  Write-Host $line
  $results.Add($line)
}

function Import-MsvcEnvironment {
  if ($env:OS -ne 'Windows_NT') { return }
  $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
  if (-not (Test-Path $vswhere)) { return }
  $install = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
  if (-not $install) { return }
  $vcvars = Join-Path $install 'VC\Auxiliary\Build\vcvars64.bat'
  if (-not (Test-Path $vcvars)) { return }
  $commandLine = '"' + $vcvars + '" >nul 2>&1 && set'
  foreach ($line in (& cmd.exe /c $commandLine)) {
    if ($line -match '^([^=]+)=(.*)$') {
      Set-Item -Path ('env:' + $matches[1]) -Value $matches[2] -ErrorAction SilentlyContinue
    }
  }
}

function Invoke-Build([string] $config, [string] $sanitizer) {
  $buildScript = Join-Path $PSScriptRoot 'build.ps1'
  # A hashtable splat keeps every switch bound by name; an array splat would
  # pass the first switch as the first positional value.
  $parameters = @{ Config = $config; Sanitizer = $sanitizer }
  if ($SkipTests) { $parameters['SkipTest'] = $true }
  & $buildScript @parameters
  if ($LASTEXITCODE -ne 0) { throw "build/test failed for $config/$sanitizer" }
}

function Invoke-Install([string] $prefix) {
  $buildDir = Join-Path $repoRoot 'build/release'
  & cmake --install $buildDir --prefix $prefix
  if ($LASTEXITCODE -ne 0) { throw 'install failed' }
}

function Invoke-Consumer([string] $prefix) {
  # The consumer is copied outside the source tree so it can only ever see the
  # installed package.
  $work = Join-Path ([System.IO.Path]::GetTempPath()) ('dmf-consumer-' + [guid]::NewGuid().ToString('N'))
  New-Item -ItemType Directory -Force -Path $work | Out-Null
  Copy-Item -Recurse -Force (Join-Path $repoRoot 'examples/consumer') (Join-Path $work 'consumer')
  $buildDir = Join-Path $work 'build'
  & cmake -S (Join-Path $work 'consumer') -B $buildDir -G Ninja "-DCMAKE_BUILD_TYPE=Release" "-DCMAKE_PREFIX_PATH=$prefix"
  if ($LASTEXITCODE -ne 0) { throw 'consumer configure failed' }
  & cmake --build $buildDir
  if ($LASTEXITCODE -ne 0) { throw 'consumer build failed' }
  & (Join-Path $buildDir 'dmf_consumer.exe')
  if ($LASTEXITCODE -ne 0) { throw 'consumer run failed' }
  if (-not $KeepArtifacts) { Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue }
}

function Invoke-Tools([string] $prefix) {
  $bin = Join-Path $prefix 'bin'
  & (Join-Path $bin 'dmf_selftest.exe')
  if ($LASTEXITCODE -ne 0) { throw 'installed selftest failed' }
  $store = Join-Path ([System.IO.Path]::GetTempPath()) ('dmf-tool-store-' + [guid]::NewGuid().ToString('N'))
  & (Join-Path $bin 'dmf_coordinator.exe') --root $store --allow-anonymous --exit-after-ready
  if ($LASTEXITCODE -ne 0) { throw 'installed coordinator failed to start' }
  & (Join-Path $bin 'dmf_cli.exe') verify --root $store
  if ($LASTEXITCODE -ne 0) { throw 'installed cli verify failed' }
  Remove-Item -Recurse -Force $store -ErrorAction SilentlyContinue
}

Import-MsvcEnvironment

if ($Stage -eq 'all' -or $Stage -eq 'release') {
  Record '== stage: release build and tests =='
  Invoke-Build 'Release' 'plain'
  Record 'release: PASS'
}

if ($Stage -eq 'all' -or $Stage -eq 'debug') {
  Record '== stage: debug build and tests =='
  Invoke-Build 'Debug' 'plain'
  Record 'debug: PASS'
}

if ($Stage -eq 'all' -or $Stage -eq 'asan') {
  Record '== stage: address sanitizer build and tests =='
  $buildScript = Join-Path $PSScriptRoot 'build.ps1'
  & $buildScript -Config Release -Sanitizer asan
  if ($LASTEXITCODE -ne 0) {
    Record 'asan: FAILED (see the configure output for the exact missing component)'
  } else {
    Record 'asan: PASS'
  }
}

if ($Stage -eq 'all' -or $Stage -eq 'install') {
  Record '== stage: install and downstream consumer =='
  $prefix = Join-Path $repoRoot 'build/prefix'
  Remove-Item -Recurse -Force $prefix -ErrorAction SilentlyContinue
  Invoke-Install $prefix
  Invoke-Consumer $prefix
  Invoke-Tools $prefix
  Record 'install and consumer: PASS'
}

if ($Stage -eq 'all' -or $Stage -eq 'fresh') {
  Record '== stage: fresh clone closure =='
  $work = Join-Path ([System.IO.Path]::GetTempPath()) ('dmf-fresh-' + [guid]::NewGuid().ToString('N'))
  New-Item -ItemType Directory -Force -Path $work | Out-Null
  & git clone --quiet $repoRoot (Join-Path $work 'clone')
  if ($LASTEXITCODE -ne 0) { throw 'fresh clone failed' }
  $clone = Join-Path $work 'clone'
  & (Join-Path $clone 'scripts/build.ps1') -Config Release
  if ($LASTEXITCODE -ne 0) { throw 'fresh clone build failed' }
  & (Join-Path $clone 'build/release/dmf_selftest.exe')
  if ($LASTEXITCODE -ne 0) { throw 'fresh clone selftest failed' }
  Record 'fresh clone: PASS'
  if (-not $KeepArtifacts) { Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue }
}

Record ''
Record '== closure summary =='
foreach ($line in $results) { if ($line -like '*: PASS' -or $line -like '*: FAILED*') { Write-Host $line } }
