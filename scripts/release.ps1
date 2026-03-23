<#
.SYNOPSIS
    CYGM Release Manager — GUI tool for packaging and uploading firmware
.DESCRIPTION
    Packages the current ESP32 build, updates website manifests, and uploads
    everything to cygm.me via FTP. Credentials are saved locally
    with DPAPI encryption (Windows user-bound).
.NOTES
    Run AFTER 'idf.py build' completes successfully.
#>

$ErrorActionPreference = "Stop"

# ==================== Paths ====================
$script:Root       = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$script:ConfigDir  = Join-Path $env:USERPROFILE ".cygm"
$script:ConfigFile = Join-Path $script:ConfigDir "ftp_config.json"
$script:EsptoolExe = "C:\Espressif\python_env\idf5.5_py3.11_env\Scripts\esptool.exe"

# ==================== Load Assemblies ====================
Add-Type -AssemblyName PresentationFramework
Add-Type -AssemblyName PresentationCore
Add-Type -AssemblyName WindowsBase
Add-Type -AssemblyName System.Windows.Forms   # for OpenFileDialog

# ==================== Version Detection ====================
function Read-FirmwareVersion {
    $h = Get-Content "$script:Root\main\shared_state.h" -Raw
    $maj   = [regex]::Match($h, 'CYGM_VERSION_MAJOR\s+(\d+)').Groups[1].Value
    $min   = [regex]::Match($h, 'CYGM_VERSION_MINOR\s+(\d+)').Groups[1].Value
    $pat   = [regex]::Match($h, 'CYGM_VERSION_PATCH\s+(\d+)').Groups[1].Value
    $stage = [regex]::Match($h, 'CYGM_VERSION_STAGE\s+"([^"]+)"').Groups[1].Value
    $date  = [regex]::Match($h, 'CYGM_VERSION_DATE\s+"([^"]+)"').Groups[1].Value
    return @{
        Major = [int]$maj; Minor = [int]$min; Patch = [int]$pat
        Stage = $stage; Date = $date
        Version = "$maj.$min.$pat"
        Tag     = "v$maj.$min.$pat"
        Display = "v$maj.$min.$pat $stage"
    }
}

# ==================== Credential Storage ====================
function Load-FtpConfig {
    if (!(Test-Path $script:ConfigFile)) {
        return @{ Host = "access-5017566809.webspace-host.com"; Username = "a1789792"; Password = "";
                  RemotePath = "/cygm/"; UseSsl = $false }
    }
    $json = Get-Content $script:ConfigFile -Raw | ConvertFrom-Json
    $pass = ""
    if ($json.EncryptedPassword) {
        try {
            $sec = ConvertTo-SecureString $json.EncryptedPassword
            $pass = [System.Net.NetworkCredential]::new("", $sec).Password
        } catch { $pass = "" }
    }
    return @{
        Host       = if ($json.Host) { $json.Host } else { "cygm.me" }
        Username   = if ($json.Username) { $json.Username } else { "" }
        Password   = $pass
        RemotePath = if ($json.RemotePath) { $json.RemotePath } else { "/public_html/" }
        UseSsl     = [bool]$json.UseSsl
    }
}

function Save-FtpConfig($cfg) {
    if (!(Test-Path $script:ConfigDir)) {
        New-Item -ItemType Directory -Path $script:ConfigDir -Force | Out-Null
    }
    $enc = ""
    if ($cfg.Password) {
        $sec = ConvertTo-SecureString $cfg.Password -AsPlainText -Force
        $enc = ConvertFrom-SecureString $sec
    }
    @{
        Host              = $cfg.Host
        Username          = $cfg.Username
        EncryptedPassword = $enc
        RemotePath        = $cfg.RemotePath
        UseSsl            = $cfg.UseSsl
    } | ConvertTo-Json | Set-Content $script:ConfigFile -Encoding UTF8
}

# ==================== Build Packaging ====================
function Get-BuildFiles {
    $bd = "$script:Root\build"
    $files = @(
        @{ Name = "cygm.bin";                Local = "$bd\cygm.bin";                              Remote = "" }
        @{ Name = "bootloader.bin";          Local = "$bd\bootloader\bootloader.bin";              Remote = "" }
        @{ Name = "partition-table.bin";     Local = "$bd\partition_table\partition-table.bin";    Remote = "" }
        @{ Name = "ota_data_initial.bin";    Local = "$bd\ota_data_initial.bin";                  Remote = "" }
    )
    foreach ($f in $files) {
        $f.Exists = Test-Path $f.Local
        if ($f.Exists) {
            $f.SizeKB = [math]::Round((Get-Item $f.Local).Length / 1024)
        } else { $f.SizeKB = 0 }
    }
    return $files
}

