Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing

$ErrorActionPreference = 'Stop'

# ---------- State ----------
$serial = $null
$writer = $null
$logFile = $null
$connected = $false
$rxBuffer = ""

$defaultLogDir = 'C:\Projects\CYGM\logs\serial'
if (!(Test-Path $defaultLogDir)) { New-Item -ItemType Directory -Path $defaultLogDir -Force | Out-Null }

# ---------- UI ----------
$form = New-Object System.Windows.Forms.Form
$form.Text = 'CYGM Serial Monitor'
$form.Size = New-Object System.Drawing.Size(980, 680)
$form.StartPosition = 'CenterScreen'

$lblPort = New-Object System.Windows.Forms.Label
$lblPort.Text = 'Port:'
$lblPort.Location = New-Object System.Drawing.Point(10, 14)
$lblPort.AutoSize = $true
$form.Controls.Add($lblPort)

$cmbPort = New-Object System.Windows.Forms.ComboBox
$cmbPort.Location = New-Object System.Drawing.Point(48, 10)
$cmbPort.Size = New-Object System.Drawing.Size(100, 24)
$cmbPort.DropDownStyle = 'DropDownList'
$form.Controls.Add($cmbPort)

$btnRefreshPorts = New-Object System.Windows.Forms.Button
$btnRefreshPorts.Text = 'Refresh'
$btnRefreshPorts.Location = New-Object System.Drawing.Point(155, 9)
$btnRefreshPorts.Size = New-Object System.Drawing.Size(70, 26)
$form.Controls.Add($btnRefreshPorts)

$lblBaud = New-Object System.Windows.Forms.Label
$lblBaud.Text = 'Baud:'
$lblBaud.Location = New-Object System.Drawing.Point(235, 14)
$lblBaud.AutoSize = $true
$form.Controls.Add($lblBaud)

$txtBaud = New-Object System.Windows.Forms.TextBox
$txtBaud.Location = New-Object System.Drawing.Point(275, 10)
$txtBaud.Size = New-Object System.Drawing.Size(80, 24)
$txtBaud.Text = '115200'
$form.Controls.Add($txtBaud)

$btnConnect = New-Object System.Windows.Forms.Button
$btnConnect.Text = 'Connect'
$btnConnect.Location = New-Object System.Drawing.Point(365, 9)
$btnConnect.Size = New-Object System.Drawing.Size(80, 26)
$form.Controls.Add($btnConnect)

$btnDisconnect = New-Object System.Windows.Forms.Button
$btnDisconnect.Text = 'Disconnect'
$btnDisconnect.Location = New-Object System.Drawing.Point(450, 9)
$btnDisconnect.Size = New-Object System.Drawing.Size(90, 26)
$btnDisconnect.Enabled = $false
$form.Controls.Add($btnDisconnect)

$btnReset = New-Object System.Windows.Forms.Button
$btnReset.Text = 'Reset (RTS)'
$btnReset.Location = New-Object System.Drawing.Point(545, 9)
$btnReset.Size = New-Object System.Drawing.Size(95, 26)
$btnReset.Enabled = $false
$form.Controls.Add($btnReset)

$btnClear = New-Object System.Windows.Forms.Button
$btnClear.Text = 'Clear'
$btnClear.Location = New-Object System.Drawing.Point(645, 9)
$btnClear.Size = New-Object System.Drawing.Size(60, 26)
$form.Controls.Add($btnClear)

$btnOpenLogDir = New-Object System.Windows.Forms.Button
$btnOpenLogDir.Text = 'Open Logs'
$btnOpenLogDir.Location = New-Object System.Drawing.Point(710, 9)
$btnOpenLogDir.Size = New-Object System.Drawing.Size(85, 26)
$form.Controls.Add($btnOpenLogDir)

$chkTimestamp = New-Object System.Windows.Forms.CheckBox
$chkTimestamp.Text = 'Timestamp'
$chkTimestamp.Checked = $true
$chkTimestamp.Location = New-Object System.Drawing.Point(805, 12)
$chkTimestamp.AutoSize = $true
$form.Controls.Add($chkTimestamp)

