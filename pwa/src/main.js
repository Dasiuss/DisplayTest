import './style.css';

const APP_VERSION = 'v0.1.0';
const SERVICE_UUID = '5f8a0001-4e56-4e46-9a7c-000000000001';
const CHARACTERISTIC_UUID = '5f8a0001-4e56-4e46-9a7c-000000000002';
const MAP_WIDTH = 40;
const MAP_HEIGHT = 40;
const MAP_ROW_BYTES = Math.ceil(MAP_WIDTH / 8);
const MAP_BYTES = MAP_ROW_BYTES * MAP_HEIGHT;
const MAP_CHUNK_BYTES = 16;
const MAP_SEND_INTERVAL = 100;

const values = {
  speed: document.querySelector('#speed'),
  heading: document.querySelector('#heading'),
  navigationAngle: document.querySelector('#navigation-angle'),
  average: document.querySelector('#average'),
  remaining: document.querySelector('#remaining'),
  total: document.querySelector('#total'),
};

const oledCanvas = document.querySelector('#oled-preview');
const oledContext = oledCanvas.getContext('2d');
const mapCanvas = document.querySelector('#map-editor');
const mapContext = mapCanvas.getContext('2d');
const mapPixels = new Uint8Array(MAP_WIDTH * MAP_HEIGHT);
let device;
let characteristic;
let mapSequence = 0;
let stateSequence = 0;
let mapTimer;
let mapDirty = false;
let isDrawing = false;
let lastDrawnPixel = -1;
let writeQueue = Promise.resolve();
let queuedWriteCount = 0;

document.querySelector('#app-version').textContent = APP_VERSION;

function clamp(value, min, max) {
  return Math.min(max, Math.max(min, value));
}

function numberValue(input, fallback = 0) {
  const value = Number.parseFloat(input.value);
  return Number.isFinite(value) ? value : fallback;
}

function angleValue(input) {
  const value = Math.round(numberValue(input));
  return ((value % 360) + 360) % 360;
}

function getState() {
  return {
    speed: clamp(numberValue(values.speed), 0, 6553.5),
    heading: angleValue(values.heading),
    navigationAngle: angleValue(values.navigationAngle),
    average: clamp(numberValue(values.average), 0, 6553.5),
    remaining: clamp(numberValue(values.remaining), 0, 6553.5),
    total: clamp(numberValue(values.total), 0, 6553.5),
  };
}

function formatNumber(value) {
  return value.toFixed(1).padStart(4, ' ');
}

function drawArrow(context, centerX, centerY, angle, radius = 7) {
  const radians = (angle * Math.PI) / 180;
  const tipX = centerX + Math.sin(radians) * radius;
  const tipY = centerY - Math.cos(radians) * radius;
  const leftX = centerX + Math.sin(radians + 2.45) * (radius - 2);
  const leftY = centerY - Math.cos(radians + 2.45) * (radius - 2);
  const rightX = centerX + Math.sin(radians - 2.45) * (radius - 2);
  const rightY = centerY - Math.cos(radians - 2.45) * (radius - 2);

  context.beginPath();
  context.moveTo(centerX, centerY);
  context.lineTo(tipX, tipY);
  context.moveTo(tipX, tipY);
  context.lineTo(leftX, leftY);
  context.moveTo(tipX, tipY);
  context.lineTo(rightX, rightY);
  context.stroke();
}

function renderOled() {
  const state = getState();
  const context = oledContext;
  context.fillStyle = '#020504';
  context.fillRect(0, 0, 128, 64);
  context.fillStyle = '#d6ffe9';
  context.strokeStyle = '#d6ffe9';
  context.lineWidth = 1;
  context.textBaseline = 'alphabetic';

  context.font = '5px monospace';
  context.fillText('SPD', 1, 6);
  context.font = 'bold 11px monospace';
  context.fillText(formatNumber(state.speed), 1, 18);

  context.font = '5px monospace';
  context.fillText('HDG', 48, 6);
  context.font = '7px monospace';
  context.fillText(String(state.heading).padStart(3, '0'), 48, 15);

  context.font = '5px monospace';
  context.fillText('REM', 1, 27);
  context.font = 'bold 10px monospace';
  context.fillText(formatNumber(state.remaining), 1, 39);
  context.font = '5px monospace';
  context.fillText('km', 32, 39);

  context.lineWidth = 1.3;
  drawArrow(context, 69, 31, state.navigationAngle, 8);

  context.strokeStyle = '#9dcfb5';
  context.beginPath();
  context.moveTo(0, 45.5);
  context.lineTo(86, 45.5);
  context.stroke();
  context.fillStyle = '#d6ffe9';
  context.font = '5px monospace';
  context.fillText(`AVG ${formatNumber(state.average)}`, 1, 54);
  context.fillText(`TOT ${formatNumber(state.total)}`, 44, 54);

  context.strokeStyle = '#9dcfb5';
  context.beginPath();
  context.moveTo(87.5, 0);
  context.lineTo(87.5, 40);
  context.stroke();
  for (let y = 0; y < MAP_HEIGHT; y += 1) {
    for (let x = 0; x < MAP_WIDTH; x += 1) {
      if (mapPixels[y * MAP_WIDTH + x]) {
        context.fillRect(88 + x, y, 1, 1);
      }
    }
  }
}

