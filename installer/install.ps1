param([switch]$Uninstall, [string]$KritaDir)
$ErrorActionPreference = 'Stop'
$log = Join-Path $env:TEMP 'haline_installer.log'

# Drives created with subst (e.g. K:) do not exist in the elevated session: use the real path.
function Resolve-RealPath([string]$path) {
    $drive = (Split-Path -Qualifier $path).ToUpper()
    foreach ($line in (subst.exe)) {
        if ($line -match '^([A-Z]:)\\: => (.+)$' -and $Matches[1] -eq $drive) {
            return $Matches[2].TrimEnd('\') + $path.Substring(2)
        }
    }
    return $path
}

function Find-Krita {
    if ($KritaDir) {
        if (Test-Path "$KritaDir\bin\krita.exe") { return $KritaDir.TrimEnd('\') }
        throw "krita.exe not found in $KritaDir\bin"
    }
    $keys = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\*',
            'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\*',
            'HKCU:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\*'
    foreach ($k in (Get-ItemProperty $keys -ErrorAction SilentlyContinue)) {
        $loc = "$($k.InstallLocation)".Trim('"').TrimEnd('\')
        if ("$($k.DisplayName)" -like 'Krita*' -and $loc -and (Test-Path "$loc\bin\krita.exe")) { return $loc }
    }
    foreach ($p in "$env:ProgramFiles\Krita (x64)", "$env:ProgramFiles\Krita", "$env:LOCALAPPDATA\Programs\Krita (x64)") {
        if (Test-Path "$p\bin\krita.exe") { return $p }
    }
    Add-Type -AssemblyName System.Windows.Forms
    $dlg = New-Object System.Windows.Forms.FolderBrowserDialog
    $dlg.Description = 'Select the Krita installation folder (the one containing bin\krita.exe)'
    if ($dlg.ShowDialog() -eq 'OK' -and (Test-Path "$($dlg.SelectedPath)\bin\krita.exe")) { return $dlg.SelectedPath }
    throw 'Krita not found.'
}

function Test-Writable([string]$dir) {
    try {
        $probe = Join-Path $dir ".haline_probe_$PID"
        [IO.File]::WriteAllText($probe, '')
        Remove-Item $probe -Force
        return $true
    } catch { return $false }
}

Start-Transcript -Path $log -Append | Out-Null
try {
    $krita = Find-Krita
    $plugins = Join-Path $krita 'lib\kritaplugins'
    $payload = Join-Path $PSScriptRoot 'lib\kritaplugins'
    Write-Host "Krita: $krita"

    if (-not (Test-Writable $plugins)) {
        Write-Host 'Requesting administrator permission...'
        $script = Resolve-RealPath $PSCommandPath
        $argList = "-NoProfile -ExecutionPolicy Bypass -File `"$script`" -KritaDir `"$krita`""
        if ($Uninstall) { $argList += ' -Uninstall' }
        Stop-Transcript | Out-Null
        Start-Process powershell.exe -Verb RunAs -ArgumentList $argList
        exit
    }

    while (Get-Process krita -ErrorAction SilentlyContinue) {
        Read-Host 'Close Krita and press Enter to continue'
    }

    foreach ($item in "$plugins\kritatoolhaline.dll", "$plugins\haline") {
        if (Test-Path $item) { Remove-Item $item -Recurse -Force }
    }

    if ($Uninstall) {
        Write-Host "Haline AI removed from $plugins" -ForegroundColor Green
    } else {
        if (-not (Test-Path "$payload\kritatoolhaline.dll")) {
            # Running from the installer source folder: the package lives in ..\dist
            $payload = Join-Path (Split-Path $PSScriptRoot -Parent) 'dist\lib\kritaplugins'
        }
        if (-not (Test-Path "$payload\kritatoolhaline.dll")) {
            throw "Plugin files not found. Run ./build.sh first."
        }
        Copy-Item "$payload\*" $plugins -Recurse -Force
        Write-Host "Haline AI installed into $plugins" -ForegroundColor Green
        Write-Host 'Open Krita and use the Haline AI tool in the toolbox (eta icon, Ctrl+Alt+H shortcut).'
    }
} catch {
    Write-Host "Error: $_" -ForegroundColor Red
    Write-Host "Log: $log"
}
Stop-Transcript | Out-Null
Read-Host 'Press Enter to exit'