$txtConsole = New-Object System.Windows.Forms.TextBox
$txtConsole.Location = New-Object System.Drawing.Point(10, 45)
$txtConsole.Size = New-Object System.Drawing.Size(944, 535)
$txtConsole.Multiline = $true
$txtConsole.ScrollBars = 'Vertical'
$txtConsole.ReadOnly = $true
$txtConsole.Font = New-Object System.Drawing.Font('Consolas', 9)
$form.Controls.Add($txtConsole)

$txtCommand = New-Object System.Windows.Forms.TextBox
$txtCommand.Location = New-Object System.Drawing.Point(10, 590)
$txtCommand.Size = New-Object System.Drawing.Size(760, 24)
$form.Controls.Add($txtCommand)

$btnSend = New-Object System.Windows.Forms.Button
$btnSend.Text = 'Send'
$btnSend.Location = New-Object System.Drawing.Point(778, 589)
$btnSend.Size = New-Object System.Drawing.Size(70, 26)
$btnSend.Enabled = $false
$form.Controls.Add($btnSend)

$btnSendCRLF = New-Object System.Windows.Forms.Button
$btnSendCRLF.Text = 'Send + CRLF'
$btnSendCRLF.Location = New-Object System.Drawing.Point(854, 589)
$btnSendCRLF.Size = New-Object System.Drawing.Size(100, 26)
$btnSendCRLF.Enabled = $false
$form.Controls.Add($btnSendCRLF)

$status = New-Object System.Windows.Forms.StatusStrip
$statusLabel = New-Object System.Windows.Forms.ToolStripStatusLabel
$statusLabel.Text = 'Disconnected'
$status.Items.Add($statusLabel) | Out-Null
$form.Controls.Add($status)

# Poll timer (UI thread = stable)
$pollTimer = New-Object System.Windows.Forms.Timer
$pollTimer.Interval = 100

# ---------- Helpers ----------
function Add-ConsoleLine([string]$line) {
    if ($txtConsole.IsDisposed) { return }
    $txtConsole.AppendText($line + [Environment]::NewLine)
    $txtConsole.SelectionStart = $txtConsole.TextLength
    $txtConsole.ScrollToCaret()
}

function Set-Status([string]$text) {
    if ($statusLabel.IsDisposed) { return }
    $statusLabel.Text = $text
}

function Refresh-Ports {
    $ports = [System.IO.Ports.SerialPort]::GetPortNames() | Sort-Object
    $cmbPort.Items.Clear()
    foreach ($p in $ports) { [void]$cmbPort.Items.Add($p) }
    if ($ports.Count -gt 0) { $cmbPort.SelectedIndex = 0 }
}

function Write-Log([string]$line) {
    if ($writer) {
        try { $writer.WriteLine($line) } catch {}
    }
}

function Emit-Line([string]$raw) {
    if ([string]::IsNullOrWhiteSpace($raw)) { return }
    if ($chkTimestamp.Checked) {
        $line = "[{0}] {1}" -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss.fff'), $raw
    } else {
        $line = $raw
    }
    Add-ConsoleLine $line
    Write-Log $line
}

function Close-Serial {
    $pollTimer.Stop()

    if ($serial) {
        try {
            if ($serial.IsOpen) { $serial.Close() }
            $serial.Dispose()
        } catch {}
        $serial = $null
    }

    if ($writer) {
        try {
            $writer.WriteLine("=== Serial log stopped: $(Get-Date -Format o) ===")
            $writer.Flush()
            $writer.Dispose()
        } catch {}
        $writer = $null
    }

    $script:rxBuffer = ""
    $script:connected = $false
    $btnConnect.Enabled = $true
    $btnDisconnect.Enabled = $false
    $btnReset.Enabled = $false
    $btnSend.Enabled = $false
    $btnSendCRLF.Enabled = $false
    Set-Status 'Disconnected'
}