function drawMapEditor() {
  mapContext.fillStyle = '#050b09';
  mapContext.fillRect(0, 0, MAP_WIDTH, MAP_HEIGHT);
  mapContext.fillStyle = '#b9ffe0';
  for (let y = 0; y < MAP_HEIGHT; y += 1) {
    for (let x = 0; x < MAP_WIDTH; x += 1) {
      if (mapPixels[y * MAP_WIDTH + x]) {
        mapContext.fillRect(x, y, 1, 1);
      }
    }
  }
  mapContext.strokeStyle = '#4a7e6a';
  mapContext.lineWidth = 0.15;
  for (let i = 0.5; i < MAP_WIDTH; i += 1) {
    mapContext.beginPath();
    mapContext.moveTo(i, 0);
    mapContext.lineTo(i, MAP_HEIGHT);
    mapContext.stroke();
    mapContext.beginPath();
    mapContext.moveTo(0, i);
    mapContext.lineTo(MAP_WIDTH, i);
    mapContext.stroke();
  }
  updateMapCount();
}

function updateMapCount() {
  const count = mapPixels.reduce((sum, pixel) => sum + pixel, 0);
  document.querySelector('#map-count').textContent = `${count} px`;
}

function mapPoint(event) {
  const rect = mapCanvas.getBoundingClientRect();
  return {
    x: clamp(Math.floor(((event.clientX - rect.left) / rect.width) * MAP_WIDTH), 0, MAP_WIDTH - 1),
    y: clamp(Math.floor(((event.clientY - rect.top) / rect.height) * MAP_HEIGHT), 0, MAP_HEIGHT - 1),
  };
}

function paintMap(event) {
  const { x, y } = mapPoint(event);
  const index = y * MAP_WIDTH + x;
  if (index === lastDrawnPixel) return;
  lastDrawnPixel = index;
  mapPixels[index] = 1;
  mapDirty = true;
  drawMapEditor();
  renderOled();
  scheduleMapSend();
}

function encodeState() {
  const state = getState();
  const packet = new Uint8Array(15);
  const view = new DataView(packet.buffer);
  packet[0] = 0x53;
  packet[1] = stateSequence++ & 0xff;
  view.setUint16(2, Math.round(state.speed * 10), true);
  view.setUint16(4, state.heading, true);
  view.setUint16(6, state.navigationAngle, true);
  view.setUint16(8, Math.round(state.average * 10), true);
  view.setUint16(10, Math.round(state.remaining * 10), true);
  view.setUint16(12, Math.round(state.total * 10), true);
  packet[14] = 1;
  return packet;
}

function encodeMap() {
  const packed = new Uint8Array(MAP_BYTES);
  for (let y = 0; y < MAP_HEIGHT; y += 1) {
    for (let x = 0; x < MAP_WIDTH; x += 1) {
      if (mapPixels[y * MAP_WIDTH + x]) {
        packed[(y * MAP_ROW_BYTES) + Math.floor(x / 8)] |= 0x80 >> (x % 8);
      }
    }
  }
  return packed;
}

function enqueueWrite(packet) {
  if (!characteristic) return Promise.resolve();
  queuedWriteCount += 1;
  writeQueue = writeQueue
    .then(async () => {
      if (!characteristic || !device?.gatt?.connected) return;
      if (characteristic.properties.writeWithoutResponse && characteristic.writeValueWithoutResponse) {
        await characteristic.writeValueWithoutResponse(packet);
      } else {
        await characteristic.writeValueWithResponse(packet);
      }
      await new Promise((resolve) => setTimeout(resolve, 3));
    })
    .catch((error) => {
      setConnectionState('Blad transmisji', error.message, false);
    })
    .finally(() => {
      queuedWriteCount = Math.max(0, queuedWriteCount - 1);
    });
  return writeQueue;
}

function sendState() {
  if (!characteristic) return;
  enqueueWrite(encodeState());
  document.querySelector('#sync-label').textContent = 'BLE LIVE';
}