function New-MergedBinary($ver) {
    $bd  = "$script:Root\build"
    $out = "$bd\cygm-merged.bin"

    if (!(Test-Path $script:EsptoolExe)) { return $null }

    $boot = "$bd\bootloader\bootloader.bin"
    $part = "$bd\partition_table\partition-table.bin"
    $ota  = "$bd\ota_data_initial.bin"
    $app  = "$bd\cygm.bin"

    foreach ($f in @($boot, $part, $ota, $app)) {
        if (!(Test-Path $f)) { return $null }
    }

    & $script:EsptoolExe --chip esp32 merge_bin `
        -o $out `
        --flash_mode dio --flash_freq 40m --flash_size 4MB `
        0x1000  $boot `
        0x8000  $part `
        0xd000  $ota `
        0x10000 $app 2>&1 | Out-Null

    if (Test-Path $out) { return $out }
    return $null
}

function Update-WebsiteManifests($ver, $notes) {
    # --- version.json (includes firmware_url for OTA) ---
    $fwUrl = "https://cygm.me/firmware/$($ver.Tag)/cygm.bin"
    @{ version = $ver.Version; stage = $ver.Stage; date = $ver.Date; firmware_url = $fwUrl } |
        ConvertTo-Json | Set-Content "$script:Root\website\version.json" -Encoding UTF8

    # --- releases.json ---
    $rp = "$script:Root\website\releases.json"
    $rel = Get-Content $rp -Raw | ConvertFrom-Json

    foreach ($v in $rel.versions) {
        if ($v.PSObject.Properties["latest"]) { $v.PSObject.Properties.Remove("latest") }
    }

    $filesObj = [PSCustomObject]@{
        merged     = "firmware/$($ver.Tag)/cygm-merged.bin"
        app        = "firmware/$($ver.Tag)/cygm.bin"
        bootloader = "firmware/$($ver.Tag)/bootloader.bin"
        partitions = "firmware/$($ver.Tag)/partition-table.bin"
        ota_data   = "firmware/$($ver.Tag)/ota_data_initial.bin"
    }

    $existing = $rel.versions | Where-Object { $_.version -eq $ver.Version }
    if ($existing) {
        $existing.date  = $ver.Date
        $existing.stage = $ver.Stage
        if ($notes) { $existing.notes = $notes }
        $existing | Add-Member -NotePropertyName "latest" -NotePropertyValue $true -Force
        $existing.files = $filesObj
    } else {
        $entry = [PSCustomObject]@{
            version = $ver.Version; date = $ver.Date; stage = $ver.Stage
            notes   = if ($notes) { $notes } else { "" }
            files   = $filesObj; latest = $true
        }
        $rel.versions = @($entry) + $rel.versions
    }

    $rel | ConvertTo-Json -Depth 5 | Set-Content $rp -Encoding UTF8

    # --- Per-version manifest.json (in firmware folder) ---
    $fwManifest = [PSCustomObject]@{
        name = "CYGM"
        version = $ver.Version
        new_install_prompt_erase = $false
        builds = @(
            [PSCustomObject]@{
                chipFamily = "ESP32"
                improv = $false
                parts = @(
                    [PSCustomObject]@{
                        path = "cygm-merged.bin"
                        offset = 0
                    }
                )
            }
        )
    }
    $fwManifestPath = "$script:Root\website\firmware\$($ver.Tag)\manifest.json"
    $fwManifest | ConvertTo-Json -Depth 5 | Set-Content $fwManifestPath -Encoding UTF8

    # --- Root cygm-manifest.json (always points to latest) ---
    $rootManifest = [PSCustomObject]@{
        name = "CYGM"
        version = $ver.Version
        new_install_prompt_erase = $false
        builds = @(
            [PSCustomObject]@{
                chipFamily = "ESP32"
                improv = $false
                parts = @(
                    [PSCustomObject]@{
                        path = "firmware/$($ver.Tag)/cygm-merged.bin"
                        offset = 0
                    }
                )
            }
        )
    }
    $rootManifest | ConvertTo-Json -Depth 5 | Set-Content "$script:Root\website\cygm-manifest.json" -Encoding UTF8
}

