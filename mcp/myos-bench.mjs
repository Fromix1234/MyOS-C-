#!/usr/bin/env node
// ===========================================================================
//  myos-bench.mjs — MCP-сервер стенда разработки MyOS (QEMU + снимки экрана)
// ===========================================================================
//  Zero-dependency MCP stdio server (JSON-RPC 2.0, одно сообщение на строку).
//  Даёт агенту инструменты вместо ручных PowerShell-простыней:
//
//    qemu_start / qemu_stop / qemu_status — запуск/останов эмулятора;
//    wait                                 — пауза (в песочнице агента нет setTimeout);
//    key / keys / type_text               — sendkey с клавиатурной картой;
//    mouse_sync / mouse_to / mouse_click / mouse_drag / mouse_button;
//    shot                                 — screendump + PPM→PNG (чистый Node);
//    serial_read                          — хвост COM1-лога ядра;
//    image_info / image_crop / image_diff — разбор PNG (кроп, диффы);
//    build                                — build.ps1 -norun + размеры артефактов.
//
//  Мышь: сервер сам следит за позицией курсора (стартует из центра экрана —
//  ядро MyOS ставит курсор в центр при запуске оболочки). Абсолютные переме-
//  щения идут относительными событиями QEMU шагами <= 40 px, как в старых
//  тестах: так QEMU не теряет дельты и ОС успевает обработать.
// ===========================================================================

import { spawn, execFileSync } from 'node:child_process';
import { readFileSync, writeFileSync, existsSync, unlinkSync, statSync } from 'node:fs';
import { connect } from 'node:net';
import { deflateSync, inflateSync } from 'node:zlib';
import path from 'node:path';

const ROOT = 'D:\\C++\\MyOS';
const QEMU_EXE = 'C:\\Program Files\\qemu\\qemu-system-i386.exe';
const SCR_W = 1024, SCR_H = 768;
const DEF_PORT = 4599;

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const log = (...a) => process.stderr.write('[myos-bench] ' + a.join(' ') + '\n');

let qemuProc = null;
let mon = null;
let monOut = '';
let monPort = DEF_PORT;
let serialPath = path.join(ROOT, '_serial.log');
let mousePos = { x: SCR_W >> 1, y: SCR_H >> 1 }; // ядро ставит курсор в центр
let shotSeq = 0;

// ---------------------------------------------------------------------------
//  QEMU + монитор
// ---------------------------------------------------------------------------

function killAllQemu() {
    try {
        execFileSync('taskkill', ['/IM', 'qemu-system-i386.exe', '/F'], { stdio: 'ignore' });
        return true;
    } catch { return false; }               // нет процессов — taskkill вернёт ошибку
}

function monitorConnect(port, tries) {
    return new Promise((resolve, reject) => {
        const attempt = (n) => {
            const s = connect({ host: '127.0.0.1', port });
            let connected = false;
            s.on('connect', () => {
                connected = true;
                s.setNoDelay(true);
                s.on('data', (d) => { monOut += d.toString('latin1'); if (monOut.length > 8192) monOut = monOut.slice(-4096); });
                s.on('error', () => {});
                s.on('close', () => { if (mon === s) mon = null; });
                mon = s;
                resolve();
            });
            s.on('error', (e) => {
                if (connected) return;
                s.destroy();
                if (n > 0) setTimeout(() => attempt(n - 1), 250);
                else reject(e);
            });
        };
        attempt(tries);
    });
}

function monSend(cmd) {
    if (!mon) throw new Error('монитор не подключён (QEMU не запущен?)');
    mon.write(cmd + '\n');
}

async function ensureMon() {
    if (mon) return;
    await monitorConnect(monPort, 10);
}

