// Settings Page Logic

// ==================== WiFi Functions ====================

async function loadWiFiStatus() {
    try {
        const status = await API.wifi.getStatus();
        const statusEl = document.getElementById('wifi-status');

        if (status.connected) {
            UI.updateStatus(statusEl, true);
            document.getElementById('wifi-ssid').value = status.ssid || '';
        } else {
            UI.updateStatus(statusEl, false);
        }
    } catch (error) {
        console.error('Failed to load WiFi status:', error);
    }
}

async function scanNetworks() {
    const button = event.target;
    UI.setButtonLoading(button, true);

    try {
        const networks = await API.wifi.scan();
        const select = document.getElementById('wifi-scan-list');

        // Clear existing options except first
        select.innerHTML = '<option value="">-- Select or type manually --</option>';

        // Add scanned networks
        networks.forEach(network => {
            const option = document.createElement('option');
            option.value = network.ssid;
            option.textContent = `${network.ssid} (${network.rssi} dBm)`;
            select.appendChild(option);
        });

        UI.showAlert('Network scan complete', 'success');
    } catch (error) {
        UI.showAlert('Failed to scan networks', 'error');
    } finally {
        UI.setButtonLoading(button, false);
    }
}

function selectNetwork() {
    const select = document.getElementById('wifi-scan-list');
    const input = document.getElementById('wifi-ssid');

    if (select.value) {
        input.value = select.value;
    }
}

async function saveWiFi() {
    const ssid = document.getElementById('wifi-ssid').value.trim();
    const password = document.getElementById('wifi-password').value;

    if (!ssid) {
        UI.showAlert('Please enter SSID', 'error');
        return;
    }

    const button = event.target;
    UI.setButtonLoading(button, true);

    try {
        await API.wifi.connect(ssid, password);
        UI.showAlert('WiFi credentials saved. Connecting...', 'success');

        // Refresh status after a delay
        setTimeout(loadWiFiStatus, 3000);
    } catch (error) {
        UI.showAlert('Failed to save WiFi credentials', 'error');
    } finally {
        UI.setButtonLoading(button, false);
    }
}

// ==================== Time Functions ====================

async function loadTimeSettings() {
    try {
        const settings = await API.time.getSettings();

        if (settings.timezone) {
            document.getElementById('timezone').value = settings.timezone;
        }

        if (settings.dst_enabled !== undefined) {
            document.getElementById('dst-enabled').checked = settings.dst_enabled;
        }

        if (settings.format_24h !== undefined) {
            const format = settings.format_24h ? '24' : '12';
            document.querySelector(`input[name="time-format"][value="${format}"]`).checked = true;
        }

        if (settings.current_time) {
            document.getElementById('current-time').textContent = settings.current_time;
        }
    } catch (error) {
        console.error('Failed to load time settings:', error);
    }
}

async function saveTime() {
    const timezone = document.getElementById('timezone').value;
    const dstEnabled = document.getElementById('dst-enabled').checked;
    const format24h = document.querySelector('input[name="time-format"]:checked').value === '24';

    const button = event.target;
    UI.setButtonLoading(button, true);

    try {
        await API.time.updateSettings({
            timezone,
            dst_enabled: dstEnabled,
            format_24h: format24h
        });

        UI.showAlert('Time settings saved', 'success');
        setTimeout(loadTimeSettings, 1000);
    } catch (error) {
        UI.showAlert('Failed to save time settings', 'error');
    } finally {
        UI.setButtonLoading(button, false);
    }
}

// ==================== Weather Functions ====================

async function loadWeatherSettings() {
    try {
        const settings = await API.weather.getSettings();

        if (settings.zipcode) {
            document.getElementById('zipcode').value = settings.zipcode;
        }

        if (settings.temp_unit) {
            document.querySelector(`input[name="temp-unit"][value="${settings.temp_unit}"]`).checked = true;
        }

        if (settings.current_weather) {
            document.getElementById('current-weather').textContent =
                `${settings.current_weather.temp}°${settings.temp_unit} - ${settings.current_weather.description}`;
        }
    } catch (error) {
        console.error('Failed to load weather settings:', error);
    }
}

async function saveWeather() {
    const zipcode = document.getElementById('zipcode').value.trim();
    const tempUnit = document.querySelector('input[name="temp-unit"]:checked').value;

    if (!zipcode) {
        UI.showAlert('Please enter zipcode', 'error');
        return;
    }

    const button = event.target;
    UI.setButtonLoading(button, true);

    try {
        await API.weather.updateSettings({
            zipcode,
            temp_unit: tempUnit
        });

        UI.showAlert('Weather settings saved', 'success');
        setTimeout(loadWeatherSettings, 1000);
    } catch (error) {
        UI.showAlert('Failed to save weather settings', 'error');
    } finally {
        UI.setButtonLoading(button, false);
    }
}

// ==================== CGM Functions ====================

async function loadCGMSettings() {
    try {
        const status = await API.cgm.getStatus();
        const authStatusEl = document.getElementById('cgm-auth-status');

        if (status.type) {
            document.getElementById('cgm-type').value = status.type;
        }

        if (status.authenticated) {
            UI.updateStatus(authStatusEl, true);
            authStatusEl.textContent = 'Authenticated';
        } else {
            UI.updateStatus(authStatusEl, false);
            authStatusEl.textContent = 'Not Authenticated';
        }

        // Show/hide Dexcom auth section
        const dexcomAuth = document.getElementById('dexcom-auth');
        dexcomAuth.style.display = status.type === 'dexcom' ? 'block' : 'none';
    } catch (error) {
        console.error('Failed to load CGM settings:', error);
    }
}

async function loginDexcom() {
    const username = document.getElementById('dexcom-username').value.trim();
    const password = document.getElementById('dexcom-password').value;

    if (!username || !password) {
        UI.showAlert('Please enter username and password', 'error');
        return;
    }

    const button = event.target;
    UI.setButtonLoading(button, true);

    try {
        await API.cgm.login(username, password);
        UI.showAlert('Login successful', 'success');

        // Clear password field
        document.getElementById('dexcom-password').value = '';

        setTimeout(loadCGMSettings, 1000);
    } catch (error) {
        UI.showAlert('Login failed. Please check credentials.', 'error');
    } finally {
        UI.setButtonLoading(button, false);
    }
}

async function logoutCGM() {
    if (!confirm('Are you sure you want to logout?')) {
        return;
    }

    try {
        await API.cgm.logout();
        UI.showAlert('Logged out successfully', 'success');
        loadCGMSettings();
    } catch (error) {
        UI.showAlert('Failed to logout', 'error');
    }
}

// Handle CGM type change
document.addEventListener('DOMContentLoaded', () => {
    const cgmTypeSelect = document.getElementById('cgm-type');
    if (cgmTypeSelect) {
        cgmTypeSelect.addEventListener('change', () => {
            const dexcomAuth = document.getElementById('dexcom-auth');
            dexcomAuth.style.display = cgmTypeSelect.value === 'dexcom' ? 'block' : 'none';
        });
    }

    // Load all settings
    loadWiFiStatus();
    loadTimeSettings();
    loadWeatherSettings();
    loadCGMSettings();
});
