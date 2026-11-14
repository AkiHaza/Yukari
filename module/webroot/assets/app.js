'use strict';

// Yukari WebUI. Commands use the root manager's native ksu.exec bridge.

const MODULE_ID = 'Yukari';
const MODULE_DIRS = [`/data/adb/modules/${MODULE_ID}`, `/data/adb/modules_update/${MODULE_ID}`];
const KEYWORDS = ['lineage', 'crdroid', 'aospa', 'pixelexperience', 'omnirom', 'protonaosp', 'profile'];
const NAME_RE = /^[A-Za-z0-9._]+(:[A-Za-z0-9._]+)?$/;
const THEME_KEY = 'yukari.theme';
const THEMES = ['auto', 'light', 'dark'];

const state = {
    dir: '',
    version: '',
    author: '',
    code: '',
    flags: { enabled: true, hide_features: true, hide_resources: true },
    targets: new Set(),
    apps: [],
    filter: 'all',
    query: '',
    ready: false,
    failed: false,
    loading: false,
    theme: readTheme(),
    saveTimer: 0,
    saveStateTimer: 0,
};

const el = (id) => document.getElementById(id);

/* ------------------------------------------------------------------ bridge */

let callbackCounter = 0;

function exec(command) {
    return new Promise((resolve, reject) => {
        const bridge = window.ksu;
        if (!bridge || typeof bridge.exec !== 'function') {
            reject(new Error('未检测到 root 桥接，请在模块 WebUI 中打开'));
            return;
        }
        const callbackName = `yukari_exec_${Date.now()}_${callbackCounter++}`;
        const cleanup = () => {
            clearTimeout(watchdog);
            delete window[callbackName];
        };
        const watchdog = setTimeout(() => {
            cleanup();
            reject(new Error('命令超时，请重试'));
        }, 20000);
        window[callbackName] = (errno, stdout, stderr) => {
            cleanup();
            resolve({ errno: Number(errno), stdout: stdout || '', stderr: stderr || '' });
        };
        try {
            // Android's JavascriptInterface accepts strings, not JS objects/functions.
            bridge.exec(command, '{}', callbackName);
        } catch (error) {
            cleanup();
            reject(error);
        }
    });
}

async function sh(command) {
    const result = await exec(command);
    if (result.errno !== 0) {
        throw new Error(result.stderr.trim() || `命令退出码 ${result.errno}`);
    }
    return result.stdout;
}

/* -------------------------------------------------------------------- data */

function parseKV(text) {
    const out = {};
    for (const line of text.split('\n')) {
        const index = line.indexOf('=');
        if (index <= 0) continue;
        out[line.slice(0, index).trim()] = line.slice(index + 1).trim();
    }
    return out;
}

function splitLines(text) {
    return text.split('\n').map((line) => line.trim()).filter(Boolean);
}

async function resolveDir() {
    const script = `for d in ${MODULE_DIRS.join(' ')}; do if [ -f "$d/webui.sh" ]; then echo "$d"; break; fi; done`;
    const found = (await sh(script)).trim().split('\n').filter(Boolean)[0];
    if (!MODULE_DIRS.includes(found)) throw new Error('未找到 Yukari 模块后端');
    return found;
}

async function loadAll() {
    if (state.loading) return false;
    state.loading = true;
    state.ready = false;
    state.failed = false;
    renderAll();
    try {
        const dir = await resolveDir();
        const info = parseKV(await sh(`sh ${dir}/webui.sh info`));
        if (info.config !== 'ready') throw new Error('配置文件不存在或不可读');
        const targets = splitLines(await sh(`sh ${dir}/webui.sh targets`));
        const apps = splitLines(await sh(`sh ${dir}/webui.sh apps`));
        state.dir = dir;
        state.version = info.version || '';
        state.code = info.code || '';
        state.author = info.author || state.author;
        state.flags.enabled = info.enabled !== 'false';
        state.flags.hide_features = info.hide_features !== 'false';
        state.flags.hide_resources = info.hide_resources !== 'false';
        state.targets = new Set(targets.filter((name) => NAME_RE.test(name)));
        state.apps = apps.filter((name) => NAME_RE.test(name));
        state.ready = true;
        state.failed = false;
        el('config-path').textContent = `${state.dir}/config.json`;
    } catch (error) {
        state.failed = true;
        snack(`读取配置失败：${error.message}`, null);
    } finally {
        state.loading = false;
        renderAll();
    }
    return state.ready;
}