async function qemuStart(a) {
    const disk = a.disk || path.join(ROOT, 'disk.img');
    const port = a.port || DEF_PORT;
    const serial = a.serial || path.join(ROOT, '_serial.log');
    if (!existsSync(disk)) throw new Error('нет образа диска: ' + disk);

    killAllQemu();
    if (qemuProc) { try { qemuProc.kill(); } catch {} qemuProc = null; }
    if (mon) { try { mon.destroy(); } catch {} mon = null; }
    await sleep(500);
    if (existsSync(serial)) { try { unlinkSync(serial); } catch {} }

    const args = [
        '-drive', `format=raw,file=${disk}`,
        '-monitor', `tcp:127.0.0.1:${port},server,nowait`,
        '-serial', `file:${serial}`,
    ];
    if (!a.show) args.push('-display', 'none');     // по умолчанию без окна

    const proc = spawn(QEMU_EXE, args, { stdio: ['ignore', 'ignore', 'pipe'], windowsHide: true });
    let qerr = '';
    proc.stderr.on('data', (d) => { qerr += d.toString(); });
    proc.on('error', (e) => { qerr += String(e); });
    proc.on('exit', () => { if (qemuProc === proc) qemuProc = null; });
    qemuProc = proc;
    monPort = port;
    serialPath = serial;
    const pid = proc.pid;

    try {
        await monitorConnect(port, 40);
    } catch (e) {
        throw new Error('QEMU запущен, но монитор недоступен: ' + (qerr || e.message));
    }
    mousePos = { x: SCR_W >> 1, y: SCR_H >> 1 };
    return `qemu запущен: pid=${pid} порт=${port} serial=${serial}${a.show ? '' : ' (без окна)'}`;
}

async function qemuStop() {
    if (mon) { try { mon.write('quit\n'); } catch {} }
    await sleep(600);
    if (qemuProc) { try { qemuProc.kill(); } catch {} qemuProc = null; }
    if (mon) { try { mon.destroy(); } catch {} mon = null; }
    killAllQemu();
    return 'qemu остановлен';
}

function qemuStatus() {
    return `qemu: ${qemuProc ? 'pid=' + qemuProc.pid : 'не запущен'}, монитор: ${mon ? 'есть (' + monPort + ')' : 'нет'}, курсор(учёт): ${mousePos.x},${mousePos.y}`;
}

function cleanupAll() {
    try { if (mon) mon.write('quit\n'); } catch {}
    try { if (qemuProc) qemuProc.kill(); } catch {}
    try { killAllQemu(); } catch {}
}

// ---------------------------------------------------------------------------
//  Ввод: клавиатура
// ---------------------------------------------------------------------------

const CHAR_MAP = {
    ' ': 'spc', '\n': 'ret', '\t': 'tab',
    '.': 'dot', ',': 'comma', '/': 'slash', ';': 'semicolon', "'": 'apostrophe',
    '-': 'minus', '=': 'equal', '[': 'bracket_left', ']': 'bracket_right',
    '\\': 'backslash', '`': 'grave_accent',
    ':': 'shift-semicolon', '_': 'shift-minus', '+': 'shift-equal',
    '!': 'shift-1', '@': 'shift-2', '#': 'shift-3', '$': 'shift-4', '%': 'shift-5',
    '^': 'shift-6', '&': 'shift-7', '*': 'shift-8', '(': 'shift-9', ')': 'shift-0',
    '{': 'shift-bracket_left', '}': 'shift-bracket_right', '|': 'shift-backslash',
    '"': 'shift-apostrophe', '<': 'shift-comma', '>': 'shift-dot',
    '?': 'shift-slash', '~': 'shift-grave_accent',
};

async function sendKey(k, hold = 100, gap = 80) {
    monSend(`sendkey ${k} ${Math.max(1, Math.round(hold))}`);
    await sleep(gap);
}

async function typeText(text, gap = 150) {
    for (const ch of text) {
        let k;
        if (/[a-z0-9]/.test(ch)) k = ch;
        else if (ch >= 'A' && ch <= 'Z') k = 'shift-' + ch.toLowerCase();
        else if (CHAR_MAP[ch]) k = CHAR_MAP[ch];
        else throw new Error('type_text: нет клавиши для ' + JSON.stringify(ch));
        await sendKey(k, 80, gap);
    }
}

// ---------------------------------------------------------------------------
//  Ввод: мышь (сервер следит за позицией курсора)
// ---------------------------------------------------------------------------

