// Icon Editor Logic

let iconCanvas, iconCtx;
let currentTool = 'pen';
let currentPaths = [];
let currentPath = null;
let isDrawing = false;
let startPoint = null;
let iconMode = 'trend';
let currentIconId = '';

// Icon IDs
const trendIcons = [
    'single_up', 'double_up', 'forty_five_up',
    'flat', 'forty_five_down', 'single_down', 'double_down'
];

const weatherIcons = [
    'sunny', 'cloudy', 'rainy', 'snowy', 'partly_cloudy'
];

// ==================== Initialization ====================

function initIconEditor() {
    iconCanvas = document.getElementById('icon-canvas');
    if (!iconCanvas) return;

    iconCtx = iconCanvas.getContext('2d');

    // Set up canvas
    iconCtx.fillStyle = '#FFFFFF';
    iconCtx.fillRect(0, 0, iconCanvas.width, iconCanvas.height);

    // Add event listeners
    iconCanvas.addEventListener('mousedown', handleMouseDown);
    iconCanvas.addEventListener('mousemove', handleMouseMove);
    iconCanvas.addEventListener('mouseup', handleMouseUp);
    iconCanvas.addEventListener('mouseleave', handleMouseUp);

    // Initialize icon selector
    changeIconMode();
}

// ==================== Mode Functions ====================

function changeIconMode() {
    const select = document.getElementById('icon-mode');
    iconMode = select.value;

    // Populate icon ID dropdown
    const iconIdSelect = document.getElementById('icon-id');
    iconIdSelect.innerHTML = '';

    const icons = iconMode === 'trend' ? trendIcons : weatherIcons;
    icons.forEach(id => {
        const option = document.createElement('option');
        option.value = id;
        option.textContent = id.replace(/_/g, ' ').replace(/\b\w/g, c => c.toUpperCase());
        iconIdSelect.appendChild(option);
    });

    currentIconId = icons[0];
    clearCanvas();
}

async function loadIconDesign() {
    const iconIdSelect = document.getElementById('icon-id');
    currentIconId = iconIdSelect.value;

    try {
        let design;
        if (iconMode === 'trend') {
            design = await API.icons.getTrend(currentIconId);
        } else {
            design = await API.icons.getWeather(currentIconId);
        }

        if (design && design.paths) {
            currentPaths = design.paths;
            redrawCanvas();
            updatePathList();
        }
    } catch (error) {
        console.log('No custom design found, starting fresh');
        clearCanvas();
    }
}

// ==================== Tool Functions ====================

function selectTool(tool) {
    currentTool = tool;

    // Update button states
    document.querySelectorAll('.tool-btn').forEach(btn => {
        btn.classList.remove('active');
        if (btn.dataset.tool === tool) {
            btn.classList.add('active');
        }
    });
}

function updateStrokeWidth() {
    const slider = document.getElementById('stroke-width');
    const value = document.getElementById('stroke-width-value');
    value.textContent = slider.value;
}

// ==================== Drawing Functions ====================

function handleMouseDown(e) {
    const rect = iconCanvas.getBoundingClientRect();
    const x = Math.floor(e.clientX - rect.left);
    const y = Math.floor(e.clientY - rect.top);

    isDrawing = true;
    startPoint = { x, y };

    const color = document.getElementById('stroke-color').value;
    const width = parseInt(document.getElementById('stroke-width').value);
    const roundCaps = document.getElementById('round-caps').checked;

    currentPath = {
        type: currentTool,
        points: [[x, y]],
        width: width,
        color: color,
        round_caps: roundCaps
    };

    if (currentTool === 'pen') {
        // Start drawing immediately for pen
        iconCtx.strokeStyle = color;
        iconCtx.lineWidth = width;
        iconCtx.lineCap = roundCaps ? 'round' : 'square';
        iconCtx.lineJoin = 'round';
        iconCtx.beginPath();
        iconCtx.moveTo(x, y);
    }
}

function handleMouseMove(e) {
    if (!isDrawing) return;

    const rect = iconCanvas.getBoundingClientRect();
    const x = Math.floor(e.clientX - rect.left);
    const y = Math.floor(e.clientY - rect.top);

    if (currentTool === 'pen') {
        // Freehand drawing
        iconCtx.lineTo(x, y);
        iconCtx.stroke();
        currentPath.points.push([x, y]);
    } else {
        // Preview for other tools
        redrawCanvas();
        previewShape(startPoint.x, startPoint.y, x, y);
    }
}

function handleMouseUp(e) {
    if (!isDrawing) return;

    const rect = iconCanvas.getBoundingClientRect();
    const x = Math.floor(e.clientX - rect.left);
    const y = Math.floor(e.clientY - rect.top);

    if (currentTool === 'pen') {
        // Finish pen stroke
        iconCtx.closePath();
    } else {
        // Add shape
        currentPath.points.push([x, y]);
        drawShape(currentPath);
    }

    // Add path to list
    currentPaths.push(currentPath);
    updatePathList();

    isDrawing = false;
    currentPath = null;
    startPoint = null;
}