function Copy-FirmwareToWebsite($ver, $mergedPath) {
    $fwDir = "$script:Root\website\firmware\$($ver.Tag)"
    if (Test-Path $fwDir) { Remove-Item "$fwDir\*" -Force }
    else { New-Item -ItemType Directory -Path $fwDir -Force | Out-Null }

    $bd = "$script:Root\build"
    Copy-Item "$bd\cygm.bin"                           "$fwDir\cygm.bin"
    Copy-Item "$bd\bootloader\bootloader.bin"           "$fwDir\bootloader.bin"
    Copy-Item "$bd\partition_table\partition-table.bin"  "$fwDir\partition-table.bin"
    Copy-Item "$bd\ota_data_initial.bin"                "$fwDir\ota_data_initial.bin"
    if ($mergedPath -and (Test-Path $mergedPath)) {
        Copy-Item $mergedPath "$fwDir\cygm-merged.bin"
    }
    return $fwDir
}

# ==================== FTP Functions ====================
# All FTP operations use curl.exe (ships with Windows 10/11).
# This completely avoids .NET FtpWebRequest's broken connection pool.

$script:CurlLogFile = Join-Path $env:TEMP "cygm_curl.log"

function Curl-Upload($host_, $user_, $pass_, $remotePath, $localPath, $ssl) {
    $url = "sftp://${host_}${remotePath}"
    $curlArgs = @(
        "-T", $localPath,
        "--user", "${user_}:${pass_}",
        "--ftp-create-dirs",
        "--insecure",
        "--connect-timeout", "30",
        "--max-time", "300",
        "-v",
        "--stderr", $script:CurlLogFile,
        $url
    )
    & "C:\Program Files\Git\mingw64\bin\curl.exe" @curlArgs | Out-Null
    $code = $LASTEXITCODE
    if ($code -ne 0) {
        $detail = ""
        if (Test-Path $script:CurlLogFile) {
            $detail = Get-Content $script:CurlLogFile -Raw
        }
        throw "CURL_EXIT_${code}|${detail}"
    }
}

function Curl-Mkdir($host_, $user_, $pass_, $dirPath, $ssl) {
    $url = "sftp://${host_}/"
    $curlArgs = @("--user", "${user_}:${pass_}", "-Q", "mkdir $dirPath", "--insecure", "--connect-timeout", "15", "-s", "-S", $url)
    & "C:\Program Files\Git\mingw64\bin\curl.exe" @curlArgs 2>&1 | Out-Null
}

function Curl-List($host_, $user_, $pass_, $dirPath, $ssl) {
    $url = "sftp://${host_}${dirPath}"
    $curlArgs = @("--user", "${user_}:${pass_}", "--insecure", "--connect-timeout", "15", "-s", "-S", "--list-only", $url)
    $output = & "C:\Program Files\Git\mingw64\bin\curl.exe" @curlArgs 2>&1
    if ($LASTEXITCODE -ne 0) { return @() }
    return $output -split "`n" | Where-Object { $_.Trim() }
}

# ==================== Archive Previous Firmware ====================
function Get-ServerFirmwareDirs($host_, $user_, $pass_, $path_, $currentTag) {
    # List the server's firmware/ directory and return all entries that
    # aren't "archive" and aren't the version being uploaded.
    $entries = Curl-List $host_ $user_ $pass_ "${path_}firmware/" $false
    $dirs = @()
    foreach ($e in $entries) {
        $name = $e.Trim().TrimEnd('/')
        if ($name -and $name -ne "." -and $name -ne ".." -and $name -ne "archive" -and $name -ne $currentTag) {
            $dirs += $name
        }
    }
    return $dirs
}

function Archive-PreviousFirmware($host_, $user_, $pass_, $path_, $prevTag) {
    $srcPath = "${path_}firmware/${prevTag}"
    $dstPath = "${path_}firmware/archive/${prevTag}"

    # Move the previous firmware directory into archive via SFTP rename.
    # The archive/ directory must already exist on the server.
    $url = "sftp://${host_}/"
    $curlArgs = @(
        "--user", "${user_}:${pass_}",
        "-Q", "rename $srcPath $dstPath",
        "--insecure", "--connect-timeout", "15", "-s", "-S",
        $url
    )
    & "C:\Program Files\Git\mingw64\bin\curl.exe" @curlArgs 2>&1 | Out-Null
    return ($LASTEXITCODE -eq 0)
}

# Pump WPF dispatcher to keep UI responsive during sync operations
function Update-UI {
    [System.Windows.Threading.Dispatcher]::CurrentDispatcher.Invoke(
        [Action]{},
        [System.Windows.Threading.DispatcherPriority]::Background
    )
}