const clampX = (x) => Math.max(0, Math.min(SCR_W - 1, Math.round(x)));
const clampY = (y) => Math.max(0, Math.min(SCR_H - 1, Math.round(y)));

async function mouseTo(x, y, stepMs = 60) {
    x = clampX(x); y = clampY(y);
    let dx = x - mousePos.x, dy = y - mousePos.y;
    const n = Math.max(1, Math.ceil(Math.max(Math.abs(dx), Math.abs(dy)) / 40));
    for (let i = 0; i < n; i++) {
        const left = n - i;
        const sx = (i === n - 1) ? dx : Math.round(dx / left);
        const sy = (i === n - 1) ? dy : Math.round(dy / left);
        dx -= sx; dy -= sy;
        if (sx === 0 && sy === 0) continue;
        monSend(`mouse_move ${sx} ${sy}`);
        await sleep(stepMs);
    }
    mousePos = { x, y };
    await sleep(150);                          // дать оболочке перерисоваться
}

const btnMask = (b) => (b === 'right' ? 2 : b === 'middle' ? 4 : 1);

async function mouseClick(button = 'left', hold = 120) {
    monSend(`mouse_button ${btnMask(button)}`);
    await sleep(hold);
    monSend('mouse_button 0');
    await sleep(180);
}

async function mouseDrag(a) {
    monSend(`mouse_button ${btnMask(a.button || 'left')}`);
    await sleep(a.hold || 150);
    await mouseTo(a.x, a.y, a.stepMs || 70);
    await sleep(150);
    monSend('mouse_button 0');
    await sleep(180);
}

// ---------------------------------------------------------------------------
//  Скриншоты: screendump -> PPM -> PNG (всё внутри Node, без PowerShell)
// ---------------------------------------------------------------------------

function parsePPM(buf) {
    let pos = 0;
    const isWs = (c) => c === 32 || c === 9 || c === 10 || c === 13;
    const token = () => {
        for (;;) {
            if (pos >= buf.length) throw new Error('ppm: неожиданный конец файла');
            const c = buf[pos];
            if (c === 35) { while (pos < buf.length && buf[pos] !== 10) pos++; } // комментарий
            else if (isWs(c)) pos++;
            else break;
        }
        const s = pos;
        while (pos < buf.length && !isWs(buf[pos]) && buf[pos] !== 35) pos++;
        return buf.toString('latin1', s, pos);
    };
    const magic = token();
    if (magic !== 'P6') throw new Error('ppm: не P6 (' + magic + ')');
    const w = parseInt(token(), 10), h = parseInt(token(), 10), maxv = parseInt(token(), 10);
    if (!w || !h || maxv !== 255) throw new Error('ppm: плохой заголовок');
    pos++;                                     // одиночный пробел после maxval
    const need = w * h * 3;
    if (buf.length - pos < need) throw new Error('ppm: данных меньше ожидаемого');
    return { w, h, rgb: buf.subarray(pos, pos + need) };
}

const CRC_T = (() => {
    const t = new Int32Array(256);
    for (let n = 0; n < 256; n++) {
        let c = n;
        for (let k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320 ^ (c >>> 1)) : (c >>> 1);
        t[n] = c;
    }
    return t;
})();
function crc32(b) {
    let c = -1;
    for (let i = 0; i < b.length; i++) c = CRC_T[(c ^ b[i]) & 255] ^ (c >>> 8);
    return (c ^ -1) >>> 0;
}
function pngChunk(type, data) {
    const out = Buffer.alloc(12 + data.length);
    out.writeUInt32BE(data.length, 0);
    out.write(type, 4, 'latin1');
    data.copy(out, 8);
    out.writeUInt32BE(crc32(out.subarray(4, 8 + data.length)), 8 + data.length);
    return out;
}
function encodePngRGB(w, h, rgb) {
    const stride = w * 3;
    const raw = Buffer.alloc(h * (stride + 1));
    for (let y = 0; y < h; y++) {
        raw[y * (stride + 1)] = 0;             // filter = none
        rgb.copy(raw, y * (stride + 1) + 1, y * stride, y * stride + stride);
    }
    const ihdr = Buffer.alloc(13);
    ihdr.writeUInt32BE(w, 0); ihdr.writeUInt32BE(h, 4);
    ihdr[8] = 8; ihdr[9] = 2;                  // 8 бит, truecolor RGB
    return Buffer.concat([
        Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]),
        pngChunk('IHDR', ihdr),
        pngChunk('IDAT', deflateSync(raw)),
        pngChunk('IEND', Buffer.alloc(0)),
    ]);
}

