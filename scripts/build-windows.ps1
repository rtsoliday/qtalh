[CmdletBinding()]
param(
  [Parameter(Position = 0)]
  [ValidateSet('build', 'clean', 'rebuild', 'test')]
  [string]$Target = 'build',
  [Parameter(Position = 1)]
  [ValidateRange(1, 1024)]
  [int]$Jobs = 4
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Require-File([string]$Path) {
  if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
    throw "Required file not found: $Path"
  }
  return (Resolve-Path -LiteralPath $Path).Path
}

$repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$cygwinRoot = if ($env:QTALH_CYGWIN_ROOT) { $env:QTALH_CYGWIN_ROOT } else { 'C:\cygwin64' }
$cygwinBin = Join-Path $cygwinRoot 'bin'
foreach ($tool in @('sh.exe', 'uname.exe', 'cp.exe', 'rm.exe', 'mkdir.exe')) {
  $null = Require-File (Join-Path $cygwinBin $tool)
}
$make = $env:QTALH_MAKE_EXE
if (-not $make) {
  $make = @('C:\Strawberry\c\bin\make.exe', (Join-Path $cygwinBin 'make.exe')) |
    Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
}
if (-not $make) { throw 'Install GNU Make or set QTALH_MAKE_EXE.' }
$make = Require-File $make
$arguments = @('-C', $repositoryRoot, "-j$Jobs", 'OS=Windows', 'ARCH=x86_64',
               ('SHELL=' + (Join-Path $cygwinBin 'sh.exe').Replace('\', '/')))
$env:PATH = "$cygwinBin;$(Split-Path $make);$env:PATH"

if ($Target -ne 'clean') {
  $qtRoot = if ($env:QT_DIR) { $env:QT_DIR } else { 'C:\Qt\6.11.2\msvc2022_64' }
  $qtVersion = if ($env:QT_VERSION) { $env:QT_VERSION } else { '6' }
  if ($qtVersion -notin @('5', '6')) { throw 'QT_VERSION must be 5 or 6.' }
  $multimediaRoot = if ($env:QT_MULTIMEDIA_DIR) { $env:QT_MULTIMEDIA_DIR } else { $qtRoot }
  $epicsRoot = if ($env:EPICS_BASE) { $env:EPICS_BASE } else { Join-Path $repositoryRoot '..\epics-base' }
  $epicsRoot = (Resolve-Path -LiteralPath $epicsRoot).Path
  $epicsArch = if ($env:EPICS_HOST_ARCH) { $env:EPICS_HOST_ARCH } else { 'windows-x64' }
  $modules = @('Core', 'Gui', 'Widgets', 'Network', 'PrintSupport')
  if ($Target -eq 'test') { $modules += 'Test' }
  foreach ($module in $modules) {
    $null = Require-File (Join-Path $qtRoot "lib\Qt$qtVersion$module.lib")
  }
  $null = Require-File (Join-Path $multimediaRoot "lib\Qt${qtVersion}Multimedia.lib")
  foreach ($tool in @('moc.exe', 'rcc.exe')) {
    $null = Require-File (Join-Path $qtRoot "bin\$tool")
  }
  foreach ($lib in @('ca', 'Com')) {
    $null = Require-File (Join-Path $epicsRoot "lib\$epicsArch\$lib.lib")
  }
  $vcvars = if ($env:VSINSTALLDIR) { Join-Path $env:VSINSTALLDIR 'VC\Auxiliary\Build\vcvars64.bat' }
  if (-not $vcvars -or -not (Test-Path -LiteralPath $vcvars)) {
    $vswhere = Require-File (Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe')
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vs) { throw 'Visual Studio x64 C++ build tools not found.' }
    $vcvars = Join-Path ($vs | Select-Object -First 1) 'VC\Auxiliary\Build\vcvars64.bat'
  }
  $vcvars = Require-File $vcvars
  # Import the compiler environment before invoking Make with an argument list.
  $compilerEnvironment = & $env:ComSpec /d /s /c "call `"$vcvars`" >nul && set"
  if ($LASTEXITCODE -ne 0) { throw 'Could not initialize the MSVC environment.' }
  foreach ($line in $compilerEnvironment) {
    if ($line -match '^([^=]+)=(.*)$') {
      [Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process')
    }
  }
  $env:PATH = "$multimediaRoot\bin;$qtRoot\bin;$epicsRoot\bin\$epicsArch;$env:PATH;$cygwinBin;$(Split-Path $make)"
  $env:QT_PLUGIN_PATH = "$multimediaRoot\plugins;$qtRoot\plugins"
  $arguments += @(('QT_DIR=' + $qtRoot.Replace('\', '/')), "QT_VERSION=$qtVersion",
                  ('QT_MULTIMEDIA_DIR=' + $multimediaRoot.Replace('\', '/')),
                  ('EPICS_BASE=' + $epicsRoot.Replace('\', '/')), "EPICS_HOST_ARCH=$epicsArch")
  Write-Host "QtALH Windows $Target (MSVC, Qt $qtVersion, EPICS $epicsArch)"
  Write-Host "  Qt: $qtRoot"
  Write-Host "  Multimedia: $multimediaRoot"
  Write-Host "  EPICS: $epicsRoot"
}
if ($Target -in @('clean', 'rebuild')) {
  & $make @arguments clean
  if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
if ($Target -ne 'clean') {
  $makeTarget = if ($Target -eq 'test') { 'test-qtalh' } else { 'all' }
  & $make @arguments $makeTarget
  exit $LASTEXITCODE
}