# ==================== WPF Window ====================
[xml]$xaml = @"
<Window xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation"
        xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml"
        Title="CYGM Release Manager"
        Height="1050" Width="580" MinWidth="500" MinHeight="600"
        WindowStartupLocation="CenterScreen"
        Background="#0d1520" Foreground="#c0ccd8"
        FontFamily="Segoe UI" FontSize="13">

    <Window.Resources>
        <Style x:Key="Card" TargetType="Border">
            <Setter Property="Background"      Value="#141c2b"/>
            <Setter Property="CornerRadius"     Value="10"/>
            <Setter Property="Padding"          Value="16"/>
            <Setter Property="Margin"           Value="0,0,0,10"/>
            <Setter Property="BorderBrush"      Value="#1e2d3d"/>
            <Setter Property="BorderThickness"  Value="1"/>
        </Style>
        <Style x:Key="Field" TargetType="TextBox">
            <Setter Property="Background"       Value="#0a1018"/>
            <Setter Property="Foreground"       Value="#c0ccd8"/>
            <Setter Property="BorderBrush"      Value="#2a3a50"/>
            <Setter Property="BorderThickness"  Value="1"/>
            <Setter Property="Padding"          Value="8,5"/>
            <Setter Property="CaretBrush"       Value="#c0ccd8"/>
        </Style>
        <Style x:Key="AccentBtn" TargetType="Button">
            <Setter Property="Background"       Value="#0f85d0"/>
            <Setter Property="Foreground"       Value="White"/>
            <Setter Property="BorderThickness"  Value="0"/>
            <Setter Property="Padding"          Value="18,9"/>
            <Setter Property="FontWeight"       Value="SemiBold"/>
            <Setter Property="Cursor"           Value="Hand"/>
            <Setter Property="Template">
                <Setter.Value>
                    <ControlTemplate TargetType="Button">
                        <Border x:Name="bd" Background="{TemplateBinding Background}"
                                CornerRadius="6" Padding="{TemplateBinding Padding}">
                            <ContentPresenter HorizontalAlignment="Center" VerticalAlignment="Center"/>
                        </Border>
                        <ControlTemplate.Triggers>
                            <Trigger Property="IsMouseOver" Value="True">
                                <Setter TargetName="bd" Property="Background" Value="#1a96e0"/>
                            </Trigger>
                            <Trigger Property="IsEnabled" Value="False">
                                <Setter TargetName="bd" Property="Background" Value="#1e2d3d"/>
                                <Setter Property="Foreground" Value="#556677"/>
                            </Trigger>
                        </ControlTemplate.Triggers>
                    </ControlTemplate>
                </Setter.Value>
            </Setter>
        </Style>
        <Style x:Key="GhostBtn" TargetType="Button">
            <Setter Property="Background"       Value="Transparent"/>
            <Setter Property="Foreground"       Value="#7a8ba0"/>
            <Setter Property="BorderBrush"      Value="#2a3a50"/>
            <Setter Property="BorderThickness"  Value="1"/>
            <Setter Property="Padding"          Value="14,8"/>
            <Setter Property="Cursor"           Value="Hand"/>
            <Setter Property="Template">
                <Setter.Value>
                    <ControlTemplate TargetType="Button">
                        <Border x:Name="bd" Background="{TemplateBinding Background}"
                                BorderBrush="{TemplateBinding BorderBrush}"
                                BorderThickness="{TemplateBinding BorderThickness}"
                                CornerRadius="6" Padding="{TemplateBinding Padding}">
                            <ContentPresenter HorizontalAlignment="Center" VerticalAlignment="Center"/>
                        </Border>
                        <ControlTemplate.Triggers>
                            <Trigger Property="IsMouseOver" Value="True">
                                <Setter TargetName="bd" Property="Background" Value="#1a2535"/>
                            </Trigger>
                            <Trigger Property="IsEnabled" Value="False">
                                <Setter Property="Foreground" Value="#334455"/>
                            </Trigger>
                        </ControlTemplate.Triggers>
                    </ControlTemplate>
                </Setter.Value>
            </Setter>
        </Style>
    </Window.Resources>

    <DockPanel>
        <!-- Header -->
        <Border DockPanel.Dock="Top" Background="#141c2b" Padding="20,14"
                BorderBrush="#1e2d3d" BorderThickness="0,0,0,1">
            <DockPanel>
                <TextBlock Text="CYGM Release Manager" FontSize="18"
                           FontWeight="SemiBold" Foreground="#0f85d0"/>
                <TextBlock DockPanel.Dock="Right" Name="lblVersion"
                           Foreground="#7a8ba0" VerticalAlignment="Center"
                           HorizontalAlignment="Right" FontSize="13"/>
            </DockPanel>
        </Border>

        <ScrollViewer VerticalScrollBarVisibility="Auto" Padding="20,14,20,20">
            <StackPanel>

                <!-- Build Info -->
                <Border Style="{StaticResource Card}">
                    <StackPanel>
                        <TextBlock Text="Build Info" FontWeight="SemiBold" Foreground="#e0e8f0"
                                   FontSize="14" Margin="0,0,0,6"/>
                        <TextBlock Name="lblBuildInfo" Foreground="#7a8ba0" TextWrapping="Wrap"/>
                    </StackPanel>
                </Border>

                <!-- Files -->
                <Border Style="{StaticResource Card}">
                    <StackPanel>
                        <TextBlock Text="Release Files" FontWeight="SemiBold" Foreground="#e0e8f0"
                                   FontSize="14" Margin="0,0,0,6"/>
                        <TextBlock Name="lblFiles" Foreground="#7a8ba0" FontFamily="Consolas"
                                   FontSize="12"/>
                    </StackPanel>
                </Border>

                <!-- Release Notes -->
                <Border Style="{StaticResource Card}">
                    <StackPanel>
                        <TextBlock Text="Release Notes" FontWeight="SemiBold" Foreground="#e0e8f0"
                                   FontSize="14" Margin="0,0,0,6"/>
                        <TextBox Name="txtNotes" Style="{StaticResource Field}"
                                 Height="65" AcceptsReturn="True" TextWrapping="Wrap"
                                 VerticalScrollBarVisibility="Auto"/>
                    </StackPanel>
                </Border>

                <!-- Server Settings -->
                <Border Style="{StaticResource Card}">
                    <StackPanel>
                        <TextBlock Text="Server Settings" FontWeight="SemiBold" Foreground="#e0e8f0"
                                   FontSize="14" Margin="0,0,0,10"/>
                        <Grid>
                            <Grid.ColumnDefinitions>
                                <ColumnDefinition Width="82"/>
                                <ColumnDefinition Width="*"/>
                            </Grid.ColumnDefinitions>
                            <Grid.RowDefinitions>
                                <RowDefinition Height="Auto"/>
                                <RowDefinition Height="Auto"/>
                                <RowDefinition Height="Auto"/>
                                <RowDefinition Height="Auto"/>
                                <RowDefinition Height="Auto"/>
                            </Grid.RowDefinitions>

                            <TextBlock Grid.Row="0" Text="Host:" VerticalAlignment="Center" Margin="0,0,0,6"/>
                            <TextBox   Grid.Row="0" Grid.Column="1" Name="txtHost"
                                       Style="{StaticResource Field}" Margin="0,0,0,6"/>

                            <TextBlock Grid.Row="1" Text="Username:" VerticalAlignment="Center" Margin="0,0,0,6"/>
                            <TextBox   Grid.Row="1" Grid.Column="1" Name="txtUser"
                                       Style="{StaticResource Field}" Margin="0,0,0,6"/>

                            <TextBlock Grid.Row="2" Text="Password:" VerticalAlignment="Center" Margin="0,0,0,6"/>
                            <PasswordBox Grid.Row="2" Grid.Column="1" Name="txtPass"
                                         Background="#0a1018" Foreground="#c0ccd8"
                                         BorderBrush="#2a3a50" BorderThickness="1"
                                         Padding="8,5" Margin="0,0,0,6"/>

                            <TextBlock Grid.Row="3" Text="Path:" VerticalAlignment="Center" Margin="0,0,0,6"/>
                            <TextBox   Grid.Row="3" Grid.Column="1" Name="txtPath"
                                       Style="{StaticResource Field}" Margin="0,0,0,6"/>

                            <StackPanel Grid.Row="4" Grid.ColumnSpan="2" Orientation="Horizontal" Margin="0,4,0,0">
                                <TextBlock Text="Protocol: SFTP" Foreground="#556677"
                                           VerticalAlignment="Center" Margin="0,0,24,0"/>
                                <CheckBox Name="chkSave" Content="  Save credentials"
                                          Foreground="#7a8ba0" IsChecked="True"/>
                            </StackPanel>
                        </Grid>
                    </StackPanel>
                </Border>

                <!-- Progress -->
                <Border Style="{StaticResource Card}">
                    <StackPanel>
                        <DockPanel Margin="0,0,0,6">
                            <TextBlock Name="lblStatus" Text="Ready" Foreground="#7a8ba0"/>
                            <TextBlock Name="lblPercent" Foreground="#7a8ba0"
                                       HorizontalAlignment="Right"/>
                        </DockPanel>
                        <Border Background="#1a2535" CornerRadius="3" Height="8">
                            <Border Name="progressFill" Background="#0f85d0" CornerRadius="3"
                                    HorizontalAlignment="Left" Width="0"/>
                        </Border>
                    </StackPanel>
                </Border>

                <!-- Log -->
                <Border Style="{StaticResource Card}" MaxHeight="170">
                    <TextBox Name="txtLog" IsReadOnly="True" Background="Transparent"
                             Foreground="#556677" BorderThickness="0"
                             VerticalScrollBarVisibility="Auto" TextWrapping="Wrap"
                             FontFamily="Consolas" FontSize="11"
                             Text="Waiting for action..."/>
                </Border>

                <!-- Buttons -->
                <DockPanel Margin="0,6,0,0">
                    <StackPanel DockPanel.Dock="Right" Orientation="Horizontal">
                        <Button Name="btnUpload" Content="Upload Release"
                                Style="{StaticResource AccentBtn}" Margin="8,0,0,0"/>
                        <Button Name="btnClose"  Content="Close"
                                Style="{StaticResource GhostBtn}" Margin="8,0,0,0"/>
                    </StackPanel>
                    <Button Name="btnExtra" Content="Upload Extra Files..."
                            Style="{StaticResource GhostBtn}"/>
                </DockPanel>

            </StackPanel>
        </ScrollViewer>
    </DockPanel>