function sendMap() {
  if (!characteristic || !mapDirty) return;
  mapDirty = false;
  const packed = encodeMap();
  const sequence = mapSequence++ & 0xff;
  const chunks = Math.ceil(packed.length / MAP_CHUNK_BYTES);
  for (let index = 0; index < chunks; index += 1) {
    const start = index * MAP_CHUNK_BYTES;
    const chunk = packed.slice(start, start + MAP_CHUNK_BYTES);
    const packet = new Uint8Array(4 + MAP_CHUNK_BYTES);
    packet[0] = 0x4d;
    packet[1] = sequence;
    packet[2] = index;
    packet[3] = chunks;
    packet.set(chunk, 4);
    enqueueWrite(packet);
  }
  document.querySelector('#bitmap-status').textContent = `Wyslano / ${queuedWriteCount} pakietow`;
}

function scheduleMapSend() {
  if (mapTimer) return;
  mapTimer = window.setTimeout(() => {
    mapTimer = undefined;
    sendMap();
    if (mapDirty && characteristic) scheduleMapSend();
  }, MAP_SEND_INTERVAL);
}

function setConnectionState(title, detail, connected) {
  document.querySelector('#connection-status').textContent = title;
  document.querySelector('#connection-detail').textContent = detail;
  document.querySelector('#status-dot').classList.toggle('connected', connected);
  document.querySelector('#connect-button').disabled = connected;
  document.querySelector('#disconnect-button').disabled = !connected;
  document.querySelector('#sync-label').textContent = connected ? 'BLE LIVE' : 'LOCAL';
}

async function connectBluetooth() {
  if (!navigator.bluetooth) {
    setConnectionState('BLE niedostepne', 'Uzyj Chrome lub Edge na HTTPS albo localhost.', false);
    return;
  }

  try {
    setConnectionState('Skanowanie...', 'Wybierz HUD ESP32-S3 w oknie przegladarki.', false);
    device = await navigator.bluetooth.requestDevice({
      filters: [{ services: [SERVICE_UUID] }],
      optionalServices: [SERVICE_UUID],
    });
    device.addEventListener('gattserverdisconnected', () => {
      characteristic = undefined;
      setConnectionState('Rozlaczono', 'ESP32 jest gotowy do ponownego polaczenia.', false);
    });
    const server = await device.gatt.connect();
    const service = await server.getPrimaryService(SERVICE_UUID);
    characteristic = await service.getCharacteristic(CHARACTERISTIC_UUID);
    setConnectionState('Polaczono', `${device.name || 'HUD ESP32-S3'} / transmisja aktywna`, true);
    sendState();
    mapDirty = true;
    sendMap();
  } catch (error) {
    characteristic = undefined;
    if (error.name !== 'NotFoundError') {
      setConnectionState('Nie polaczono', error.message, false);
    } else {
      setConnectionState('Anulowano', 'Wybierz urzadzenie, aby rozpocząć.', false);
    }
  }
}

function disconnectBluetooth() {
  if (device?.gatt?.connected) device.gatt.disconnect();
}

function updateArrowReadout() {
  const angle = angleValue(values.navigationAngle);
  document.querySelector('#angle-readout').textContent = angle;
  document.querySelector('#compass-arrow').style.transform = `rotate(${angle}deg)`;
}

function onValueInput() {
  updateArrowReadout();
  renderOled();
  sendState();
}

function clearMap() {
  mapPixels.fill(0);
  mapDirty = true;
  drawMapEditor();
  renderOled();
  scheduleMapSend();
}

function invertMap() {
  for (let index = 0; index < mapPixels.length; index += 1) mapPixels[index] = mapPixels[index] ? 0 : 1;
  mapDirty = true;
  drawMapEditor();
  renderOled();
  scheduleMapSend();
}

document.querySelector('#connect-button').addEventListener('click', connectBluetooth);
document.querySelector('#disconnect-button').addEventListener('click', disconnectBluetooth);
document.querySelector('#clear-map').addEventListener('click', clearMap);
document.querySelector('#invert-map').addEventListener('click', invertMap);
Object.values(values).forEach((input) => input.addEventListener('input', onValueInput));

mapCanvas.addEventListener('pointerdown', (event) => {
  event.preventDefault();
  isDrawing = true;
  lastDrawnPixel = -1;
  mapCanvas.setPointerCapture(event.pointerId);
  paintMap(event);
});
mapCanvas.addEventListener('pointermove', (event) => {
  if (isDrawing) paintMap(event);
});
mapCanvas.addEventListener('pointerup', () => {
  isDrawing = false;
  lastDrawnPixel = -1;
  if (mapDirty) sendMap();
});
mapCanvas.addEventListener('pointercancel', () => {
  isDrawing = false;
  lastDrawnPixel = -1;
});

setConnectionState('Niepolaczono', 'Wybierz ESP32-S3, aby rozpocząć transmisję.', false);
updateArrowReadout();
drawMapEditor();
renderOled();

if ('serviceWorker' in navigator) {
  navigator.serviceWorker.register('./sw.js').catch(() => undefined);
}