function decodePng(buf) {
    if (buf.length < 8 || buf.readUInt32BE(0) !== 0x89504E47) throw new Error('png: плохая подпись');
    let pos = 8, w = 0, h = 0, bd = 0, ct = 0, inter = 0;
    const idat = [];
    while (pos + 8 <= buf.length) {
        const len = buf.readUInt32BE(pos);
        const type = buf.toString('latin1', pos + 4, pos + 8);
        const data = buf.subarray(pos + 8, pos + 8 + len);
        if (type === 'IHDR') { w = data.readUInt32BE(0); h = data.readUInt32BE(4); bd = data[8]; ct = data[9]; inter = data[12]; }
        else if (type === 'IDAT') idat.push(data);
        else if (type === 'IEND') break;
        pos += 12 + len;
    }
    if (bd !== 8 || inter !== 0 || (ct !== 2 && ct !== 6))
        throw new Error(`png: неподдерживаемый формат (bd=${bd} ct=${ct} inter=${inter})`);
    const ch = ct === 2 ? 3 : 4;
    const raw = inflateSync(Buffer.concat(idat));
    const stride = w * ch;
    if (raw.length < h * (stride + 1)) throw new Error('png: мало данных IDAT');
    const out = Buffer.alloc(h * stride);
    let p = 0;
    for (let y = 0; y < h; y++) {
        const f = raw[p++];
        const r0 = y * stride, b0 = (y - 1) * stride;
        for (let x = 0; x < stride; x++) {
            const v = raw[p++];
            const a = x >= ch ? out[r0 + x - ch] : 0;
            const b = y > 0 ? out[b0 + x] : 0;
            const c = (x >= ch && y > 0) ? out[b0 + x - ch] : 0;
            let r;
            switch (f) {
                case 0: r = v; break;
                case 1: r = v + a; break;
                case 2: r = v + b; break;
                case 3: r = v + ((a + b) >> 1); break;
                case 4: {
                    const pa = Math.abs(b - c), pb = Math.abs(a - c), pc = Math.abs(a + b - 2 * c);
                    r = v + ((pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c));
                    break;
                }
                default: throw new Error('png: неизвестный фильтр ' + f);
            }
            out[r0 + x] = r & 255;
        }
    }
    return { w, h, ch, data: out };
}

async function shot(a) {
    if (!mon) {
        if (!qemuProc) throw new Error('QEMU не запущен');
        await ensureMon();
    }
    let name = a.name || ('shot_' + (++shotSeq));
    if (!/^[A-Za-z0-9_.-]+$/.test(name)) throw new Error('shot: недопустимое имя ' + name);
    const base = name.replace(/\.png$/i, '');
    const ppm = path.join(ROOT, base + '.ppm');
    const png = path.join(ROOT, base + '.png');
    if (existsSync(ppm)) unlinkSync(ppm);
    monOut = '';
    monSend(`screendump ${ppm}`);
    let last = -1, ok = false;
    for (let i = 0; i < 60; i++) {
        await sleep(100);
        if (existsSync(ppm)) {
            const sz = statSync(ppm).size;
            if (sz > 0 && sz === last) { ok = true; break; }   // размер стабилен
            last = sz;
        }
    }
    if (!ok) {
        const hint = monOut.trim() ? ' | монитор: ' + monOut.trim().split('\n').slice(-3).join(' / ') : '';
        throw new Error('screendump не создал файл ' + ppm + hint);
    }
    const { w, h, rgb } = parsePPM(readFileSync(ppm));
    writeFileSync(png, encodePngRGB(w, h, rgb));
    try { unlinkSync(ppm); } catch {}
    return png;
}

