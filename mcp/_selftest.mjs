// ===========================================================================
//  _selftest.mjs — самопроверка MCP-сервера myos-bench.mjs
// ===========================================================================
//  Гоняет полный цикл: инициализация -> qemu_start -> снимки -> клик мышью
//  по доку (Terminal) -> serial -> qemu_stop. Результат: mcp_boot.png (стол
//  после загрузки) и mcp_click.png (открылся ли терминал).
// ===========================================================================

import { spawn } from 'node:child_process';

const srv = spawn('node', ['D:\\C++\\MyOS\\mcp\\myos-bench.mjs'], { stdio: ['pipe', 'pipe', 'inherit'] });

let buf = '';
const pending = new Map();
let nextId = 1;
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

srv.stdout.on('data', (d) => {
    buf += d;
    let i;
    while ((i = buf.indexOf('\n')) >= 0) {
        const line = buf.slice(0, i);
        buf = buf.slice(i + 1);
        if (!line.trim()) continue;
        let m;
        try { m = JSON.parse(line); } catch { console.log('non-json:', line); continue; }
        if (m.id && pending.has(m.id)) {
            const p = pending.get(m.id);
            pending.delete(m.id);
            m.error ? p.reject(new Error(JSON.stringify(m.error))) : p.resolve(m.result);
        }
    }
});

function req(method, params) {
    return new Promise((resolve, reject) => {
        const id = nextId++;
        pending.set(id, { resolve, reject });
        srv.stdin.write(JSON.stringify({ jsonrpc: '2.0', id, method, params }) + '\n');
    });
}
function notify(method, params) {
    srv.stdin.write(JSON.stringify({ jsonrpc: '2.0', method, params }) + '\n');
}
async function call(name, args) {
    const r = await req('tools/call', { name, arguments: args || {} });
    const t = r.content.map((c) => c.text).join('\n');
    console.log(`[${name}] ${t}`);
    if (r.isError) throw new Error('tool failed: ' + name);
    return t;
}

try {
    const init = await req('initialize', { protocolVersion: '2025-06-18', capabilities: {}, clientInfo: { name: 'selftest', version: '1.0' } });
    console.log('protocol:', init.protocolVersion, '| server:', init.serverInfo.name);
    notify('notifications/initialized', {});

    const tools = await req('tools/list', {});
    console.log('tools:', tools.tools.map((t) => t.name).join(', '));

    await call('qemu_start', {});
    await call('wait', { ms: 4000 });
    await call('shot', { name: 'mcp_boot' });
    console.log('serial(4):', await call('serial_read', { lines: 4 }));

    // Клик по иконке Terminal в доке (координаты из test_mouse.ps1)
    await call('mouse_sync', { x: 512, y: 384 });
    await call('mouse_to', { x: 398, y: 740 });
    await call('mouse_click', {});
    await call('wait', { ms: 1500 });
    await call('shot', { name: 'mcp_click' });

    // Клавиатура + serial: печатаем help в открытом терминале,
    // вывод команды идёт через SYS_PRINT -> системный лог -> COM1.
    await call('type_text', { text: 'help\n' });
    await call('wait', { ms: 800 });
    await call('shot', { name: 'mcp_keys' });
    console.log('serial(8):', await call('serial_read', { lines: 8 }));
    await call('qemu_status', {});

    await call('qemu_stop', {});
    console.log('SELFTEST OK');
} catch (e) {
    console.error('SELFTEST FAIL:', e.message);
    try { await call('qemu_stop', {}); } catch {}
    process.exitCode = 1;
}
srv.stdin.end();