# ---------- Events ----------
$pollTimer.Add_Tick({
    if (-not $serial -or -not $serial.IsOpen) { return }
    try {
        $chunk = $serial.ReadExisting()
        if ([string]::IsNullOrEmpty($chunk)) { return }

        $script:rxBuffer += $chunk
        $script:rxBuffer = $script:rxBuffer -replace "`r", ""

        while ($script:rxBuffer.Contains("`n")) {
            $idx = $script:rxBuffer.IndexOf("`n")
            $line = $script:rxBuffer.Substring(0, $idx)
            $script:rxBuffer = $script:rxBuffer.Substring($idx + 1)
            Emit-Line $line
        }
    } catch {
        Add-ConsoleLine "=== Read error: $($_.Exception.Message) ==="
    }
})

$btnRefreshPorts.Add_Click({ Refresh-Ports })
$btnClear.Add_Click({ $txtConsole.Clear() })
$btnOpenLogDir.Add_Click({ Start-Process explorer.exe $defaultLogDir })

$btnConnect.Add_Click({
    try {
        if (-not $cmbPort.SelectedItem) { throw 'No COM port selected.' }
        $port = [string]$cmbPort.SelectedItem
        $baud = [int]$txtBaud.Text

        $stamp = Get-Date -Format 'yyyy-MM-dd_HH-mm-ss'
        $script:logFile = Join-Path $defaultLogDir ("serial_{0}_{1}.log" -f $port, $stamp)
        $script:writer = New-Object System.IO.StreamWriter($script:logFile, $true, [System.Text.Encoding]::UTF8)
        $writer.AutoFlush = $true

        $script:serial = New-Object System.IO.Ports.SerialPort($port, $baud, [System.IO.Ports.Parity]::None, 8, [System.IO.Ports.StopBits]::One)
        $serial.Handshake = [System.IO.Ports.Handshake]::None
        $serial.ReadTimeout = 200
        $serial.WriteTimeout = 500
        $serial.NewLine = "`n"
        $serial.Open()

        $script:rxBuffer = ""
        Write-Log "=== Serial log started: $(Get-Date -Format o) | Port=$port | Baud=$baud ==="
        Add-ConsoleLine "=== Connected to $port @ $baud | Logging: $logFile ==="

        $pollTimer.Start()

        $script:connected = $true
        $btnConnect.Enabled = $false
        $btnDisconnect.Enabled = $true
        $btnReset.Enabled = $true
        $btnSend.Enabled = $true
        $btnSendCRLF.Enabled = $true
        Set-Status "Connected: $port @ $baud"
    }
    catch {
        [System.Windows.Forms.MessageBox]::Show("Connect failed: $($_.Exception.Message)", 'CYGM Serial Monitor', 'OK', 'Error') | Out-Null
        Close-Serial
    }
})

$btnDisconnect.Add_Click({ Close-Serial })

$btnReset.Add_Click({
    try {
        if ($serial -and $serial.IsOpen) {
            $serial.RtsEnable = $true
            Start-Sleep -Milliseconds 120
            $serial.RtsEnable = $false
            Add-ConsoleLine "=== Reset pulse sent (RTS) ==="
        }
    } catch {
        Add-ConsoleLine "=== Reset failed: $($_.Exception.Message) ==="
    }
})

$btnSend.Add_Click({
    try {
        if ($serial -and $serial.IsOpen) {
            $msg = $txtCommand.Text
            if (![string]::IsNullOrWhiteSpace($msg)) {
                $serial.Write($msg)
                Add-ConsoleLine ">>> $msg"
            }
        }
    } catch {
        Add-ConsoleLine "=== Send failed: $($_.Exception.Message) ==="
    }
})

$btnSendCRLF.Add_Click({
    try {
        if ($serial -and $serial.IsOpen) {
            $msg = $txtCommand.Text
            if (![string]::IsNullOrWhiteSpace($msg)) {
                $serial.WriteLine($msg)
                Add-ConsoleLine ">>> $msg [CRLF]"
            }
        }
    } catch {
        Add-ConsoleLine "=== Send failed: $($_.Exception.Message) ==="
    }
})

$txtCommand.Add_KeyDown({
    if ($_.KeyCode -eq 'Enter') {
        $btnSendCRLF.PerformClick()
        $_.SuppressKeyPress = $true
    }
})

$form.Add_FormClosing({ Close-Serial })

Refresh-Ports
[void]$form.ShowDialog()
