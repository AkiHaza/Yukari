const assert = require('node:assert/strict');
const { readFileSync } = require('node:fs');
const { join } = require('node:path');
const { test } = require('node:test');
const vm = require('node:vm');

const source = readFileSync(process.env.YUKARI_WEBUI_SOURCE ||
    join(__dirname, '../../module/webroot/assets/app.js'), 'utf8').replace(/\nboot\(\);\s*$/, '');
const dir = '/data/adb/modules/Yukari';
const info = 'config=ready\nenabled=false\nhide_features=true\nhide_resources=false\nversion=2.0.0\ncode=2\n';

function createApp() {
    const elements = new Map();
    const checks = [];
    function element() {
        return {
            listeners: {}, dataset: {}, textContent: '', hidden: false, disabled: false,
            classList: { add() {}, remove() {}, toggle() {} },
            setAttribute() {}, append() {}, replaceChildren() {}, animate() {},
            querySelector() { return element(); },
            addEventListener(name, handler) { this.listeners[name] = handler; },
        };
    }
    const timers = new Map();
    let timerId = 0;
    const context = vm.createContext({
        window: { location: { hash: '' }, addEventListener() {}, scrollTo() {} },
        document: {
            documentElement: element(),
            getElementById(id) {
                if (!elements.has(id)) elements.set(id, element());
                return elements.get(id);
            },
            createElement(tag) {
                const node = element();
                if (tag === 'input') checks.push(node);
                return node;
            },
            createDocumentFragment: element,
            querySelectorAll(selector) { return selector === '.app-check' ? checks : []; },
        },
        localStorage: { getItem() { return null; }, setItem() {} },
        setTimeout(fn, ms) { timers.set(++timerId, { fn, ms }); return timerId; },
        clearTimeout(id) { timers.delete(id); },
        requestAnimationFrame(fn) { fn(); },
    });
    vm.runInContext(source, context);
    const run = (code) => vm.runInContext(code, context);
    const calls = [];
    function bridge(reply = (command) => {
        if (command.startsWith('for d in')) return [0, dir, ''];
        if (command.endsWith(' info')) return [0, info, ''];
        if (command.endsWith(' targets')) return [0, 'com.example.app\ncom.removed.app:worker\n', ''];
        if (command.endsWith(' apps')) return [0, 'com.example.app\ncom.other.app\n', ''];
        return [0, '', ''];
    }) {
        context.window.ksu = {
            // Android JavascriptInterface exposes exec(String, String, String).
            exec(command, options, callbackName) {
                assert.equal(typeof options, 'string');
                assert.deepEqual(JSON.parse(options), {});
                assert.equal(typeof callbackName, 'string');
                assert.equal(typeof context.window[callbackName], 'function');
                calls.push({ command, callbackName });
                const result = reply(command);
                if (result) queueMicrotask(() => context.window[callbackName](...result));
            },
        };
    }
    return { context, run, bridge, calls, timers, elements };
}

test('native string callback bridge reads actual flags, targets and apps', async () => {
    const app = createApp();
    app.bridge();
    await app.run('loadAll()');
    assert.equal(app.run('state.ready'), true);
    assert.equal(app.run('state.flags.enabled'), false);
    assert.equal(app.run('state.flags.hide_resources'), false);
    assert.deepEqual(Array.from(app.run('state.targets')), ['com.example.app', 'com.removed.app:worker']);
    assert.deepEqual(Array.from(app.run('state.apps')), ['com.example.app', 'com.other.app']);
    assert.equal(app.elements.get('config-path').textContent, `${dir}/config.json`);
    assert.equal(app.elements.get('toggle-enabled').disabled, false);
    assert.equal(Object.keys(app.context.window).some((key) => key.startsWith('yukari_exec_')), false);
    assert.equal([...app.timers.values()].some((timer) => timer.ms === 20000), false);
});

test('nonzero exit rejects even when the command produced stdout', async () => {
    const app = createApp();
    app.bridge(() => [1, 'partial output', 'permission denied']);
    await assert.rejects(app.run('sh("command")'), /permission denied/);
});