</Window>
"@

# ==================== Create Window ====================
$reader = New-Object System.Xml.XmlNodeReader $xaml
$window = [Windows.Markup.XamlReader]::Load($reader)

# Named controls
$lblVersion   = $window.FindName("lblVersion")
$lblBuildInfo = $window.FindName("lblBuildInfo")
$lblFiles     = $window.FindName("lblFiles")
$txtNotes     = $window.FindName("txtNotes")
$txtHost      = $window.FindName("txtHost")
$txtUser      = $window.FindName("txtUser")
$txtPass      = $window.FindName("txtPass")
$txtPath      = $window.FindName("txtPath")
# chkSsl removed — protocol is always SFTP
$chkSave      = $window.FindName("chkSave")
$lblStatus    = $window.FindName("lblStatus")
$lblPercent   = $window.FindName("lblPercent")
$progressFill = $window.FindName("progressFill")
$txtLog       = $window.FindName("txtLog")
$btnUpload    = $window.FindName("btnUpload")
$btnClose     = $window.FindName("btnClose")
$btnExtra     = $window.FindName("btnExtra")

# Progress bar parent (for calculating width)
$progressTrack = $progressFill.Parent

# ==================== Helpers ====================
function Log($msg) {
    $ts = Get-Date -Format "HH:mm:ss"
    $txtLog.AppendText("[$ts] $msg`r`n")
    $txtLog.ScrollToEnd()
    Update-UI
}