function buildConfig() {
    return JSON.stringify({ ...state.flags, targets: [...state.targets].sort() }, null, 2);
}

function scheduleSave() {
    clearTimeout(state.saveTimer);
    state.saveTimer = setTimeout(persist, 240);
}

let saveQueue = Promise.resolve();

async function persist() {
    if (!state.ready) return;
    const command = `sh ${state.dir}/webui.sh save '${buildConfig()}'`;
    // Each save replaces the same config file; keep writes in UI order.
    const save = saveQueue.catch(() => {}).then(() => sh(command));
    saveQueue = save;
    setSaveState('保存中…');
    try {
        await save;
        if (save === saveQueue) setSaveState('已保存');
    } catch (error) {
        setSaveState('保存失败');
        snack(`保存失败：${error.message}`, null);
    }
}

/* ------------------------------------------------------------------- theme */

function readTheme() {
    try {
        const stored = localStorage.getItem(THEME_KEY);
        if (THEMES.includes(stored)) return stored;
    } catch (error) {
        /* storage may be unavailable in restricted WebViews */
    }
    return 'auto';
}

function applyTheme(theme) {
    state.theme = theme;
    document.documentElement.setAttribute('data-theme', theme);
    const icon = theme === 'light' ? '#i-sun' : '#i-moon';
    const use = el('theme-toggle').querySelector('use');
    use.setAttribute('href', icon);
    el('theme-toggle').setAttribute('aria-label', `主题：${theme === 'auto' ? '跟随系统' : theme === 'light' ? '浅色' : '深色'}`);
    try {
        localStorage.setItem(THEME_KEY, theme);
    } catch (error) {
        /* ignore */
    }
}

/* ------------------------------------------------------------------ render */

function rowModel() {
    const known = new Set(state.apps);
    const rows = state.apps.map((name) => ({ name, installed: true }));
    for (const name of state.targets) {
        if (!known.has(name)) rows.push({ name, installed: false });
    }
    rows.sort((a, b) => a.name.localeCompare(b.name));
    return rows;
}

function visibleRows() {
    const query = state.query.trim().toLowerCase();
    return rowModel().filter((row) => {
        if (query && !row.name.toLowerCase().includes(query)) return false;
        if (state.filter === 'selected') return state.targets.has(row.name);
        if (state.filter === 'stale') return !row.installed;
        return true;
    });
}

function avatarTone(name) {
    let hash = 0;
    for (let index = 0; index < name.length; index += 1) {
        hash = (hash * 31 + name.charCodeAt(index)) % 9973;
    }
    return (hash % 6) + 1;
}

function rowElement(row) {
    const selected = state.targets.has(row.name);
    const item = document.createElement('li');
    item.className = 'list__item';

    const label = document.createElement('label');
    label.className = `app-row${selected ? ' is-on' : ''}`;

    const check = document.createElement('input');
    check.type = 'checkbox';
    check.className = 'app-check';
    check.checked = selected;
    check.disabled = !state.ready;
    check.dataset.pkg = row.name;
    check.setAttribute('aria-label', `屏蔽 ${row.name}`);

    const avatar = document.createElement('span');
    avatar.className = 'avatar';
    avatar.dataset.tone = String(avatarTone(row.name));
    avatar.textContent = row.name.replace(/^(com|org|io|net|me)\./, '').charAt(0) || '?';

    const body = document.createElement('span');
    body.className = 'app-body';
    const name = document.createElement('span');
    name.className = 'app-name';
    name.textContent = row.name;
    const meta = document.createElement('span');
    meta.className = 'app-meta';
    if (!row.installed) {
        meta.textContent = '未安装 · 规则保留';
    } else if (row.name.includes(':')) {
        meta.textContent = selected ? '进程规则 · 已保护' : '进程规则';
    } else {
        meta.textContent = selected ? '已保护 · 第三方应用' : '第三方应用';
    }
    body.append(name, meta);

    label.append(check, avatar, body);

    if (!row.installed) {
        const badge = document.createElement('span');
        badge.className = 'badge';
        badge.textContent = '未安装';
        label.append(badge);
    }

    item.append(label);
    return item;
}

