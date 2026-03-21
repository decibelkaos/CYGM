<?php
header('Content-Type: application/json');

// Stripe secret key — must match create-checkout-session.php
$secret_key = 'sk_test_51TDDbWAEiVbSevHuZQ167ndm5e17KetCSkNcBII29oc2OgozVPUUeanHjzewfQAjODrfJHeOioiOe0BTO9ul7nso00LAvHvlYv';

$session_id = $_GET['session_id'] ?? '';

if (empty($session_id)) {
    http_response_code(400);
    echo json_encode(['error' => 'Missing session_id']);
    exit;
}

// Sanitize session ID — should only contain alphanumeric, underscores, and hyphens
if (!preg_match('/^cs_[a-zA-Z0-9_-]+$/', $session_id)) {
    http_response_code(400);
    echo json_encode(['error' => 'Invalid session_id format']);
    exit;
}

$ch = curl_init('https://api.stripe.com/v1/checkout/sessions/' . urlencode($session_id));
curl_setopt($ch, CURLOPT_USERPWD, $secret_key . ':');
curl_setopt($ch, CURLOPT_RETURNTRANSFER, true);
curl_setopt($ch, CURLOPT_TIMEOUT, 15);

$response = curl_exec($ch);
$http_code = curl_getinfo($ch, CURLINFO_HTTP_CODE);
$curl_error = curl_error($ch);
curl_close($ch);

if ($curl_error || $http_code !== 200) {
    http_response_code(500);
    echo json_encode(['error' => 'Failed to retrieve session']);
    exit;
}

$data = json_decode($response, true);

// Expand line items to determine product type (donation vs device)
$line_items_url = 'https://api.stripe.com/v1/checkout/sessions/' . urlencode($session_id) . '/line_items';
$ch2 = curl_init($line_items_url);
curl_setopt($ch2, CURLOPT_USERPWD, $secret_key . ':');
curl_setopt($ch2, CURLOPT_RETURNTRANSFER, true);
curl_setopt($ch2, CURLOPT_TIMEOUT, 10);
$li_response = curl_exec($ch2);
curl_close($ch2);

$li_data = json_decode($li_response, true);
$product_name = $li_data['data'][0]['description'] ?? '';
$is_donation = (stripos($product_name, 'donation') !== false || stripos($product_name, 'donate') !== false);

echo json_encode([
    'status'         => $data['status'] ?? 'unknown',
    'payment_status' => $data['payment_status'] ?? 'unknown',
    'customer_email' => $data['customer_details']['email'] ?? '',
    'is_donation'    => $is_donation,
]);