function Set-Progress($pct, $status) {
    $lblStatus.Text  = $status
    $lblPercent.Text = "$pct%"
    $trackWidth = $progressTrack.ActualWidth
    if ($trackWidth -gt 0) {
        $progressFill.Width = [math]::Round($trackWidth * $pct / 100)
    }
    Update-UI
}

# ==================== Populate Window ====================
$ver = Read-FirmwareVersion
$lblVersion.Text = "$($ver.Display)  ($($ver.Date))"

# Build info
$appBin = "$script:Root\build\cygm.bin"
if (Test-Path $appBin) {
    $bt = (Get-Item $appBin).LastWriteTime
    $age = (Get-Date) - $bt
    $info = "Build time: $bt"
    if ($age.TotalMinutes -gt 30) {
        $info += "  `(WARNING: $([int]$age.TotalMinutes) min old -- consider rebuilding`)"
    }
    $lblBuildInfo.Text = $info
} else {
    $lblBuildInfo.Text = "ERROR: No build found. Run 'idf.py build' first."
    $btnUpload.IsEnabled = $false
}

# File listing
$buildFiles = Get-BuildFiles
$missing = $buildFiles | Where-Object { -not $_.Exists }
if ($missing) {
    $lblFiles.Text = "MISSING FILES:`n" + (($missing | ForEach-Object { "  $($_.Name)" }) -join "`n")
    $btnUpload.IsEnabled = $false
} else {
    $lines = $buildFiles | ForEach-Object {
        $sizeStr = "$($_.SizeKB) KB".PadLeft(10)
        "  $([char]0x2713)  $($_.Name.PadRight(24)) $sizeStr"
    }
    # Add manifest/website files
    $lines += "  $([char]0x2713)  version.json"
    $lines += "  $([char]0x2713)  releases.json"
    $lines += "  $([char]0x2713)  cygm-manifest.json"
    $lblFiles.Text = $lines -join "`n"
}

