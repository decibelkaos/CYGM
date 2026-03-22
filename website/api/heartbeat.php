<?php
// Device heartbeat endpoint — tracks online CYGM devices.
// GET ?id=XXXX  — record heartbeat for device, return online count
// GET (no id)   — return online count only (used by website)

$dataFile = __DIR__ . '/devices.json';
$timeout = 30 * 60; // 30 minutes = considered "online"

// Read existing device data
$devices = [];
if (file_exists($dataFile)) {
    $raw = @file_get_contents($dataFile);
    if ($raw !== false) {
        $devices = json_decode($raw, true) ?: [];
    }
}

// Record heartbeat if device ID provided
$id = isset($_GET['id']) ? preg_replace('/[^A-Fa-f0-9]/', '', $_GET['id']) : '';
if ($id !== '') {
    $devices[$id] = time();
}

// Prune devices not seen within timeout
$now = time();
foreach ($devices as $devId => $ts) {
    if (($now - $ts) >= $timeout) {
        unset($devices[$devId]);
    }
}

// Save (atomic write to avoid corruption)
$tmp = $dataFile . '.tmp';
if (file_put_contents($tmp, json_encode($devices)) !== false) {
    rename($tmp, $dataFile);
}

// Return count
header('Content-Type: application/json');
header('Access-Control-Allow-Origin: *');
header('Cache-Control: no-cache, no-store, must-revalidate');
echo json_encode(['count' => count($devices)]);