test('bridge exceptions and timeouts remove their global callbacks', async () => {
    const app = createApp();
    app.context.window.ksu = { exec() { throw new Error('bridge failure'); } };
    await assert.rejects(app.run('sh("command")'), /bridge failure/);
    assert.equal(Object.keys(app.context.window).some((key) => key.startsWith('yukari_exec_')), false);
    app.bridge(() => null);
    const rejected = assert.rejects(app.run('sh("command")'), /命令超时/);
    [...app.timers.values()].find((timer) => timer.ms === 20000).fn();
    await rejected;
    assert.equal(app.context.window[app.calls[0].callbackName], undefined);
    assert.equal(app.timers.size, 0);
});

test('concurrent commands have independent named callbacks', async () => {
    const app = createApp();
    app.bridge(() => null);
    const first = app.run('sh("first")');
    const second = app.run('sh("second")');
    const [a, b] = app.calls.map((call) => call.callbackName);
    assert.notEqual(a, b);
    app.context.window[b](0, 'second result', '');
    app.context.window[a](0, 'first result', '');
    assert.deepEqual(await Promise.all([first, second]), ['first result', 'second result']);
});

test('missing bridge shows failure without fake targets and refresh can recover', async () => {
    const app = createApp();
    app.run('bindEvents()');
    assert.equal(await app.run('loadAll()'), false);
    assert.equal(app.run('state.targets.size'), 0);
    assert.equal(app.elements.get('toggle-enabled').disabled, true);
    assert.equal(app.elements.get('add-manual').disabled, true);
    assert.equal(app.elements.get('refresh').disabled, false);
    assert.match(app.elements.get('snackbar-text').textContent, /root 桥接/);
    app.bridge();
    await app.elements.get('refresh').listeners.click();
    assert.equal(app.run('state.ready'), true);
    assert.equal(app.elements.get('add-manual').disabled, false);
});

test('failed reads do not publish partial data or report refresh success', async () => {
    const app = createApp();
    app.run('bindEvents()');
    app.bridge((command) => {
        if (command.startsWith('for d in')) return [0, dir, ''];
        if (command.endsWith(' info')) return [0, info, ''];
        if (command.endsWith(' targets')) return [0, 'com.example.app', ''];
        return [1, 'partial output', 'cannot list installed apps'];
    });
    await app.elements.get('refresh').listeners.click();
    assert.equal(app.run('state.ready'), false);
    assert.equal(app.run('state.targets.size'), 0);
    assert.match(app.elements.get('snackbar-text').textContent, /读取配置失败.*cannot list/);
    assert.equal(app.elements.get('refresh').disabled, false);
});

test('missing config is a failure and cannot be saved as defaults', async () => {
    const app = createApp();
    app.bridge((command) => [0, command.startsWith('for d in') ? dir : 'config=missing', '']);
    assert.equal(await app.run('loadAll()'), false);
    await app.run('persist()');
    assert.equal(app.calls.some((call) => call.command.includes(' save ')), false);
    assert.match(app.elements.get('snackbar-text').textContent, /配置文件不存在或不可读/);
});

test('saves serialize snapshots and report backend failures', async () => {
    const app = createApp();
    app.bridge();
    await app.run('loadAll()');
    const saves = [];
    app.bridge((command) => { saves.push(command); return null; });
    const first = app.run('state.targets.add("com.first.app"); persist()');
    const second = app.run('state.targets.add("com.second.app"); persist()');
    await new Promise(setImmediate);
    assert.equal(saves.length, 1);
    assert.equal(saves[0].includes('com.second.app'), false);
    app.context.window[app.calls.at(-1).callbackName](0, 'saved', '');
    await first;
    await new Promise(setImmediate);
    assert.equal(saves.length, 2);
    assert.equal(saves[1].includes('com.second.app'), true);
    app.context.window[app.calls.at(-1).callbackName](1, 'partial output', 'cannot replace config');
    await second;
    assert.equal(app.elements.get('save-state').textContent, '保存失败');
    assert.match(app.elements.get('snackbar-text').textContent, /cannot replace config/);
    app.bridge(() => [0, 'saved', '']);
    await app.run('persist()');
    assert.equal(app.elements.get('save-state').textContent, '已保存');
});
