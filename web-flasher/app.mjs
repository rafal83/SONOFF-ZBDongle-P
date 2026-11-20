// SPDX-License-Identifier: MIT
import { TiRom, APP_END, FLASH_SIZE, CCFG, installApplication, restoreApplication, encodeBackup, decodeBackup, sha256, validateApplication } from './rom.mjs';

const $ = id => document.getElementById(id);
const ui = Object.fromEntries(['baud', 'consent', 'connect', 'install', 'disconnect', 'status', 'progress',
  'percent', 'log', 'compatibility', 'device-info', 'backup-link', 'result-help', 'release-name',
  'release-link', 'firmware-hash', 'hex-link', 'operation', 'install-settings', 'restore-settings',
  'restore-file', 'restore-info', 'restore-hash', 'restore-controls', 'restore-consent', 'restore'].map(id => [id, $(id)]));
const state = { busy: false, catalog: null, image: null, binary: null, rom: null, backup: null, backupUrl: null, failedWrite: false, restoreImage: null };
const supported = 'serial' in navigator && window.isSecureContext && Boolean(crypto.subtle);
function log(message) { ui.log.textContent += `${new Date().toLocaleTimeString('fr-FR')}  ${message}\n`; }
function status(message, percent) {
  ui.status.textContent = message;
  if (percent !== undefined) { ui.progress.value = percent; ui.percent.textContent = `${Math.round(percent)} %`; }
}
function update() {
  const restoring = ui.operation.value === 'restore';
  const ready = supported && !state.busy && state.rom && state.backup && ui.consent.checked && !state.failedWrite;
  ui.connect.disabled = !supported || state.busy || (!restoring && !state.binary) || !ui.consent.checked || Boolean(state.rom);
  ui.install.disabled = !ready || restoring || !state.binary;
  ui.restore.disabled = !ready || !restoring || !state.restoreImage || !ui['restore-consent'].checked || state.restoreImage.ieee !== state.rom?.device?.ieee;
  ui['install-settings'].hidden = restoring;
  ui['restore-settings'].hidden = !restoring;
  ui['restore-controls'].hidden = !restoring;
  ui.install.hidden = restoring;
  ui.operation.disabled = state.busy;
  ui['restore-file'].disabled = state.busy;
  ui['restore-consent'].disabled = state.busy;
  ui.baud.disabled = state.busy || Boolean(state.rom) || !state.catalog;
  ui.consent.disabled = state.busy;
  ui.disconnect.hidden = !state.rom || state.busy;
}
function errorMessage(error) {
  if (error.name === 'NotFoundError') return 'Sélection annulée. Aucun dongle modifié.';
  if (error.name === 'NetworkError' || error.name === 'InvalidStateError') return 'Port USB indisponible. Arrêtez Zigbee2MQTT ou toute autre application qui utilise le dongle.';
  return error.message || String(error);
}
function validateCatalog(catalog) {
  if (catalog.schema !== 1 || catalog.chip !== 'CC2652P' || !/^[a-f0-9]{40}$/.test(catalog.commit) ||
      !Array.isArray(catalog.ccfg) || catalog.ccfg.length !== FLASH_SIZE - CCFG ||
      !catalog.ccfg.every(b => Number.isInteger(b) && b >= 0 && b <= 255) ||
      !Array.isArray(catalog.images) || catalog.images.length !== 2) throw new Error('Catalogue de firmware invalide.');
  for (const image of catalog.images) {
    if (![115200, 1000000].includes(image.baud) || image.path !== `firmware/sonoff-freertos-${image.baud}.bin` ||
        image.hex !== `firmware/sonoff-freertos-${image.baud}.hex` || !/^[a-f0-9]{64}$/.test(image.sha256)) {
      throw new Error('Référence de firmware invalide.');
    }
  }
  if (new Set(catalog.images.map(i => i.baud)).size !== 2) throw new Error('Variantes de firmware invalides.');
}
async function loadFirmware() {
  state.busy = true;
  state.binary = null;
  update();
  status('Vérification du firmware…', 0);
  try {
    state.image = state.catalog.images.find(i => i.baud === Number(ui.baud.value));
    const response = await fetch(new URL(state.image.path, import.meta.url), { cache: 'no-store' });
    if (!response.ok) throw new Error('Téléchargement du firmware impossible. Rechargez la page.');
    const binary = new Uint8Array(await response.arrayBuffer());
    if (binary.length !== APP_END || await sha256(binary) !== state.image.sha256) throw new Error('Le firmware reçu ne correspond pas à son SHA-256. Rechargez la page.');
    validateApplication(binary);
    state.binary = binary;
    ui['firmware-hash'].textContent = state.image.sha256;
    ui['hex-link'].href = state.image.hex;
    ui['hex-link'].hidden = false;
    status(supported ? 'Firmware vérifié. Vous pouvez connecter votre dongle.' : 'Firmware vérifié. Navigateur compatible requis.', 0);
    log(`Firmware ${state.image.baud} bauds : SHA-256 vérifié.`);
  } catch (error) { status(errorMessage(error)); log(errorMessage(error)); }
  finally { state.busy = false; update(); }
}
async function saveBackup(bytes, ieee) {
  if (state.backupUrl) URL.revokeObjectURL(state.backupUrl);
  state.backupUrl = URL.createObjectURL(new Blob([await encodeBackup(bytes, ieee)], { type: 'application/json' }));
  ui['backup-link'].href = state.backupUrl;
  ui['backup-link'].download = `zbdongle-p-${ieee}-${new Date().toISOString().replaceAll(':', '-')}.json`;
  ui['backup-link'].hidden = false;
  ui['backup-link'].click();
}
async function disconnect({ reboot = true } = {}) {
  const rom = state.rom;
  state.rom = null;
  state.backup = null;
  if (rom) {
    try { if (reboot && rom.device) await rom.reset(); }
    finally { await rom.close(); }
  }
  ui['device-info'].textContent = 'Aucun dongle connecté';
  update();
}
ui.consent.addEventListener('change', update);
ui['restore-consent'].addEventListener('change', update);
ui.operation.addEventListener('change', () => { ui['restore-consent'].checked = false; update(); });
ui['restore-file'].addEventListener('change', async () => {
  state.restoreImage = null;
  ui['restore-consent'].checked = false;
  ui['restore-hash'].textContent = 'Non vérifié';
  ui['result-help'].textContent = '';
  const file = ui['restore-file'].files[0];
  if (!file) { ui['restore-info'].textContent = 'Aucune sauvegarde sélectionnée.'; update(); return; }
  state.busy = true; update();
  try {
    if (file.size > 1024 * 1024) throw new Error('Fichier de sauvegarde trop volumineux.');
    const saved = await decodeBackup(new Uint8Array(await file.arrayBuffer()), file.name);
    state.restoreImage = saved;
    ui['restore-info'].textContent = `IEEE ${saved.ieee} · ${saved.baud} bauds · révision ${saved.revision}. ${saved.legacy ? 'Ancien BIN : aucune somme de contrôle indépendante ; utilisez votre fichier original.' : 'Intégrité SHA-256 vérifiée.'}`;
    ui['restore-hash'].textContent = saved.hash;
    if (state.rom && saved.ieee !== state.rom.device.ieee) throw new Error('Cette sauvegarde appartient à un autre dongle.');
    ui['result-help'].textContent = state.rom ? 'Sauvegarde compatible avec ce dongle. Confirmez la restauration pour continuer.' : 'Connectez le dongle et sauvegardez son état actuel avant de restaurer le firmware.';
    log('Sauvegarde de restauration chargée localement. Aucune écriture effectuée.');
  } catch (error) { state.restoreImage = null; ui['restore-info'].textContent = errorMessage(error); log(errorMessage(error)); }
  finally { state.busy = false; update(); }
});
ui.baud.addEventListener('change', loadFirmware);
ui.connect.addEventListener('click', async () => {
  if (ui.connect.disabled) return;
  state.busy = true;
  state.failedWrite = false;
  update();
  ui['result-help'].textContent = '';
  let rom;
  try {
    // requestPort is called directly from the click: the browser owns device consent.
    const port = await navigator.serial.requestPort({ filters: [{ usbVendorId: 0x10c4, usbProductId: 0xea60 }] });
    rom = new TiRom(port);
    state.rom = rom;
    status('Connexion au bootloader…', 0);
    await rom.open();
    const device = await rom.identify();
    ui['device-info'].textContent = `${device.model} · IEEE ${device.ieee}`;
    log(`${device.model} identifié, mémoire de 352 Kio.`);
    status('Sauvegarde de la mémoire du dongle…', 0);
    const backup = await rom.backup(p => status('Sauvegarde de la mémoire du dongle…', p * 100));
    if (ui.operation.value === 'install' && state.catalog.ccfg.some((b, i) => b !== backup[CCFG + i])) throw new Error('Configuration du dongle incompatible avec ce firmware. Aucun flash effectué.');
    state.backup = backup;
    await saveBackup(backup, device.ieee);
    status('Sauvegarde vérifiée. Prêt à installer ou restaurer.', 0);
    ui['result-help'].textContent = state.restoreImage && state.restoreImage.ieee !== device.ieee ? 'La sauvegarde sélectionnée appartient à un autre dongle. Choisissez celle de ce dongle.' : 'Conservez le fichier JSON téléchargé : il contient une copie complète du dongle, son identité et une somme de contrôle, y compris ses données privées.';
    log('Sauvegarde complète vérifiée avec le CRC du dongle et proposée au téléchargement.');
  } catch (error) {
    const message = errorMessage(error);
    status(message, 0); log(message);
    try { await disconnect(); } catch (closeError) { log(errorMessage(closeError)); }
    if (rom && !state.rom) { try { await rom.close(); } catch {} }
  } finally { state.busy = false; update(); }
});
async function program(restoring) {
  if ((restoring ? ui.restore : ui.install).disabled) return;
  state.busy = true;
  update();
  ui['result-help'].textContent = 'Gardez le dongle branché et cette page ouverte pendant l’écriture.';
  let wrote = false;
  try {
    const progress = (phase, fraction) => {
      if (phase === 'erase') wrote = true;
      const stages = { erase: ['Effacement de la zone application…', 0, 20], program: ['Écriture du firmware…', 20, 50], verify: ['Relecture et vérification du dongle…', 70, 29], verified: ['Flash et données conservées vérifiés.', 100, 0] };
      const [label, base, span] = stages[phase];
      status(label, base + fraction * span);
    };
    let info;
    if (restoring) {
      info = await restoreApplication(state.rom, state.restoreImage, state.backup, progress);
    } else {
      await installApplication(state.rom, state.binary, state.image.sha256, state.backup, state.catalog.ccfg, progress);
      info = state.image;
    }
    log('Application relue intégralement ; stockage Zigbee, NV et CCFG inchangés. CRC final vérifié.');
    await disconnect();
    status(`${restoring ? 'Restauration' : 'Installation'} terminée. Vous pouvez débrancher le dongle.`, 100);
    ui['result-help'].textContent = `Vous pouvez reconnecter le dongle à Zigbee2MQTT avec adapter: zstack et baudrate: ${info.baud}. Les données et compteurs Zigbee actuels sont conservés.`;
    log('Commande de redémarrage acquittée. Le fonctionnement radio n’est pas vérifié par cette page.');
  } catch (error) {
    const message = errorMessage(error);
    state.failedWrite = wrote;
    status(wrote ? `Écriture interrompue : ${message}` : message);
    ui['result-help'].textContent = wrote ? 'La vérification n’est pas terminée. Reconnectez le dongle à cette page pour relancer l’opération ; conservez la sauvegarde initiale.' : 'Aucune écriture effectuée. Déconnectez puis reconnectez le dongle pour réessayer.';
    log(message);
    // Never ask an incompletely programmed image to boot.
    try { await disconnect({ reboot: false }); } catch (closeError) { log(errorMessage(closeError)); }
  } finally { state.busy = false; update(); }
}
ui.install.addEventListener('click', () => program(false));
ui.restore.addEventListener('click', () => program(true));
ui.disconnect.addEventListener('click', async () => {
  state.busy = true; update();
  try { await disconnect({ reboot: !state.failedWrite }); status('Dongle déconnecté.', 0); }
  catch (error) { status(errorMessage(error)); }
  finally { state.busy = false; update(); }
});
window.addEventListener('beforeunload', event => {
  if (state.busy || state.rom) { event.preventDefault(); event.returnValue = ''; }
});
if (!supported) {
  ui.compatibility.classList.add('error');
  ui.compatibility.textContent = 'Pour flasher par USB, ouvrez cette page HTTPS dans Chrome ou Edge sur ordinateur. Vous pouvez aussi télécharger le fichier HEX.';
}
try {
  const response = await fetch('./firmware.json', { cache: 'no-store' });
  if (!response.ok) throw new Error('Le catalogue de firmware est indisponible.');
  const catalog = await response.json();
  validateCatalog(catalog);
  state.catalog = catalog;
  ui['release-name'].textContent = catalog.commit.slice(0, 12);
  ui['release-link'].href = `https://github.com/rafal83/SONOFF-ZBDongle-P/releases/tag/freertos-${catalog.commit.slice(0, 12)}`;
  ui['release-link'].hidden = false;
  await loadFirmware();
} catch (error) {
  ui['release-name'].textContent = 'Version indisponible';
  ui['firmware-hash'].textContent = 'Non vérifié';
  status(errorMessage(error)); log(errorMessage(error)); update();
}
