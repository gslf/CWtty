# CWtty installer for Windows. Downloads the prebuilt binary of the latest
# release from GitHub and installs it for the current user, no compiler or
# administrator rights needed:
#
#   irm https://raw.githubusercontent.com/gslf/CWtty/main/install.ps1 | iex
#
# Options need the script block form, for example:
#   & ([scriptblock]::Create((irm https://raw.githubusercontent.com/gslf/CWtty/main/install.ps1))) -Uninstall
#
#   -Version X    install release X (e.g. 1.0.0) instead of the latest
#   -Prefix DIR   install into DIR (default %LOCALAPPDATA%\Programs\cwtty)
#   -Uninstall    remove cwtty
# CWTTY_VERSION and CWTTY_PREFIX in the environment work too.
# Run it again to update.

param(
    [string]$Version = $env:CWTTY_VERSION,
    [string]$Prefix = $env:CWTTY_PREFIX,
    [switch]$Uninstall
)

# Everything lives in a function so that, run through iex, the script does not
# change the preferences of the caller's session.
function Install-CWtty([string]$Version, [string]$Prefix, [switch]$Uninstall) {
    $ErrorActionPreference = 'Stop'
    $ProgressPreference = 'SilentlyContinue'  # Invoke-WebRequest crawls with the progress bar

    $repo = 'gslf/CWtty'
    $asset = 'cwtty-windows-x86_64.zip'

    # The user PATH is edited in the registry to keep its %VARIABLES% unexpanded.
    function Get-UserPath {
        $key = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey('Environment')
        try { [string]$key.GetValue('Path', '', 'DoNotExpandEnvironmentNames') } finally { $key.Close() }
    }
    function Set-UserPath([string]$Value) {
        $key = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey('Environment', $true)
        try { $key.SetValue('Path', $Value, 'ExpandString') } finally { $key.Close() }
        # Setting a user variable broadcasts WM_SETTINGCHANGE: new terminals
        # then see the new PATH.
        [Environment]::SetEnvironmentVariable('CWTTY_INSTALL', '1', 'User')
        [Environment]::SetEnvironmentVariable('CWTTY_INSTALL', $null, 'User')
    }
    function Test-SameDir([string]$A, [string]$B) {
        [Environment]::ExpandEnvironmentVariables($A).TrimEnd('\') -ieq $B.TrimEnd('\')
    }

    if (-not $Prefix) { $Prefix = Join-Path $env:LOCALAPPDATA 'Programs\cwtty' }
    $Prefix = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Prefix)
    $exe = Join-Path $Prefix 'cwtty.exe'
    $userPath = @((Get-UserPath) -split ';' | Where-Object { $_ })

    if ($Uninstall) {
        if (Test-Path $exe) {
            Remove-Item $exe -Force
            Write-Host "Removed $exe"
        } else {
            Write-Host "cwtty is not installed in $Prefix"
        }
        Remove-Item "$exe.old" -Force -ErrorAction SilentlyContinue
        if ((Test-Path $Prefix) -and -not (Get-ChildItem $Prefix -Force)) { Remove-Item $Prefix }
        $kept = @($userPath | Where-Object { -not (Test-SameDir $_ $Prefix) })
        if ($kept.Count -ne $userPath.Count) {
            Set-UserPath ($kept -join ';')
            Write-Host "Removed $Prefix from your PATH"
        }
        Write-Host "Your settings are still in $env:APPDATA\cwtty (delete it by hand if you want)."
        return
    }

    if (-not [Environment]::Is64BitOperatingSystem) { throw 'CWtty needs 64-bit Windows 10 or later' }

    if (-not $Version -or $Version -eq 'latest') {
        $Version = 'latest'
        $base = "https://github.com/$repo/releases/latest/download"
    } else {
        $tag = if ($Version -like 'v*') { $Version } else { "v$Version" }
        $base = "https://github.com/$repo/releases/download/$tag"
    }

    # Windows PowerShell 5.1 may still offer TLS 1.0 only.
    [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12

    $tmp = Join-Path ([IO.Path]::GetTempPath()) "cwtty-$([Guid]::NewGuid())"
    New-Item -ItemType Directory $tmp | Out-Null
    try {
        Write-Host "Downloading $asset ($Version)..."
        $zip = Join-Path $tmp $asset
        try { Invoke-WebRequest "$base/$asset" -OutFile $zip -UseBasicParsing }
        catch { throw "download failed: $base/$asset ($($_.Exception.Message))" }

        $sums = Join-Path $tmp 'SHA256SUMS'
        $haveSums = $true
        try { Invoke-WebRequest "$base/SHA256SUMS" -OutFile $sums -UseBasicParsing } catch { $haveSums = $false }
        if ($haveSums) {
            $expected = @(foreach ($line in Get-Content $sums) {
                $hash, $name = $line -split '\s+\*?', 2
                if ($name -eq $asset) { $hash }
            })[0]
            if ($expected -and (Get-FileHash $zip -Algorithm SHA256).Hash -ne $expected) {
                throw "checksum mismatch for $asset, the download is corrupted"
            }
        }

        $pkg = Join-Path $tmp 'pkg'
        Expand-Archive $zip -DestinationPath $pkg
        $new = Join-Path $pkg 'cwtty.exe'
        if (-not (Test-Path $new)) { throw "$asset does not contain cwtty.exe" }

        # A running exe cannot be overwritten but can be renamed: move the old
        # one aside, it is deleted now or at the next update.
        New-Item -ItemType Directory -Force $Prefix | Out-Null
        Remove-Item "$exe.old" -Force -ErrorAction SilentlyContinue
        if (Test-Path $exe) { Move-Item $exe "$exe.old" -Force }
        Copy-Item $new $exe
        Remove-Item "$exe.old" -Force -ErrorAction SilentlyContinue
    } finally {
        Remove-Item $tmp -Recurse -Force -ErrorAction SilentlyContinue
    }

    if (-not ($userPath | Where-Object { Test-SameDir $_ $Prefix })) {
        Set-UserPath (($userPath + $Prefix) -join ';')
        Write-Host "Added $Prefix to your PATH"
    }
    if (-not ($env:Path -split ';' | Where-Object { $_ -and (Test-SameDir $_ $Prefix) })) {
        $env:Path = "$env:Path;$Prefix"
    }

    $installed = & $exe --version 2>$null
    if (-not $installed) { $installed = 'cwtty' }
    Write-Host "Installed $installed in $exe"
    Write-Host 'Start it with: cwtty'
}

try {
    Install-CWtty -Version $Version -Prefix $Prefix -Uninstall:$Uninstall
} catch {
    Write-Host "cwtty-install: $($_.Exception.Message)" -ForegroundColor Red
}