// ---------------------------------------------------------------------------
//  serial-лог, картинки, сборка
// ---------------------------------------------------------------------------

function serialRead(lines = 80) {
    if (!existsSync(serialPath)) return '(serial-лога пока нет: ' + serialPath + ')';
    let buf;
    try { buf = readFileSync(serialPath); }
    catch (e) { return '(serial не читается прямо сейчас: ' + e.message + ')'; }
    const arr = buf.toString('utf8').split(/\r?\n/);
    while (arr.length && arr[arr.length - 1] === '') arr.pop();
    return arr.slice(-Math.max(1, lines)).join('\n') || '(serial-лог пуст)';
}

function imageCrop(a) {
    const img = decodePng(readFileSync(a.src));
    const x0 = Math.min(Math.max(0, a.x | 0), img.w - 1);
    const y0 = Math.min(Math.max(0, a.y | 0), img.h - 1);
    const cw = Math.min(a.w | 0, img.w - x0);
    const chh = Math.min(a.h | 0, img.h - y0);
    if (cw <= 0 || chh <= 0) throw new Error('image_crop: пустой прямоугольник');
    const scale = a.scale && a.scale > 0 ? Math.round(a.scale) : 1;
    const ow = cw * scale, oh = chh * scale;
    const out = Buffer.alloc(ow * oh * 3);
    for (let oy = 0; oy < oh; oy++) {
        const sy = y0 + Math.floor(oy / scale);
        for (let ox = 0; ox < ow; ox++) {
            const sx = x0 + Math.floor(ox / scale);
            const si = (sy * img.w + sx) * img.ch;
            const di = (oy * ow + ox) * 3;
            out[di] = img.data[si]; out[di + 1] = img.data[si + 1]; out[di + 2] = img.data[si + 2];
        }
    }
    writeFileSync(a.dst, encodePngRGB(ow, oh, out));
    return `${a.dst} (${ow}x${oh})`;
}

function imageDiff(a) {
    const A = decodePng(readFileSync(a.a));
    const B = decodePng(readFileSync(a.b));
    if (A.w !== B.w || A.h !== B.h) throw new Error(`image_diff: размеры разные (${A.w}x${A.h} vs ${B.w}x${B.h})`);
    if (A.ch !== B.ch) throw new Error('image_diff: разные каналы (сравнивайте свежие снимки)');
    const th = a.threshold == null ? 0 : a.threshold | 0;
    const n = A.w * A.h;
    let cnt = 0, minx = 1e9, miny = 1e9, maxx = -1, maxy = -1;
    const mark = a.dst ? Buffer.from(A.data) : null;
    for (let px = 0; px < n; px++) {
        const i = px * A.ch;
        const d = Math.max(
            Math.abs(A.data[i] - B.data[i]),
            Math.abs(A.data[i + 1] - B.data[i + 1]),
            Math.abs(A.data[i + 2] - B.data[i + 2]));
        if (d > th) {
            cnt++;
            const x = px % A.w, y = (px / A.w) | 0;
            if (x < minx) minx = x; if (x > maxx) maxx = x;
            if (y < miny) miny = y; if (y > maxy) maxy = y;
            if (mark) { mark[i] = 255; mark[i + 1] = 0; mark[i + 2] = 0; }
        }
    }
    if (a.dst && mark) {
        let rgb = mark;
        if (A.ch === 4) {
            rgb = Buffer.alloc(n * 3);
            for (let i = 0, j = 0; i < n * 4; i += 4, j += 3) {
                rgb[j] = mark[i]; rgb[j + 1] = mark[i + 1]; rgb[j + 2] = mark[i + 2];
            }
        }
        writeFileSync(a.dst, encodePngRGB(A.w, A.h, rgb));
    }
    const bbox = cnt ? `${minx},${miny}..${maxx},${maxy}` : 'нет';
    return `отличий: ${cnt}  bbox: ${bbox}${a.dst ? '  -> ' + a.dst : ''}`;
}

