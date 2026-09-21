<#
.SYNOPSIS
  Configure, build, test and optionally install Degraded Mode Fabric.

.DESCRIPTION
  Discovers the Microsoft Visual C++ toolchain on Windows (or uses the ambient
  compiler elsewhere), configures a Ninja build directory, builds it, and runs
  the test suites. No step is given an artificial timeout: a hanging test is a
  defect to be diagnosed, not a step to be abandoned.
#>
[CmdletBinding()]
param(
  [ValidateSet('Release', 'Debug', 'RelWithDebInfo')]
  [string] $Config = 'Release',

  [ValidateSet('plain', 'asan', 'ubsan')]
  [string] $Sanitizer = 'plain',

  [string] $BuildDir = '',

  [string] $Prefix = '',

  [switch] $SkipConfigure,
  [switch] $SkipBuild,
  [switch] $SkipTest,
  [switch] $Install
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot

function Import-MsvcEnvironment {
  $onWindows = ($env:OS -eq 'Windows_NT')
  if (-not $onWindows) { return }
  $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
  if (-not (Test-Path $vswhere)) { return }
  $install = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
  if (-not $install) { return }
  $vcvars = Join-Path $install 'VC\Auxiliary\Build\vcvars64.bat'
  if (-not (Test-Path $vcvars)) { return }
  $commandLine = '"' + $vcvars + '" >nul 2>&1 && set'
  $dump = & cmd.exe /c $commandLine
  foreach ($line in $dump) {
    if ($line -match '^([^=]+)=(.*)$') {
      Set-Item -Path ('env:' + $matches[1]) -Value $matches[2] -ErrorAction SilentlyContinue
    }
  }
  Write-Host "Using MSVC from $install"
}

Import-MsvcEnvironment

if (-not $BuildDir) {
  $suffix = if ($Sanitizer -eq 'plain') { '' } else { "-$Sanitizer" }
  $BuildDir = Join-Path $repoRoot ("build/" + $Config.ToLowerInvariant() + $suffix)
}
if (-not $Prefix) { $Prefix = Join-Path $repoRoot 'build/prefix' }

$configureArgs = @(
  '-S', $repoRoot,
  '-B', $BuildDir,
  '-G', 'Ninja',
  "-DCMAKE_BUILD_TYPE=$Config",
  '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON'
)
switch ($Sanitizer) {
  'asan' { $configureArgs += '-DDMF_ENABLE_ASAN=ON' }
  'ubsan' { $configureArgs += '-DDMF_ENABLE_UBSAN=ON' }
  default { }
}

if (-not $SkipConfigure) {
  Write-Host "== configure ($Config/$Sanitizer) =="
  & cmake @configureArgs
  if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

if (-not $SkipBuild) {
  Write-Host '== build =='
  & cmake --build $BuildDir --parallel
  if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

if (-not $SkipTest) {
  Write-Host '== test =='
  & ctest --test-dir $BuildDir --output-on-failure
  if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

if ($Install) {
  Write-Host "== install to $Prefix =="
  & cmake --install $BuildDir --prefix $Prefix
  if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

Write-Host "build directory: $BuildDir"
