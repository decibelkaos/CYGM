// CYGM Web Interface - Shared Utilities

// ==================== API Wrapper ====================

const API = {
    baseUrl: '',

    // Generic GET request
    async get(endpoint) {
        try {
            const response = await fetch(`${this.baseUrl}${endpoint}`);
            if (!response.ok) {
                throw new Error(`HTTP ${response.status}: ${response.statusText}`);
            }
            return await response.json();
        } catch (error) {
            console.error(`GET ${endpoint} failed:`, error);
            throw error;
        }
    },

    // Generic POST request
    async post(endpoint, data) {
        try {
            const response = await fetch(`${this.baseUrl}${endpoint}`, {
                method: 'POST',
                headers: {
                    'Content-Type': 'application/json'
                },
                body: JSON.stringify(data)
            });
            if (!response.ok) {
                throw new Error(`HTTP ${response.status}: ${response.statusText}`);
            }
            return await response.json();
        } catch (error) {
            console.error(`POST ${endpoint} failed:`, error);
            throw error;
        }
    },

    // Generic DELETE request
    async delete(endpoint) {
        try {
            const response = await fetch(`${this.baseUrl}${endpoint}`, {
                method: 'DELETE'
            });
            if (!response.ok) {
                throw new Error(`HTTP ${response.status}: ${response.statusText}`);
            }
            return await response.json();
        } catch (error) {
            console.error(`DELETE ${endpoint} failed:`, error);
            throw error;
        }
    },

    // System endpoints
    system: {
        getInfo: () => API.get('/api/system/info'),
        restart: () => API.post('/api/system/restart', {})
    },

    // WiFi endpoints
    wifi: {
        getStatus: () => API.get('/api/wifi'),
        scan: () => API.get('/api/wifi/scan'),
        connect: (ssid, password) => API.post('/api/wifi', { ssid, password })
    },

    // Time endpoints
    time: {
        getSettings: () => API.get('/api/time'),
        updateSettings: (settings) => API.post('/api/time', settings)
    },

    // Weather endpoints
    weather: {
        getSettings: () => API.get('/api/weather'),
        updateSettings: (settings) => API.post('/api/weather', settings)
    },

    // CGM endpoints
    cgm: {
        getStatus: () => API.get('/api/cgm'),
        login: (username, password) => API.post('/api/cgm/login', { username, password }),
        logout: () => API.delete('/api/cgm/auth')
    },

    // UI Layout endpoints
    ui: {
        getLayout: () => API.get('/api/ui/layout'),
        saveLayout: (layout) => API.post('/api/ui/layout', layout),
        resetLayout: () => API.get('/api/ui/defaults'),
        getScreenshot: () => API.get('/api/ui/screenshot')
    },

    // Icon endpoints
    icons: {
        getTrend: (id) => API.get(`/api/icons/trend?id=${id}`),
        saveTrend: (design) => API.post('/api/icons/trend', design),
        getWeather: (code) => API.get(`/api/icons/weather?code=${code}`),
        saveWeather: (design) => API.post('/api/icons/weather', design)
    }
};

// ==================== UI Utilities ====================

const UI = {
    // Show alert message
    showAlert(message, type = 'info') {
        const alertDiv = document.createElement('div');
        alertDiv.className = `alert alert-${type}`;
        alertDiv.textContent = message;

        const container = document.querySelector('.container');
        container.insertBefore(alertDiv, container.firstChild);

        setTimeout(() => alertDiv.remove(), 5000);
    },

    // Show loading spinner on button
    setButtonLoading(button, loading) {
        if (loading) {
            button.disabled = true;
            button.dataset.originalText = button.textContent;
            button.innerHTML = '<span class="spinner"></span> Loading...';
        } else {
            button.disabled = false;
            button.textContent = button.dataset.originalText;
        }
    },

    // Update status indicator
    updateStatus(element, connected) {
        element.className = `status ${connected ? 'status-connected' : 'status-disconnected'}`;
        element.textContent = connected ? 'Connected' : 'Disconnected';
    },

    // Format uptime
    formatUptime(seconds) {
        const days = Math.floor(seconds / 86400);
        const hours = Math.floor((seconds % 86400) / 3600);
        const minutes = Math.floor((seconds % 3600) / 60);

        const parts = [];
        if (days > 0) parts.push(`${days}d`);
        if (hours > 0) parts.push(`${hours}h`);
        if (minutes > 0) parts.push(`${minutes}m`);

        return parts.join(' ') || '0m';
    },

    // Format bytes
    formatBytes(bytes) {
        if (bytes < 1024) return `${bytes} B`;
        if (bytes < 1048576) return `${(bytes / 1024).toFixed(1)} KB`;
        return `${(bytes / 1048576).toFixed(1)} MB`;
    }
};

// ==================== Tab System ====================

function initTabs() {
    const tabs = document.querySelectorAll('.tab');
    const contents = document.querySelectorAll('.tab-content');

    tabs.forEach(tab => {
        tab.addEventListener('click', () => {
            const target = tab.dataset.tab;

            // Remove active class from all tabs and contents
            tabs.forEach(t => t.classList.remove('active'));
            contents.forEach(c => c.classList.remove('active'));

            // Add active class to clicked tab and corresponding content
            tab.classList.add('active');
            document.getElementById(target).classList.add('active');
        });
    });

    // Activate first tab by default
    if (tabs.length > 0) {
        tabs[0].click();
    }
}

// ==================== Navigation ====================

function updateActiveNav() {
    const currentPage = window.location.pathname.split('/').pop() || 'index.html';
    const navLinks = document.querySelectorAll('nav a');

    navLinks.forEach(link => {
        const href = link.getAttribute('href');
        if (href === currentPage) {
            link.classList.add('active');
        } else {
            link.classList.remove('active');
        }
    });
}

// ==================== Initialization ====================

document.addEventListener('DOMContentLoaded', () => {
    updateActiveNav();
    initTabs();
});
