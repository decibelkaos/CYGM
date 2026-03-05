param(
    [string]$Port = "COM40",
    [int]$Baud = 115200,
    [string]$LogDir = "C:\Projects\CYGM\logs\serial",
    [switch]$NoTimestampPrefix
)

$ErrorActionPreference = "Stop"

if (!(Test-Path $LogDir)) {
    New-Item -ItemType Directory -Path $LogDir -Force | Out-Null
}

$stamp = Get-Date -Format "yyyy-MM-dd_HH-mm-ss"
$logFile = Join-Path $LogDir ("serial_{0}_{1}.log" -f $Port.Replace('\\.\','').Replace(':','_'), $stamp)

Write-Host "Starting serial logger"
Write-Host "Port: $Port  Baud: $Baud"
Write-Host "Log:  $logFile"
Write-Host "Press Ctrl+C to stop.`n"

$sp = New-Object System.IO.Ports.SerialPort
$sp.PortName = $Port
$sp.BaudRate = $Baud
$sp.DataBits = 8
$sp.Parity = [System.IO.Ports.Parity]::None
$sp.StopBits = [System.IO.Ports.StopBits]::One
$sp.Handshake = [System.IO.Ports.Handshake]::None
$sp.ReadTimeout = 250
$sp.NewLine = "`n"

$writer = New-Object System.IO.StreamWriter($logFile, $true, [System.Text.Encoding]::UTF8)
$writer.AutoFlush = $true

try {
    $sp.Open()
    $writer.WriteLine("=== Serial log started: $(Get-Date -Format o) | Port=$Port | Baud=$Baud ===")

    while ($true) {
        try {
            $line = $sp.ReadLine().TrimEnd("`r")
            if ([string]::IsNullOrWhiteSpace($line)) { continue }

            if ($NoTimestampPrefix) {
                Write-Host $line
                $writer.WriteLine($line)
            } else {
                $ts = Get-Date -Format "yyyy-MM-dd HH:mm:ss.fff"
                $out = "[$ts] $line"
                Write-Host $out
                $writer.WriteLine($out)
            }
        } catch [System.TimeoutException] {
            continue
        }
    }
}
catch {
    Write-Error $_
}
finally {
    if ($sp -and $sp.IsOpen) { $sp.Close() }
    if ($writer) {
        $writer.WriteLine("=== Serial log stopped: $(Get-Date -Format o) ===")
        $writer.Dispose()
    }
}