function renderList() {
    const list = el('app-list');
    const rows = visibleRows();
    const fragment = document.createDocumentFragment();
    for (const row of rows) fragment.append(rowElement(row));
    list.replaceChildren(fragment);

    const skeleton = el('app-skeleton');
    skeleton.hidden = state.ready || state.failed;

    const empty = el('app-empty');
    const busy = !state.ready && !state.failed;
    empty.hidden = rows.length > 0 || busy;
    if (!empty.hidden) {
        const title = empty.querySelector('.empty__title');
        const body = empty.querySelector('.empty__body');
        if (state.failed) {
            title.textContent = '无法读取模块配置';
            body.textContent = '确认模块已安装，并在 root 管理器内置的 WebUI 中打开此页面。';
        } else {
            title.textContent = '没有匹配的应用';
            body.textContent = '换个关键词，或用「手动添加」直接写入包名。';
        }
    }
}

function renderSummary() {
    const rows = rowModel();
    const stale = rows.filter((row) => !row.installed).length;
    const channels = state.ready && state.flags.enabled
        ? 1 + (state.flags.hide_features ? 1 : 0) + (state.flags.hide_resources ? 1 : 0)
        : 0;

    el('stat-targets').textContent = String(state.targets.size);
    el('stat-channels').textContent = `${channels}/3`;
    el('stat-apps').textContent = String(state.apps.length);

    el('count-all').textContent = String(rows.length);
    el('count-selected').textContent = String(state.targets.size);
    el('count-stale').textContent = String(stale);

    const active = [];
    if (state.flags.enabled) {
        active.push('服务列表');
        if (state.flags.hide_features) active.push('特性');
        if (state.flags.hide_resources) active.push('资源');
    }
    el('hero-state').textContent = !state.ready
        ? (state.failed ? '读取失败 · 点击右上角刷新重试' : '正在读取模块配置…')
        : state.flags.enabled
        ? `已启用 · ${active.join(' / ')}`
        : '已停用 · 所有服务查询原样放行';

    el('toggle-enabled').checked = state.ready && state.flags.enabled;
    el('toggle-features').checked = state.ready && state.flags.hide_features;
    el('toggle-resources').checked = state.ready && state.flags.hide_resources;
    el('toggle-enabled').disabled = !state.ready;
    el('toggle-features').disabled = !state.ready || !state.flags.enabled;
    el('toggle-resources').disabled = !state.ready || !state.flags.enabled;
    for (const id of ['select-all', 'clear-all', 'add-manual', 'manual-confirm', 'action-reset', 'action-force-stop']) {
        el(id).disabled = !state.ready;
    }
    for (const check of document.querySelectorAll('.app-check')) check.disabled = !state.ready;
    el('refresh').disabled = state.loading;

    el('version-badge').textContent = state.version ? `v${state.version}` : '—';
    el('about-author').textContent = `作者 ${state.author || '—'}`;
    el('about-version').textContent = state.version ? `版本 ${state.version} · 构建 ${state.code || '—'}` : 'Zygisk 模块';
}

function renderAll() {
    renderList();
    renderSummary();
}

function renderKeywords() {
    const host = el('keywords');
    const fragment = document.createDocumentFragment();
    for (const keyword of KEYWORDS) {
        const chip = document.createElement('span');
        chip.className = 'keyword';
        chip.textContent = keyword;
        fragment.append(chip);
    }
    host.replaceChildren(fragment);
}

/* ------------------------------------------------------------------- toast */

let snackTimer = 0;

function snack(message, action) {
    const bar = el('snackbar');
    const button = el('snackbar-action');
    el('snackbar-text').textContent = message;
    if (action) {
        button.hidden = false;
        button.textContent = action.label;
        button.onclick = () => {
            hideSnack();
            action.run();
        };
    } else {
        button.hidden = true;
        button.onclick = null;
    }
    bar.hidden = false;
    requestAnimationFrame(() => bar.classList.add('is-visible'));
    clearTimeout(snackTimer);
    snackTimer = setTimeout(hideSnack, action ? 6000 : 2600);
}