# Load saved FTP config
$ftpCfg = Load-FtpConfig
$txtHost.Text     = $ftpCfg.Host
$txtUser.Text     = $ftpCfg.Username
$txtPass.Password = $ftpCfg.Password
$txtPath.Text     = $ftpCfg.RemotePath
# SFTP always used (server requires it)

# ==================== Upload Release ====================
$btnUpload.Add_Click({
    $btnUpload.IsEnabled = $false
    $btnExtra.IsEnabled  = $false
    $txtLog.Text = ""

    try {
        $host_  = $txtHost.Text.Trim()
        $user_  = $txtUser.Text.Trim()
        $pass_  = $txtPass.Password
        $path_  = $txtPath.Text.Trim().TrimEnd("/") + "/"
        $ssl_   = $false
        $notes_ = $txtNotes.Text.Trim()

        if (!$host_ -or !$user_ -or !$pass_) {
            Log "ERROR: Fill in Host, Username, and Password."
            $btnUpload.IsEnabled = $true
            $btnExtra.IsEnabled  = $true
            return
        }

        # Save credentials if checked
        if ($chkSave.IsChecked) {
            Save-FtpConfig @{
                Host = $host_; Username = $user_; Password = $pass_
                RemotePath = $path_; UseSsl = $ssl_
            }
            Log "Credentials saved."
        }

        # Step 1: Scan server for firmware dirs to archive
        Set-Progress 2 "Scanning server firmware..."
        $archiveDirs = Get-ServerFirmwareDirs $host_ $user_ $pass_ $path_ $ver.Tag
        if ($archiveDirs.Count -gt 0) {
            Log "Found $($archiveDirs.Count) firmware dir(s) to archive: $($archiveDirs -join ', ')"
        } else {
            Log "No previous firmware to archive on server."
        }

        # Step 2: Package locally
        Set-Progress 5 "Creating merged binary..."
        Log "Packaging firmware $($ver.Display)..."
        $merged = New-MergedBinary $ver
        if ($merged) {
            $mergedKB = [math]::Round((Get-Item $merged).Length / 1024)
            Log "Merged binary created -- ${mergedKB} KB"
        } else {
            Log "WARNING: Could not create merged binary -- esptool not found. Skipping."
        }

        Set-Progress 10 "Copying to website folder..."
        $fwDir = Copy-FirmwareToWebsite $ver $merged
        Log "Firmware copied to website/firmware/$($ver.Tag)/"

        Set-Progress 15 "Updating manifests..."
        Update-WebsiteManifests $ver $notes_
        Log "version.json and releases.json updated."

        # Step 3: Archive all previous firmware on server
        if ($archiveDirs.Count -gt 0) {
            Set-Progress 18 "Archiving previous firmware..."
            foreach ($dir in $archiveDirs) {
                Log "Archiving $dir to firmware/archive/$dir..."
                $archived = Archive-PreviousFirmware $host_ $user_ $pass_ $path_ $dir
                if ($archived) {
                    Log "Archived $dir successfully."
                } else {
                    Log "WARNING: Could not archive $dir (rename failed). Continuing."
                }
            }
        }

        # Step 4: Build upload list (remote path + local path pairs)
        $fwRemote = "${path_}firmware/$($ver.Tag)/"
        $uploads = @()
        Get-ChildItem $fwDir | ForEach-Object {
            $uploads += @{ LocalPath = $_.FullName; RemotePath = "${fwRemote}$($_.Name)"; Name = "firmware/$($ver.Tag)/$($_.Name)" }
        }
        $uploads += @{ LocalPath = "$script:Root\website\version.json";      RemotePath = "${path_}version.json";      Name = "version.json" }
        $uploads += @{ LocalPath = "$script:Root\website\releases.json";     RemotePath = "${path_}releases.json";     Name = "releases.json" }
        $uploads += @{ LocalPath = "$script:Root\website\cygm-manifest.json"; RemotePath = "${path_}cygm-manifest.json"; Name = "cygm-manifest.json" }

        # Step 5: Upload via curl (--ftp-create-dirs handles directory creation)
        Set-Progress 20 "Uploading to $host_..."
        Log "Uploading to $host_ via curl..."

        $total = $uploads.Count
        for ($i = 0; $i -lt $total; $i++) {
            $f = $uploads[$i]
            $pct = 20 + [int](($i / $total) * 75)
            $sizeKB = [math]::Round((Get-Item $f.LocalPath).Length / 1024)
            Set-Progress $pct "Uploading $($f.Name)..."
            Log "Uploading $($f.Name) -- $sizeKB KB"
            Update-UI

            Curl-Upload $host_ $user_ $pass_ $f.RemotePath $f.LocalPath $ssl_

            # Brief pause between uploads to avoid SSH rate limiting
            if ($i -lt ($total - 1)) { Start-Sleep -Milliseconds 500 }
        }

        Set-Progress 100 "Upload complete!"
        Log "All files uploaded successfully!"
        Log "Release $($ver.Display) is now live on $host_"

    } catch {
        $msg = $_.Exception.Message
        if ($msg -match "CURL_EXIT_(\d+)\|(.*)") {
            Log "ERROR: curl exit code $($Matches[1])"
            $lines = $Matches[2] -split "`n"
            foreach ($ln in $lines) {
                $t = $ln.Trim()
                if ($t -and $t -notmatch "^[%\s]*Total" -and $t -notmatch "^[\s\d]+[kMG]?\s") {
                    Log "  $t"
                }
            }
        } else {
            Log "ERROR: $msg"
        }
        Set-Progress 0 "Upload failed"
    } finally {
        $btnUpload.IsEnabled = $true
        $btnExtra.IsEnabled  = $true
    }
})

