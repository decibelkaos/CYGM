// UI Layout Editor Logic

let currentLayout = null;
let selectedElement = null;
let previewRefreshInterval = null;

// ==================== Preview Functions ====================

async function refreshUIPreview() {
    const canvas = document.getElementById('ui-preview-canvas');
    if (!canvas) return;

    const ctx = canvas.getContext('2d');

    try {
        const screenshot = await API.ui.getScreenshot();

        if (screenshot.image) {
            const img = new Image();
            img.onload = () => {
                ctx.drawImage(img, 0, 0, canvas.width, canvas.height);
            };
            img.src = 'data:image/png;base64,' + screenshot.image;
        }
    } catch (error) {
        console.error('Failed to refresh preview:', error);

        // Draw placeholder
        ctx.fillStyle = '#1F2937';
        ctx.fillRect(0, 0, canvas.width, canvas.height);
        ctx.fillStyle = '#9CA3AF';
        ctx.font = '14px sans-serif';
        ctx.textAlign = 'center';
        ctx.fillText('Preview not available', canvas.width/2, canvas.height/2);
    }
}

// ==================== Layout Functions ====================

async function loadLayout() {
    try {
        const layout = await API.ui.getLayout();
        currentLayout = layout;

        // Populate element list
        const elementList = document.getElementById('element-list');
        if (!elementList) return;

        elementList.innerHTML = '';

        if (layout.elements) {
            layout.elements.forEach(element => {
                const li = document.createElement('li');
                li.className = 'element-item';
                li.textContent = element.id;
                li.dataset.elementId = element.id;
                li.onclick = () => selectElementById(element.id);
                elementList.appendChild(li);
            });
        }
    } catch (error) {
        console.error('Failed to load layout:', error);
        UI.showAlert('Failed to load layout', 'error');
    }
}

function selectElementById(elementId) {
    if (!currentLayout || !currentLayout.elements) return;

    const element = currentLayout.elements.find(e => e.id === elementId);
    if (!element) return;

    selectedElement = element;

    // Update UI
    document.getElementById('selected-element-name').textContent = element.id;
    document.getElementById('selection-box').style.display = 'block';

    // Populate form
    document.getElementById('elem-x').value = element.x;
    document.getElementById('elem-y').value = element.y;
    document.getElementById('elem-width').value = element.width;
    document.getElementById('elem-height').value = element.height;

    // Convert color number to hex
    const colorHex = '#' + ('000000' + element.color.toString(16)).slice(-6);
    document.getElementById('elem-color').value = colorHex;

    // Highlight in list
    document.querySelectorAll('.element-item').forEach(item => {
        item.classList.remove('selected');
        if (item.dataset.elementId === elementId) {
            item.classList.add('selected');
        }
    });
}

function applyElementChanges() {
    if (!selectedElement) {
        UI.showAlert('No element selected', 'error');
        return;
    }

    // Update selected element
    selectedElement.x = parseInt(document.getElementById('elem-x').value);
    selectedElement.y = parseInt(document.getElementById('elem-y').value);
    selectedElement.width = parseInt(document.getElementById('elem-width').value);
    selectedElement.height = parseInt(document.getElementById('elem-height').value);

    // Convert hex color to number
    const colorHex = document.getElementById('elem-color').value;
    selectedElement.color = parseInt(colorHex.substring(1), 16);

    UI.showAlert('Changes applied (click Save Layout to persist)', 'success');
    refreshUIPreview();
}

function cancelElementEdit() {
    selectedElement = null;
    document.getElementById('selection-box').style.display = 'none';

    document.querySelectorAll('.element-item').forEach(item => {
        item.classList.remove('selected');
    });

    // Clear form
    document.getElementById('elem-x').value = '';
    document.getElementById('elem-y').value = '';
    document.getElementById('elem-width').value = '';
    document.getElementById('elem-height').value = '';
}

async function saveCurrentLayout() {
    if (!currentLayout) {
        UI.showAlert('No layout to save', 'error');
        return;
    }

    const button = event.target;
    UI.setButtonLoading(button, true);

    try {
        await API.ui.saveLayout(currentLayout);
        UI.showAlert('Layout saved successfully', 'success');
        setTimeout(refreshUIPreview, 500);
    } catch (error) {
        console.error('Failed to save layout:', error);
        UI.showAlert('Failed to save layout', 'error');
    } finally {
        UI.setButtonLoading(button, false);
    }
}

async function resetLayout() {
    if (!confirm('Are you sure you want to reset to the default layout? This cannot be undone.')) {
        return;
    }

    try {
        await API.ui.resetLayout();
        UI.showAlert('Layout reset to defaults', 'success');
        await loadLayout();
        cancelElementEdit();
        setTimeout(refreshUIPreview, 500);
    } catch (error) {
        console.error('Failed to reset layout:', error);
        UI.showAlert('Failed to reset layout', 'error');
    }
}

function exportLayout() {
    if (!currentLayout) {
        UI.showAlert('No layout to export', 'error');
        return;
    }

    const json = JSON.stringify(currentLayout, null, 2);
    const blob = new Blob([json], { type: 'application/json' });
    const url = URL.createObjectURL(blob);

    const a = document.createElement('a');
    a.href = url;
    a.download = 'cygm-layout.json';
    a.click();

    URL.revokeObjectURL(url);
    UI.showAlert('Layout exported', 'success');
}

// ==================== Canvas Interaction ====================

function initCanvasInteraction() {
    const canvas = document.getElementById('ui-preview-canvas');
    if (!canvas) return;

    canvas.addEventListener('click', (e) => {
        if (!currentLayout || !currentLayout.elements) return;

        const rect = canvas.getBoundingClientRect();
        const x = Math.floor((e.clientX - rect.left) * (canvas.width / rect.width));
        const y = Math.floor((e.clientY - rect.top) * (canvas.height / rect.height));

        // Find element at click position
        for (let i = currentLayout.elements.length - 1; i >= 0; i--) {
            const elem = currentLayout.elements[i];
            if (x >= elem.x && x <= elem.x + elem.width &&
                y >= elem.y && y <= elem.y + elem.height) {
                selectElementById(elem.id);
                break;
            }
        }
    });
}

// ==================== Initialization ====================

if (document.getElementById('tab-ui-editor')) {
    document.addEventListener('DOMContentLoaded', () => {
        loadLayout();
        refreshUIPreview();
        initCanvasInteraction();

        // Auto-refresh preview every 3 seconds
        previewRefreshInterval = setInterval(refreshUIPreview, 3000);
    });

    // Stop auto-refresh when leaving page
    window.addEventListener('beforeunload', () => {
        if (previewRefreshInterval) {
            clearInterval(previewRefreshInterval);
        }
    });
}