function hideSnack() {
    clearTimeout(snackTimer);
    const bar = el('snackbar');
    bar.classList.remove('is-visible');
    setTimeout(() => { bar.hidden = true; }, 240);
}

function setSaveState(text) {
    const host = el('save-state');
    host.textContent = text;
    host.classList.add('is-visible');
    clearTimeout(state.saveStateTimer);
    if (text === '已保存') {
        state.saveStateTimer = setTimeout(() => host.classList.remove('is-visible'), 1800);
    }
}

/* ------------------------------------------------------------------ events */

function setFilter(filter) {
    state.filter = filter;
    for (const chip of document.querySelectorAll('.chip')) {
        const selected = chip.dataset.filter === filter;
        chip.classList.toggle('is-selected', selected);
        chip.setAttribute('aria-selected', String(selected));
    }
    renderList();
}

function switchView(view, updateHash = true) {
    for (const section of document.querySelectorAll('.view')) {
        section.classList.toggle('is-active', section.id === `view-${view}`);
    }
    for (const item of document.querySelectorAll('.navbar__item')) {
        const selected = item.dataset.view === view;
        item.classList.toggle('is-selected', selected);
        item.setAttribute('aria-selected', String(selected));
    }
    if (updateHash) {
        try {
            history.replaceState(null, '', `#${view}`);
        } catch (error) {
            /* file:// or opaque origins may refuse */
        }
    }
    window.scrollTo({ top: 0, behavior: 'smooth' });
}

function viewFromHash() {
    const view = window.location.hash.replace('#', '');
    return ['apps', 'settings', 'about'].includes(view) ? view : 'apps';
}

function commitTargets(mutate) {
    const snapshot = new Set(state.targets);
    mutate();
    renderSummary();
    renderList();
    scheduleSave();
    return snapshot;
}

function openManualSheet() {
    const sheet = el('manual-sheet');
    const input = el('manual-input');
    input.value = '';
    input.classList.remove('is-invalid');
    el('manual-error').hidden = true;
    sheet.showModal();
    setTimeout(() => input.focus(), 40);
}

function closeManualSheet() {
    el('manual-sheet').close();
}

function submitManual() {
    const input = el('manual-input');
    const value = input.value.trim();
    if (!NAME_RE.test(value)) {
        input.classList.add('is-invalid');
        const error = el('manual-error');
        error.textContent = '只允许字母、数字、点、下划线与单个冒号进程分隔';
        error.hidden = false;
        return;
    }
    if (state.targets.has(value)) {
        closeManualSheet();
        snack('该目标已在列表中', null);
        return;
    }
    state.targets.add(value);
    closeManualSheet();
    state.query = '';
    el('search').value = '';
    el('search-clear').hidden = true;
    setFilter('all');
    renderSummary();
    scheduleSave();
    snack(`已添加 ${value}`, null);
}