# ==================== Upload Extra Files ====================
$btnExtra.Add_Click({
    $dlg = New-Object System.Windows.Forms.OpenFileDialog
    $dlg.Title = "Select files to upload"
    $dlg.Multiselect = $true
    $dlg.Filter = "All Files (*.*)|*.*"

    if ($dlg.ShowDialog() -ne [System.Windows.Forms.DialogResult]::OK) { return }

    $host_  = $txtHost.Text.Trim()
    $user_  = $txtUser.Text.Trim()
    $pass_  = $txtPass.Password
    $path_  = $txtPath.Text.Trim().TrimEnd("/") + "/"
    $ssl_   = $false

    if (!$host_ -or !$user_ -or !$pass_) {
        Log "ERROR: Fill in server credentials first."
        return
    }

    # Ask for remote subdirectory
    $subDir = [Microsoft.VisualBasic.Interaction]::InputBox(
        "Upload to which remote subdirectory?`nRelative to $path_",
        "Remote Path", "")

    $btnUpload.IsEnabled = $false
    $btnExtra.IsEnabled  = $false

    try {
        $remoteDest = $path_
        if ($subDir) {
            $remoteDest += $subDir.TrimStart("/").TrimEnd("/") + "/"
        }

        $files = $dlg.FileNames
        $total = $files.Count
        for ($i = 0; $i -lt $total; $i++) {
            $f = $files[$i]
            $name = [System.IO.Path]::GetFileName($f)
            $pct = [int](($i / $total) * 100)
            $sizeKB = [math]::Round((Get-Item $f).Length / 1024)
            Set-Progress $pct "Uploading $name..."
            Log "Extra: uploading $name -- $sizeKB KB to $remoteDest"
            Update-UI

            Curl-Upload $host_ $user_ $pass_ "${remoteDest}${name}" $f $ssl_
        }

        Set-Progress 100 "Extra files uploaded!"
        Log "Uploaded $total extra files to $remoteDest"

    } catch {
        $errMsg = $_.Exception.Message
        if ($_.Exception.InnerException) { $errMsg = $_.Exception.InnerException.Message }
        Log "ERROR: $($_.Exception.Message)"
    } finally {
        $btnUpload.IsEnabled = $true
        $btnExtra.IsEnabled  = $true
    }
})

# ==================== Close ====================
$btnClose.Add_Click({ $window.Close() })

# ==================== Load VB assembly for InputBox ====================
try { Add-Type -AssemblyName Microsoft.VisualBasic } catch {}

# ==================== Show Window ====================
$window.ShowDialog() | Out-Null