function runBuild(a) {
    return new Promise((resolve) => {
        const extra = (a && a.args) || ['-norun'];
        const args = ['-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', path.join(ROOT, 'build.ps1'), ...extra];
        const p = spawn('powershell', args, { cwd: ROOT, windowsHide: true });
        let out = '';
        p.stdout.on('data', (d) => { out += d; });
        p.stderr.on('data', (d) => { out += d; });
        p.on('close', (code) => {
            const lines = out.split(/\r?\n/).filter((l) => l.trim() !== '');
            const tail = lines.slice(-40).join('\n');
            const arts = ['kernel.bin', 'shell.bin', 'disk.img'].map((f) => {
                try { return `${f}: ${statSync(path.join(ROOT, f)).size} B`; }
                catch { return `${f}: НЕТ`; }
            }).join(', ');
            resolve(`build exit=${code}; ${arts}\n--- хвост вывода ---\n${tail}`);
        });
    });
}

// ---------------------------------------------------------------------------
//  MCP: инструменты
// ---------------------------------------------------------------------------

const TOOLS = [
    { name: 'qemu_start', description: 'Запустить QEMU с образов MyOS (по умолчанию disk.img, без окна, монитор на 127.0.0.1:4599, serial в _serial.log). Сначала убивает все прежние qemu-system-i386.', inputSchema: { type: 'object', properties: { disk: { type: 'string' }, port: { type: 'integer' }, show: { type: 'boolean', description: 'показать окно QEMU (по умолчанию headless)' }, serial: { type: 'string' } }, additionalProperties: false } },
    { name: 'qemu_stop', description: 'Остановить QEMU (quit + taskkill).', inputSchema: { type: 'object', properties: {}, additionalProperties: false } },
    { name: 'qemu_status', description: 'Статус QEMU/монитора и учётная позиция курсора.', inputSchema: { type: 'object', properties: {}, additionalProperties: false } },
    { name: 'wait', description: 'Пауза в миллисекундах (максимум 600000).', inputSchema: { type: 'object', properties: { ms: { type: 'integer' } }, required: ['ms'], additionalProperties: false } },
    { name: 'key', description: 'Нажать клавишу (имя QEMU: ret, esc, f1..f12, up, down, left, right, spc, tab, backspace, delete, shift-a, ctrl-c, ctrl-alt-del...).', inputSchema: { type: 'object', properties: { key: { type: 'string' }, hold: { type: 'integer' }, gap: { type: 'integer' } }, required: ['key'], additionalProperties: false } },
    { name: 'keys', description: 'Последовательность клавиш (массив имён QEMU).', inputSchema: { type: 'object', properties: { keys: { type: 'array', items: { type: 'string' } }, hold: { type: 'integer' }, gap: { type: 'integer' } }, required: ['keys'], additionalProperties: false } },
    { name: 'type_text', description: 'Напечатать строку (латиница/цифры/пунктуация раскладки US; \\n = Enter).', inputSchema: { type: 'object', properties: { text: { type: 'string' }, gap: { type: 'integer' } }, required: ['text'], additionalProperties: false } },
    { name: 'mouse_sync', description: 'Задать учётную позицию курсора (после перезапуска ОС: 512,384).', inputSchema: { type: 'object', properties: { x: { type: 'integer' }, y: { type: 'integer' } }, required: ['x', 'y'], additionalProperties: false } },
    { name: 'mouse_to', description: 'Переместить курсор в абсолютную точку (шагами по 40 px).', inputSchema: { type: 'object', properties: { x: { type: 'integer' }, y: { type: 'integer' }, stepMs: { type: 'integer' } }, required: ['x', 'y'], additionalProperties: false } },
    { name: 'mouse_click', description: 'Клик кнопкой (left/right/middle) в текущей позиции.', inputSchema: { type: 'object', properties: { button: { type: 'string', enum: ['left', 'right', 'middle'] }, hold: { type: 'integer' } }, additionalProperties: false } },
    { name: 'mouse_button', description: 'Нажать/отпустить кнопку мыши без клика.', inputSchema: { type: 'object', properties: { button: { type: 'string', enum: ['left', 'right', 'middle'] }, down: { type: 'boolean' } }, required: ['button', 'down'], additionalProperties: false } },
    { name: 'mouse_drag', description: 'Тащить: зажать кнопку, перевести курсор в (x,y), отпустить.', inputSchema: { type: 'object', properties: { x: { type: 'integer' }, y: { type: 'integer' }, button: { type: 'string', enum: ['left', 'right', 'middle'] }, hold: { type: 'integer' }, stepMs: { type: 'integer' } }, required: ['x', 'y'], additionalProperties: false } },
    { name: 'shot', description: 'Снимок экрана QEMU -> PNG в D:\\C++\\MyOS\\<name>.png; вернуть путь для чтения.', inputSchema: { type: 'object', properties: { name: { type: 'string' } }, additionalProperties: false } },
    { name: 'serial_read', description: 'Хвост serial-лога ядра (COM1).', inputSchema: { type: 'object', properties: { lines: { type: 'integer' } }, additionalProperties: false } },
    { name: 'image_info', description: 'Размеры PNG.', inputSchema: { type: 'object', properties: { src: { type: 'string' } }, required: ['src'], additionalProperties: false } },
    { name: 'image_crop', description: 'Вырезать область PNG с масштабом (нейрест) и сохранить.', inputSchema: { type: 'object', properties: { src: { type: 'string' }, dst: { type: 'string' }, x: { type: 'integer' }, y: { type: 'integer' }, w: { type: 'integer' }, h: { type: 'integer' }, scale: { type: 'integer' } }, required: ['src', 'dst', 'x', 'y', 'w', 'h'], additionalProperties: false } },
    { name: 'image_diff', description: 'Сравнить два PNG: число отличий, bbox; опционально картинка с красной разметкой.', inputSchema: { type: 'object', properties: { a: { type: 'string' }, b: { type: 'string' }, dst: { type: 'string' }, threshold: { type: 'integer' } }, required: ['a', 'b'], additionalProperties: false } },
    { name: 'build', description: 'Собрать MyOS (build.ps1, по умолчанию -norun) и вернуть хвост вывода + размеры артефактов.', inputSchema: { type: 'object', properties: { args: { type: 'array', items: { type: 'string' } } }, additionalProperties: false } },
];