function bindEvents() {
    el('theme-toggle').addEventListener('click', () => {
        const next = THEMES[(THEMES.indexOf(state.theme) + 1) % THEMES.length];
        applyTheme(next);
        snack(next === 'auto' ? '主题：跟随系统' : next === 'light' ? '主题：浅色' : '主题：深色', null);
    });

    el('refresh').addEventListener('click', async () => {
        const button = el('refresh');
        button.animate(
            [{ transform: 'rotate(0deg)' }, { transform: 'rotate(360deg)' }],
            { duration: 620, easing: 'cubic-bezier(.2,0,0,1)' }
        );
        if (await loadAll()) snack('已重新读取模块状态', null);
    });

    for (const item of document.querySelectorAll('.navbar__item')) {
        item.addEventListener('click', () => switchView(item.dataset.view));
    }

    el('search').addEventListener('input', (event) => {
        state.query = event.target.value;
        el('search-clear').hidden = state.query.length === 0;
        renderList();
    });

    el('search-clear').addEventListener('click', () => {
        state.query = '';
        el('search').value = '';
        el('search-clear').hidden = true;
        renderList();
        el('search').focus();
    });

    for (const chip of document.querySelectorAll('.chip')) {
        chip.addEventListener('click', () => setFilter(chip.dataset.filter));
    }

    el('app-list').addEventListener('change', (event) => {
        const check = event.target.closest('.app-check');
        if (!check) return;
        const name = check.dataset.pkg;
        if (check.checked) {
            state.targets.add(name);
        } else {
            state.targets.delete(name);
        }
        const row = check.closest('.app-row');
        if (row) row.classList.toggle('is-on', check.checked);
        renderSummary();
        scheduleSave();
        if (state.filter !== 'all') {
            setTimeout(renderList, 260);
        }
    });

    el('select-all').addEventListener('click', () => {
        const candidates = visibleRows().map((row) => row.name);
        if (!candidates.length) {
            snack('当前列表没有可选应用', null);
            return;
        }
        const snapshot = commitTargets(() => {
            for (const name of candidates) state.targets.add(name);
        });
        snack(`已选择 ${candidates.length} 个应用`, {
            label: '撤销',
            run: () => {
                state.targets = snapshot;
                renderAll();
                scheduleSave();
            },
        });
    });

    el('clear-all').addEventListener('click', () => {
        if (!state.targets.size) {
            snack('列表已经是空的', null);
            return;
        }
        const count = state.targets.size;
        const snapshot = commitTargets(() => state.targets.clear());
        snack(`已清空 ${count} 条规则`, {
            label: '撤销',
            run: () => {
                state.targets = snapshot;
                renderAll();
                scheduleSave();
            },
        });
    });

    el('add-manual').addEventListener('click', openManualSheet);
    el('manual-cancel').addEventListener('click', closeManualSheet);
    el('manual-form').addEventListener('submit', (event) => {
        event.preventDefault();
        submitManual();
    });
    el('manual-input').addEventListener('input', () => {
        el('manual-input').classList.remove('is-invalid');
        el('manual-error').hidden = true;
    });

    el('toggle-enabled').addEventListener('change', (event) => {
        state.flags.enabled = event.target.checked;
        renderSummary();
        scheduleSave();
        snack(state.flags.enabled ? '屏蔽已启用' : '屏蔽已停用，所有查询放行', null);
    });

    el('toggle-features').addEventListener('change', (event) => {
        state.flags.hide_features = event.target.checked;
        renderSummary();
        scheduleSave();
    });

    el('toggle-resources').addEventListener('change', (event) => {
        state.flags.hide_resources = event.target.checked;
        renderSummary();
        scheduleSave();
    });

    el('action-force-stop').addEventListener('click', async () => {
        if (!state.targets.size) {
            snack('还没有目标应用', null);
            return;
        }
        try {
            const output = await sh(`sh ${state.dir}/webui.sh force-stop`);
            const stopped = parseKV(output).stopped || '0';
            snack(`已停止 ${stopped} 个应用进程`, null);
        } catch (error) {
            snack(`强制停止失败：${error.message}`, null);
        }
    });

    el('action-reset').addEventListener('click', () => {
        const snapshotTargets = new Set(state.targets);
        const snapshotFlags = { ...state.flags };
        state.flags = { enabled: true, hide_features: true, hide_resources: true };
        state.targets.clear();
        renderAll();
        scheduleSave();
        snack('已恢复默认配置', {
            label: '撤销',
            run: () => {
                state.flags = snapshotFlags;
                state.targets = snapshotTargets;
                renderAll();
                scheduleSave();
            },
        });
    });

    el('copy-path').addEventListener('click', async () => {
        const text = el('config-path').textContent;
        try {
            await navigator.clipboard.writeText(text);
            snack('路径已复制', null);
        } catch (error) {
            snack(text, null);
        }
    });

    el('manual-sheet').addEventListener('click', (event) => {
        if (event.target === el('manual-sheet')) closeManualSheet();
    });
}

/* -------------------------------------------------------------------- boot */

function boot() {
    applyTheme(state.theme);
    renderKeywords();
    bindEvents();

    switchView(viewFromHash(), false);
    window.addEventListener('hashchange', () => switchView(viewFromHash(), false));

    loadAll();
}

boot();
