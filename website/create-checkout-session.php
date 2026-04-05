<?php
header('Content-Type: application/json');
header('Access-Control-Allow-Origin: *');
header('Access-Control-Allow-Methods: POST');
header('Access-Control-Allow-Headers: Content-Type');

if ($_SERVER['REQUEST_METHOD'] === 'OPTIONS') {
    http_response_code(204);
    exit;
}

if ($_SERVER['REQUEST_METHOD'] !== 'POST') {
    http_response_code(405);
    echo json_encode(['error' => 'Method not allowed']);
    exit;
}

// Stripe secret key — replace with your real key from https://dashboard.stripe.com/apikeys
$secret_key = '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';

$input = json_decode(file_get_contents('php://input'), true);
$product_id = $input['product_id'] ?? '';
$color = $input['color'] ?? '';
$quantity = intval($input['quantity'] ?? 1);

// Map product IDs to Stripe Price IDs (create these in your Stripe Dashboard)
$prices = [
    'complete_device' => 'price_1TDEtdAEiVbSevHukrtZ3Fsh',
    'diy_kit'         => 'price_1TDF4KAEiVbSevHutT3nmD9H',
    'donate'          => 'price_1TDG6QAEiVbSevHuQWBhXjpm',
];

// Products that don't need shipping
$no_shipping = ['donate'];

$price_id = $prices[$product_id] ?? null;

if (!$price_id) {
    http_response_code(400);
    echo json_encode(['error' => 'Invalid product']);
    exit;
}

if ($quantity < 1 || $quantity > 999) {
    http_response_code(400);
    echo json_encode(['error' => 'Invalid quantity']);
    exit;
}

// Build Stripe API parameters
$params = [
    'ui_mode'                 => 'embedded',
    'mode'                    => 'payment',
    'return_url'              => 'https://cygm.me/index2.html?session_id={CHECKOUT_SESSION_ID}',
];

// Donations use price_data with dynamic amount; products use fixed price IDs
if ($product_id === 'donate') {
    $amount_cents = $quantity * 100;
    $params['line_items[0][price_data][currency]']             = 'usd';
    $params['line_items[0][price_data][product_data][name]']   = 'Donation to CYGM';
    $params['line_items[0][price_data][unit_amount]']          = $amount_cents;
    $params['line_items[0][quantity]']                          = 1;
} else {
    $params['line_items[0][price]']    = $price_id;
    $params['line_items[0][quantity]'] = 1;
}

// Only collect shipping for physical products
if (!in_array($product_id, $no_shipping)) {
    $params['shipping_address_collection[allowed_countries][0]'] = 'US';
    $params['shipping_options[0][shipping_rate]'] = 'shr_1TEJHlAEiVbSevHu2arBsniB';
}

// Pass case color as metadata on both session and payment intent
if (!empty($color)) {
    $params['metadata[case_color]'] = $color;
    $params['payment_intent_data[metadata][case_color]'] = $color;
}

$ch = curl_init('https://api.stripe.com/v1/checkout/sessions');
curl_setopt($ch, CURLOPT_POST, true);
curl_setopt($ch, CURLOPT_USERPWD, $secret_key . ':');
curl_setopt($ch, CURLOPT_POSTFIELDS, http_build_query($params));
curl_setopt($ch, CURLOPT_RETURNTRANSFER, true);
curl_setopt($ch, CURLOPT_TIMEOUT, 30);

$response = curl_exec($ch);
$http_code = curl_getinfo($ch, CURLINFO_HTTP_CODE);
$curl_error = curl_error($ch);
curl_close($ch);

if ($curl_error) {
    http_response_code(500);
    echo json_encode(['error' => 'Network error creating checkout session']);
    exit;
}

$data = json_decode($response, true);

if ($http_code !== 200 || empty($data['client_secret'])) {
    http_response_code(500);
    echo json_encode(['error' => $data['error']['message'] ?? 'Failed to create checkout session']);
    exit;
}

echo json_encode(['clientSecret' => $data['client_secret']]);