const INSTRUCTIONS = [
    'Стенд MyOS (D:\\C++\\MyOS): QEMU + снимки экрана + сборка.',
    'Цикл: qemu_start -> wait ~4000 -> shot -> ввод -> qemu_stop.',
    'Снимки (shot) сохраняются как PNG в D:\\C++\\MyOS и возвращают путь — читай их инструментом read.',
    'Позиция курсора учитывается сервером от центра (512,384); после перезапуска ОС вызывай mouse_sync.',
    'Клавиши: имена QEMU (ret, esc, f3, right, shift-a); строки — через type_text.',
    'serial_read — хвост COM1-лога ядра. image_crop/image_diff — разбор снимков. build — сборка.',
    'Одновременно работает один QEMU; qemu_start сам убивает прежние экземпляры.',
].join(' ');

async function callTool(name, a) {
    switch (name) {
        case 'qemu_start': return await qemuStart(a);
        case 'qemu_stop': return await qemuStop();
        case 'qemu_status': return qemuStatus();
        case 'wait': {
            const ms = Math.max(0, Math.min(600000, a.ms | 0));
            await sleep(ms);
            return `пауза ${ms} мс`;
        }
        case 'key': await sendKey(a.key, a.hold ?? 100, a.gap ?? 80); return `клавиша ${a.key}`;
        case 'keys': {
            for (const k of a.keys || []) await sendKey(k, a.hold ?? 100, a.gap ?? 150);
            return 'клавиши: ' + (a.keys || []).join(' ');
        }
        case 'type_text': await typeText(a.text || '', a.gap ?? 150); return 'набрано: ' + JSON.stringify(a.text || '');
        case 'mouse_sync': mousePos = { x: clampX(a.x), y: clampY(a.y) }; return `курсор(учёт) = ${mousePos.x},${mousePos.y}`;
        case 'mouse_to': await mouseTo(a.x, a.y, a.stepMs ?? 60); return `курсор(учёт) = ${mousePos.x},${mousePos.y}`;
        case 'mouse_click': await mouseClick(a.button || 'left', a.hold ?? 120); return `клик ${a.button || 'left'} в ${mousePos.x},${mousePos.y}`;
        case 'mouse_button': monSend(`mouse_button ${a.down ? btnMask(a.button || 'left') : 0}`); await sleep(80); return `${a.down ? 'нажата' : 'отпущена'} ${a.button || 'left'}`;
        case 'mouse_drag': await mouseDrag(a); return `тащили в ${mousePos.x},${mousePos.y}`;
        case 'shot': return await shot(a);
        case 'serial_read': return serialRead(a.lines ?? 80);
        case 'image_info': {
            const im = decodePng(readFileSync(a.src));
            return `${a.src}: ${im.w}x${im.h}, каналов=${im.ch}`;
        }
        case 'image_crop': return imageCrop(a);
        case 'image_diff': return imageDiff(a);
        case 'build': return await runBuild(a);
        default: throw new Error('неизвестный инструмент: ' + name);
    }
}