function previewShape(x1, y1, x2, y2) {
    const color = document.getElementById('stroke-color').value;
    const width = parseInt(document.getElementById('stroke-width').value);
    const roundCaps = document.getElementById('round-caps').checked;

    iconCtx.strokeStyle = color;
    iconCtx.fillStyle = color;
    iconCtx.lineWidth = width;
    iconCtx.lineCap = roundCaps ? 'round' : 'square';

    switch (currentTool) {
        case 'line':
            iconCtx.beginPath();
            iconCtx.moveTo(x1, y1);
            iconCtx.lineTo(x2, y2);
            iconCtx.stroke();
            break;

        case 'rect':
            iconCtx.strokeRect(x1, y1, x2 - x1, y2 - y1);
            break;

        case 'arc':
            const radius = Math.sqrt(Math.pow(x2 - x1, 2) + Math.pow(y2 - y1, 2));
            iconCtx.beginPath();
            iconCtx.arc(x1, y1, radius, 0, Math.PI * 2);
            iconCtx.stroke();
            break;
    }
}

function drawShape(path) {
    const color = path.color;
    const width = path.width;
    const roundCaps = path.round_caps;

    iconCtx.strokeStyle = color;
    iconCtx.fillStyle = color;
    iconCtx.lineWidth = width;
    iconCtx.lineCap = roundCaps ? 'round' : 'square';
    iconCtx.lineJoin = 'round';

    switch (path.type) {
        case 'pen':
            iconCtx.beginPath();
            iconCtx.moveTo(path.points[0][0], path.points[0][1]);
            for (let i = 1; i < path.points.length; i++) {
                iconCtx.lineTo(path.points[i][0], path.points[i][1]);
            }
            iconCtx.stroke();
            break;

        case 'line':
            if (path.points.length >= 2) {
                iconCtx.beginPath();
                iconCtx.moveTo(path.points[0][0], path.points[0][1]);
                iconCtx.lineTo(path.points[1][0], path.points[1][1]);
                iconCtx.stroke();
            }
            break;

        case 'rect':
            if (path.points.length >= 2) {
                const x = path.points[0][0];
                const y = path.points[0][1];
                const w = path.points[1][0] - x;
                const h = path.points[1][1] - y;
                iconCtx.strokeRect(x, y, w, h);
            }
            break;

        case 'arc':
            if (path.points.length >= 2) {
                const x1 = path.points[0][0];
                const y1 = path.points[0][1];
                const x2 = path.points[1][0];
                const y2 = path.points[1][1];
                const radius = Math.sqrt(Math.pow(x2 - x1, 2) + Math.pow(y2 - y1, 2));
                iconCtx.beginPath();
                iconCtx.arc(x1, y1, radius, 0, Math.PI * 2);
                iconCtx.stroke();
            }
            break;
    }
}

function redrawCanvas() {
    // Clear canvas
    iconCtx.fillStyle = '#FFFFFF';
    iconCtx.fillRect(0, 0, iconCanvas.width, iconCanvas.height);

    // Redraw all paths
    currentPaths.forEach(path => drawShape(path));
}

function clearCanvas() {
    currentPaths = [];
    iconCtx.fillStyle = '#FFFFFF';
    iconCtx.fillRect(0, 0, iconCanvas.width, iconCanvas.height);
    updatePathList();
}

function undoPath() {
    if (currentPaths.length > 0) {
        currentPaths.pop();
        redrawCanvas();
        updatePathList();
    }
}

// ==================== Path Management ====================

function updatePathList() {
    const pathList = document.getElementById('path-list');
    pathList.innerHTML = '';

    currentPaths.forEach((path, index) => {
        const li = document.createElement('li');
        li.className = 'element-item';
        li.textContent = `${path.type} (${path.points.length} points)`;

        const deleteBtn = document.createElement('button');
        deleteBtn.textContent = 'Delete';
        deleteBtn.className = 'btn btn-danger';
        deleteBtn.style.marginLeft = '10px';
        deleteBtn.style.padding = '2px 8px';
        deleteBtn.style.fontSize = '0.8rem';
        deleteBtn.onclick = () => deletePath(index);

        li.appendChild(deleteBtn);
        pathList.appendChild(li);
    });
}

function deletePath(index) {
    currentPaths.splice(index, 1);
    redrawCanvas();
    updatePathList();
}

// ==================== Save/Load Functions ====================

async function saveIconDesign() {
    if (currentPaths.length === 0) {
        UI.showAlert('Nothing to save', 'error');
        return;
    }

    const design = {
        id: currentIconId,
        canvas_width: iconMode === 'trend' ? 40 : 60,
        canvas_height: iconMode === 'trend' ? 40 : 50,
        paths: currentPaths
    };

    const button = event.target;
    UI.setButtonLoading(button, true);

    try {
        if (iconMode === 'trend') {
            await API.icons.saveTrend(design);
        } else {
            await API.icons.saveWeather(design);
        }

        UI.showAlert('Icon design saved successfully', 'success');
    } catch (error) {
        console.error('Failed to save icon design:', error);
        UI.showAlert('Failed to save icon design', 'error');
    } finally {
        UI.setButtonLoading(button, false);
    }
}

// ==================== Initialization ====================

if (document.getElementById('icon-canvas')) {
    document.addEventListener('DOMContentLoaded', () => {
        initIconEditor();
    });
}