// ---------------------------------------------------------------------------
//  MCP: транспорт (stdio, newline-delimited JSON-RPC 2.0)
// ---------------------------------------------------------------------------

function reply(id, result) {
    process.stdout.write(JSON.stringify({ jsonrpc: '2.0', id, result }) + '\n');
}
function replyErr(id, code, message) {
    process.stdout.write(JSON.stringify({ jsonrpc: '2.0', id, error: { code, message } }) + '\n');
}

let callQueue = Promise.resolve();
function enqueue(fn) {
    const p = callQueue.then(fn, fn);
    callQueue = p.catch(() => {});
    return p;
}

async function handleLine(line) {
    let msg;
    try { msg = JSON.parse(line); } catch { return; }
    const { id, method, params } = msg;
    if (method === 'initialize') {
        reply(id, {
            protocolVersion: (params && params.protocolVersion) || '2025-06-18',
            capabilities: { tools: {} },
            serverInfo: { name: 'myos-bench', version: '1.0.0' },
            instructions: INSTRUCTIONS,
        });
        return;
    }
    if (method === 'notifications/initialized' || (method && method.startsWith('notifications/'))) return;
    if (method === 'ping') { reply(id, {}); return; }
    if (method === 'tools/list') { reply(id, { tools: TOOLS }); return; }
    if (method === 'tools/call') {
        const name = params && params.name;
        const args = (params && params.arguments) || {};
        enqueue(async () => {
            try {
                const text = await callTool(name, args);
                reply(id, { content: [{ type: 'text', text: String(text) }] });
            } catch (e) {
                reply(id, { content: [{ type: 'text', text: 'ОШИБКА: ' + (e && e.message ? e.message : String(e)) }], isError: true });
            }
        });
        return;
    }
    if (id !== undefined) replyErr(id, -32601, 'метод не найден: ' + method);
}

let sbuf = '';
process.stdin.setEncoding('utf8');
process.stdin.on('data', (d) => {
    sbuf += d;
    let i;
    while ((i = sbuf.indexOf('\n')) >= 0) {
        const line = sbuf.slice(0, i).replace(/\r$/, '');
        sbuf = sbuf.slice(i + 1);
        if (line.trim()) handleLine(line).catch((e) => log('ошибка обработки: ' + e));
    }
});
process.stdin.on('end', () => { cleanupAll(); process.exit(0); });
process.on('SIGINT', () => { cleanupAll(); process.exit(0); });
process.on('SIGTERM', () => { cleanupAll(); process.exit(0); });
process.on('uncaughtException', (e) => log('uncaught: ' + (e && e.stack ? e.stack : e)));
process.on('unhandledRejection', (e) => log('unhandled: ' + (e && e.stack ? e.stack : e)));

log('myos-bench MCP запущен (root=' + ROOT + ')');
