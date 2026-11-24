import * as kernelsu from 'kernelsu';
import { RELEASE_API, selectReleaseAsset } from './release.mjs';

import './app.css';

/*
 * bybus WebUI —— 静态控制台
 *
 * 接口契约：20-protocol.md（schema 1）
 *   - 所有控制面访问只经过 invoke(commandTokens, argsObject)。
 *   - bybusctl argv：<command tokens...> [--json '<args object>']
 *   - 每次输出单个 JSON 对象；解析失败显示 protocol_invalid。
 *   - 错误码文案只来自 CODE_TEXT（协议 §4 固定集合）。
 */
(function () {
  'use strict';

  /* =====================================================================
   * 0. 常量
   * ===================================================================== */

  var BYBUSCTL = '/data/adb/modules/bybus/bin/bybusctl';
  var POLL_FAST_MS = 1000;
  var POLL_HIDDEN_MS = 4000;
  var POLL_MAX_FAILURES = 5;
  var EXEC_TIMEOUT_MS = 40000;
  var EXEC_PROBE_TIMEOUT_MS = 3000;    /* 桥接形态尚未确定时的单形态探测超时 */
  var EXEC_ACCEPT_TIMEOUT_MS = 15000;  /* update start / snapshot build 受理超时 10s */
  var ACCEPT_FAST = { 'update start': true, 'snapshot build': true };
  var TAB_NAMES = ['overview', 'sources', 'update', 'diagnostics'];
  var TERMINAL_STATES = { done: true, failed: true, cancelled: true };
  var PREF_TAB = 'bybus.tab';
  var PREF_DIAG_OPEN = 'bybus.diagOpen';
  var SAVED_GITHUB_PAT = 'bybus.githubPat.v1';

  /* 协议 §4 固定错误码 + 两个本地桥接状态（不由协议响应提供） */
  var CODE_TEXT = {
    ok: '成功',
    usage: '命令用法错误',
    unknown_command: '未知命令',
    invalid_argument: '参数无效',
    game_missing: '未检测到游戏安装',
    text_missing: '缺少游戏原文',
    game_missing_text: '游戏原文不存在',
    archive_invalid: '归档无效',
    archive_limit: '归档超出限制',
    archive_path_unsafe: '归档路径不安全',
    archive_duplicate: '归档含重复目标',
    source_missing: '汉化源不存在',
    source_changed: '汉化源已变化',
    font_invalid: '字体文件无效',
    snapshot_invalid: '快照无效',
    snapshot_stale: '快照已过期',
    snapshot_busy: '快照正在构建',
    index_invalid: '翻译索引无效',
    snapshot_index_missing: '缺少翻译索引',
    snapshot_index_invalid: '翻译索引校验失败',
    appsealing_guard_mismatch: '保护库守卫指令不匹配',
    appsealing_exact_mismatch: '保护库完整配置校验失败',
    font_component_rejected: '字体组件准备失败',
    unsupported_runtime: '运行环境不受支持',
    zygisk_missing: '未检测到 Zygisk',
    network_unavailable: '网络不可用',
    network_rejected: '网络请求被拒绝',
    job_missing: '任务不存在',
    job_cancelled: '任务已取消',
    io_error: '读写错误',
    internal_error: '内部错误',
    /* 桥接层状态：控制程序没有给出可解析响应时使用 */
    bridge_missing: '未检测到 KernelSU WebUI 桥接',
    protocol_invalid: '控制程序返回的数据无法解析'
  };

  var MODULE_STATE_TEXT = { enabled: '已启用', disabled: '已停用', removing: '卸载中' };
  var PROVIDER_TEXT = { zygisknext: 'ZygiskNext', none: '无' };
  var TEXT_STATE_TEXT = {
    ready: '原文已就绪',
    waiting_for_game_text: '等待游戏原文',
    game_missing: '游戏未安装'
  };
  var PROFILE_STATE_TEXT = {
    ready: '兼容计划可用',
    missing: '缺少计划',
    unsupported: '保护库版本未识别',
    invalid: '计划无效'
  };
  var SNAPSHOT_STATE_TEXT = {
    missing: '未生成',
    building: '构建中',
    ready: '可用',
    invalid: '无效'
  };
  var JOB_STATE_TEXT = {
    idle: '空闲',
    queued: '排队中',
    fetching: '拉取中',
    validating: '校验中',
    pairing: '配对中',
    publishing: '发布中',
    done: '完成',
    failed: '失败',
    cancelled: '已取消'
  };
  var STAGE_TEXT = {
    queued: '排队',
    fetching: '拉取',
    validating: '校验',
    pairing: '配对',
    publishing: '发布',
    extract: '解包',
    download: '下载',
    hash: '摘要',
    font: '字体',
    scan: '扫描原文',
    done: '完成'
  };
  var KIND_TEXT = { official: '官方源', local: '本地包' };
  var FONT_TEXT = { unknown: '未知', ready: '已就绪', failed: '失败', missing: '缺失' };
  var COMPAT_TEXT = { unknown: '未知', none: '未安装', installed: '已安装', rejected: '已回滚' };

  var PENDING_TEXT = '下次启动生效：所选汉化源将在下一次游戏启动时使用。';

  /* =====================================================================
   * 1. 小工具
   * ===================================================================== */

  function $(id) {
    return document.getElementById(id);
  }

  function obj(value) {
    return value && typeof value === 'object' && !Array.isArray(value) ? value : {};
  }

  function textOf(value) {
    if (typeof value === 'string') return value;
    if (typeof value === 'number' && isFinite(value)) return String(value);
    return '';
  }

  function num(value) {
    var n = Number(value);
    return isFinite(n) ? Math.trunc(n) : 0;
  }

  function setText(id, value) {
    var node = $(id);
    if (!node) return;
    node.textContent = value === null || value === undefined ? '' : String(value);
  }

  function setHidden(id, hidden) {
    var node = $(id);
    if (node) node.hidden = !!hidden;
  }

  function codeText(code) {
    var value = textOf(code);
    if (!value) return CODE_TEXT.internal_error;
    return CODE_TEXT[value] || value;
  }

  /* 只显示映射表里的中文；缺码时显示码本身，并附原始码便于对照 */
  function showCode(id, code) {
    var node = $(id);
    if (!node) return;
    var value = textOf(code);
    if (!value || value === 'ok') {
      node.hidden = true;
      node.textContent = '';
      return;
    }
    node.hidden = false;
    var label = CODE_TEXT[value];
    node.textContent = label ? '错误：' + label + '（' + value + '）' : '错误：' + value;
  }

  function mapText(table, value, fallback) {
    var key = textOf(value);
    if (!key) return fallback || '—';
    return Object.prototype.hasOwnProperty.call(table, key) ? table[key] : (fallback || key);
  }

  function shortHash(value) {
    var text = textOf(value);
    if (!text) return '—';
    return text.length > 16 ? text.slice(0, 12) + '…' : text;
  }

  function formatBytes(value) {
    var n = Number(value);
    if (!isFinite(n) || n < 0) return '—';
    if (n < 1024) return n + ' B';
    var units = ['KiB', 'MiB', 'GiB', 'TiB', 'PiB'];
    var i = -1;
    var v = n;
    do {
      v = v / 1024;
      i += 1;
    } while (v >= 1024 && i < units.length - 1);
    return (v >= 100 ? v.toFixed(0) : v.toFixed(1)) + ' ' + units[i];
  }

  function formatTime(millis) {
    var n = Number(millis);
    if (!isFinite(n) || n <= 0) return '—';
    try {
      return new Date(n).toLocaleString('zh-CN', { hour12: false });
    } catch (err) {
      return String(n);
    }
  }

  function formatTimeShort(millis) {
    var n = Number(millis);
    if (!isFinite(n) || n <= 0) return '—';
    try {
      return new Date(n).toLocaleTimeString('zh-CN', { hour12: false });
    } catch (err) {
      return String(n);
    }
  }

  function announce(message) {
    var node = $('announcer');
    if (!node) return;
    node.textContent = textOf(message);
  }

  /* 页面偏好：仅上次标签与诊断展开状态 */
  function storeGet(key) {
    try {
      return window.localStorage.getItem(key);
    } catch (err) {
      return null;
    }
  }

  function storeSet(key, value) {
    try {
      window.localStorage.setItem(key, value);
    } catch (err) {
      /* 页面偏好写入失败不影响功能 */
    }
  }

  function validPat(value) {
    return !!value && !/[\s\x00-\x1f\x7f]/.test(value);
  }

  function restoreSavedPat() {
    var pat = storeGet(SAVED_GITHUB_PAT);
    var input = $('github-pat-input');
    if (input && validPat(pat)) input.value = pat;
  }

  function withBusy(button, task) {
    if (button) {
      button.disabled = true;
      button.setAttribute('aria-busy', 'true');
    }
    function restore() {
      if (button) {
        button.disabled = false;
        button.removeAttribute('aria-busy');
      }
    }
    return Promise.resolve().then(task).then(function (value) {
      restore();
      return value;
    }, function (error) {
      restore();
      throw error;
    });
  }

  /* =====================================================================
  /* =====================================================================
   * 2. 桥接层：使用 Vite 打包的官方 kernelsu npm 库。
   *
   * Vite 将依赖和页面打包为经典脚本，兼容不同管理器的 WebUI 加载方式。
   * exec 返回 Promise<{errno, stdout, stderr}>；超时由本层补足。
   * ===================================================================== */

  var BRIDGE_WAIT_MS = 3000;

  function bridgeApi() {
    return typeof window.ksu !== 'undefined' && window.ksu &&
      typeof window.ksu.exec === 'function' &&
      kernelsu && typeof kernelsu.exec === 'function' ? kernelsu : null;
  }

  /* 管理器可能在页面脚本之后才注入接口，因此允许短时间重试。 */
  function ensureBridge(waitMs) {
    if (bridgeApi()) return Promise.resolve(true);
    var deadline = Date.now() + (waitMs || BRIDGE_WAIT_MS);
    return new Promise(function (resolve) {
      (function poll() {
        if (bridgeApi()) {
          resolve(true);
          return;
        }
        if (Date.now() >= deadline) {
          resolve(false);
          return;
        }
        window.setTimeout(poll, 100);
      })();
    });
  }

  /* 缺桥时的可复制诊断：区分“不是在管理器里打开”和“管理器未注入接口”。 */
  function bridgeDiagnostics() {
    var lines = [];
    try { lines.push('location: ' + String(window.location && window.location.href)); } catch (err) {}
    lines.push('userAgent: ' + String(navigator.userAgent || ''));
    lines.push('window.ksu: ' + (typeof ksu));
    lines.push('kernelsu.exec: ' + (bridgeApi() ? 'function' : 'missing'));
    return lines.join('\n');
  }

  /* 仅用于字符串命令形态：把单个 argv 元素安全地交给 sh */
  function quoteArg(value) {
    var text = textOf(value);
    if (text !== '' && /^[A-Za-z0-9_@%+=:,./-]+$/.test(text)) return text;
    return "'" + text.replace(/'/g, "'\\''") + "'";
  }

  function normalizeExecResult(res) {
    var value = res && typeof res === 'object' ? res : {};
    return {
      errno: num(value.errno),
      stdout: typeof value.stdout === 'string' ? value.stdout : textOf(value.stdout),
      stderr: typeof value.stderr === 'string' ? value.stderr : textOf(value.stderr)
    };
  }

  /* 最近一次 exec 的失败细节：命令长度、errno、stderr 摘要。
   * MUI 等宿主对命令串长度敏感，空 stdout 时这些信息是唯一线索。 */
  var lastExecFailure = null;

  function describeExecFailure() {
    if (!lastExecFailure) return '';
    var parts = [lastExecFailure.status];
    parts.push('长度 ' + lastExecFailure.commandLength);
    if (lastExecFailure.errno) parts.push('errno ' + lastExecFailure.errno);
    if (lastExecFailure.stderr) parts.push('stderr ' + lastExecFailure.stderr);
    return parts.join('，');
  }

  /* 库本身没有超时参数，这里与定时器竞速。 */
  function runBridge(argv, timeoutMs) {
    var api = bridgeApi();
    if (!api) return Promise.resolve({ status: 'missing' });
    var command = BYBUSCTL + (argv.length ? ' ' + argv.map(quoteArg).join(' ') : '');
    lastExecFailure = null;
    return new Promise(function (resolve) {
      var settled = false;
      var timer = window.setTimeout(function () {
        finish({ status: 'timeout' });
      }, timeoutMs);

      function finish(outcome) {
        if (settled) return;
        settled = true;
        window.clearTimeout(timer);
        if (outcome.status !== 'ok') {
          var result = outcome.result || {};
          lastExecFailure = {
            status: outcome.status,
            commandLength: command.length,
            errno: result.errno || 0,
            stderr: textOf(result.stderr).trim().slice(0, 160)
          };
        }
        resolve(outcome);
      }

      var pending;
      try {
        pending = api.exec(command);
      } catch (err) {
        finish({ status: 'throw' });
        return;
      }
      Promise.resolve(pending).then(function (res) {
        var result = normalizeExecResult(res);
        finish({ status: result.stdout.trim() ? 'ok' : 'empty', result: result });
      }, function () {
        finish({ status: 'throw' });
      });
    });
  }

  function renderBridgeHelp() {
    var panel = $('panel-overview');
    if (!panel) return;
    if ($('bridge-help')) return;
    var box = document.createElement('details');
    box.id = 'bridge-help';
    box.className = 'bridge-help';
    box.open = true;
    var summary = document.createElement('summary');
    summary.textContent = '为什么没有桥接，以及如何继续';
    box.appendChild(summary);
    var body = document.createElement('div');
    body.className = 'bridge-help-body';
    function paragraph(value) {
      var node = document.createElement('p');
      node.textContent = value;
      body.appendChild(node);
    }
    paragraph('当前页面拿不到 kernelsu.exec，因此无法调用 bybusctl。常见原因：页面不是在 KernelSU 管理器的模块 WebUI 中打开；或管理器过旧、为未提供该接口的分支。');
    paragraph('请在 KernelSU 管理器中进入“模块 → bybus → 打开”再试。若仍失败，把下面的环境信息发给开发者。');
    var diagnostics = document.createElement('pre');
    diagnostics.className = 'code';
    diagnostics.textContent = bridgeDiagnostics();
    body.appendChild(diagnostics);
    paragraph('不依赖网页的等价命令（root shell 中执行）：');
    var commands = document.createElement('pre');
    commands.className = 'code';
    commands.textContent = [
      '/data/adb/modules/bybus/bin/bybusctl status',
      '/data/adb/modules/bybus/bin/bybusctl sources list',
      '/data/adb/modules/bybus/bin/bybusctl update start --json \'{"mode":"release"}\'',
      '/data/adb/modules/bybus/bin/bybusctl diagnostics'
    ].join('\n');
    body.appendChild(commands);
    box.appendChild(body);
    panel.insertBefore(box, panel.firstChild ? panel.firstChild.nextSibling : null);
  }

  /* =====================================================================
   * 3. invoke：唯一的控制面访问入口
   *
   * argv 固定形态：bybusctl <command tokens...> [--json '<args object>']
   *   invoke(['status'], null)
   *   invoke(['sources', 'select'], { id: 'zero' })
   *   -> ["sources", "select", "--json", '{"id":"zero"}']
   * ===================================================================== */

  function transportFailure(code) {
    return { ok: false, code: code, message: '', data: {}, transport: true };
  }

  function buildArgv(commandTokens, argsObject) {
    var tokens = [];
    if (Array.isArray(commandTokens)) {
      commandTokens.forEach(function (token) {
        var value = textOf(token).trim();
        if (value) tokens.push(value);
      });
    } else {
      var single = textOf(commandTokens).trim();
      if (single) tokens = single.split(/\s+/);
    }
    if (!tokens.length) return null;

    var argv = tokens.slice();
    if (argsObject !== null && argsObject !== undefined) {
      if (typeof argsObject !== 'object' || Array.isArray(argsObject)) {
        return { invalid: true };
      }
      argv.push('--json', JSON.stringify(argsObject));
    }
    return { argv: argv, name: tokens.join(' ') };
  }

  function invoke(commandTokens, argsObject) {
    var built = buildArgv(commandTokens, argsObject);
    if (!built) return Promise.resolve(transportFailure('protocol_invalid'));
    if (built.invalid) return Promise.resolve(transportFailure('invalid_argument'));

    var timeoutMs = ACCEPT_FAST[built.name] ? EXEC_ACCEPT_TIMEOUT_MS : EXEC_TIMEOUT_MS;

    /* 桥接未就绪时先等待注入；仍失败则按 bridge_missing 处理并给出诊断面板。 */
    return ensureBridge(BRIDGE_WAIT_MS).then(function (ready) {
      if (!ready) return { status: 'missing' };
      return runBridge(built.argv, timeoutMs);
    }).then(function (outcome) {
      if (outcome.status !== 'ok') {
        if (outcome.status === 'missing') renderBridgeHelp();
        if (outcome.status === 'timeout') return transportFailure('io_error');
        if (outcome.status === 'empty') return transportFailure('protocol_invalid');
        return transportFailure('bridge_missing');
      }

      var raw = textOf(outcome.result.stdout).trim();
      var parsed;
      try {
        parsed = JSON.parse(raw);
      } catch (err) {
        return transportFailure('protocol_invalid');
      }
      if (!parsed || typeof parsed !== 'object' || Array.isArray(parsed)) {
        return transportFailure('protocol_invalid');
      }
      if (parsed.schema !== 1) {
        return transportFailure('protocol_invalid');
      }
      if (typeof parsed.ok !== 'boolean') {
        return transportFailure('protocol_invalid');
      }
      var code = textOf(parsed.code) || (parsed.ok ? 'ok' : 'internal_error');
      return {
        ok: parsed.ok,
        code: code,
        message: textOf(parsed.message),
        data: obj(parsed.data),
        transport: false
      };
    });
  }

  /* =====================================================================
   * 4. 页面状态（全部来自控制面响应，不写 localStorage）
   * ===================================================================== */

  var latestStatus = null;
  var latestSourceId = '';
  var activeTab = 'overview';

  var jobView = { id: '', state: 'idle', stage: '', code: 'ok', errorCode: '' };
  var poll = { id: '', active: false, timer: 0, failures: 0 };
  /* 已见过终态的任务：status 短暂滞后时不要重新开始轮询 */
  var terminalJobIds = {};

  var pendingRemove = { id: '', timer: 0 };

  /* =====================================================================
   * 5. 状态渲染
   * ===================================================================== */

  function setPending(flag) {
    var show = !!flag;
    ['ov-source-pending', 'sources-pending'].forEach(function (id) {
      var node = $(id);
      if (!node) return;
      node.hidden = !show;
      if (show) node.textContent = PENDING_TEXT;
    });
  }

  function setBar(barId, fillId, percent, running) {
    var bar = $(barId);
    var fill = $(fillId);
    var hasValue = typeof percent === 'number' && isFinite(percent) && percent >= 0;
    var pct = hasValue ? Math.max(0, Math.min(100, Math.round(percent))) : 0;

    if (fill) fill.style.width = pct + '%';
    if (bar) {
      if (hasValue) {
        bar.setAttribute('aria-valuenow', String(pct));
        bar.classList.remove('is-indeterminate');
      } else if (running) {
        bar.removeAttribute('aria-valuenow');
        bar.classList.add('is-indeterminate');
      } else {
        bar.setAttribute('aria-valuenow', '0');
        bar.classList.remove('is-indeterminate');
      }
    }
  }

  function applyJobView(job) {
    var j = obj(job);
    var id = textOf(j.id);
    var state = textOf(j.state) || 'idle';
    var stage = textOf(j.stage);
    var code = textOf(j.code) || 'ok';
    var errorCode = textOf(j.errorCode);
    var done = num(j.done);
    var total = num(j.total);
    var progress = obj(j.progress);
    var bytesDone = num(progress.bytesDone);
    var bytesTotal = num(progress.bytesTotal);

    jobView = { id: id, state: state, stage: stage, code: code, errorCode: errorCode };

    var stateLabel = state === 'idle' ? '空闲' : mapText(JOB_STATE_TEXT, state, state);
    var stageLabel = stage ? mapText(STAGE_TEXT, stage, stage) : '';
    var summary = '空闲';
    if (state !== 'idle') {
      summary = id
        ? stateLabel + (stageLabel ? ' · ' + stageLabel : '') + ' · ' + id
        : stateLabel + (stageLabel ? ' · ' + stageLabel : '');
    }

    setText('job-summary', summary);
    setText('ov-job-state', id ? stateLabel + (stageLabel ? ' / ' + stageLabel : '') : '空闲');
    setText('ov-job-id', id || '—');

    var percent = -1;
    var parts = [];
    if (bytesTotal > 0) {
      percent = (bytesDone / bytesTotal) * 100;
      parts.push('字节 ' + formatBytes(bytesDone) + ' / ' + formatBytes(bytesTotal));
    }
    if (total > 0) {
      if (percent < 0) percent = (done / total) * 100;
      parts.push('项 ' + done + ' / ' + total);
    }
    if (!parts.length) parts.push('无进度信息');
    var progressText = parts.join(' · ');
    setText('job-progress', progressText);
    setText('ov-job-progress', progressText);

    var running = !!id && state !== 'idle' && !TERMINAL_STATES[state];
    setBar('job-bar', 'job-bar-fill', percent, running);

    var visibleCode = errorCode && errorCode !== 'ok' ? errorCode : (code !== 'ok' ? code : '');
    showCode('job-code', visibleCode);
    showCode('ov-job-code', visibleCode);

    var cancel = $('btn-job-cancel');
    if (cancel) {
      var canCancel = running && state !== 'publishing';
      cancel.disabled = !canCancel;
      cancel.setAttribute('aria-disabled', canCancel ? 'false' : 'true');
    }
  }

  function renderStatus(data) {
    var status = obj(data);
    latestStatus = status;

    var module = obj(status.module);
    var version = textOf(module.version);
    var versionCode = num(module.versionCode);
    setText('ov-module-version', version ? version + (versionCode ? '（' + versionCode + '）' : '') : '—');
    setText('ov-module-state', mapText(MODULE_STATE_TEXT, textOf(module.state), textOf(module.state) || '—'));

    var zygisk = obj(status.zygisk);
    if (zygisk.available === true) {
      setText('ov-zygisk', '可用（' + mapText(PROVIDER_TEXT, textOf(zygisk.provider), textOf(zygisk.provider)) + '）');
    } else {
      setText('ov-zygisk', codeText(textOf(zygisk.code) || 'zygisk_missing'));
    }

    var storage = obj(status.storage);
    var used = num(storage.usedBytes);
    var limit = num(storage.limitBytes);
    setText('ov-storage', formatBytes(used) + (limit > 0 ? ' / ' + formatBytes(limit) : '（不限额）'));

    var game = obj(status.game);
    var installed = game.installed === true;
    setText('ov-game-state', installed ? '已安装' : '未安装');
    setText('ov-game-package', textOf(game.packageName) || '—');
    var gameName = textOf(game.versionName);
    var gameCode = num(game.versionCode);
    setText('ov-game-version', gameName || gameCode ? (gameName || '—') + '（' + gameCode + '）' : '—');
    setText('ov-game-text', mapText(TEXT_STATE_TEXT, textOf(game.textState), textOf(game.textState) || '—'));
    var locales = Array.isArray(game.locales) ? game.locales.filter(function (item) {
      return typeof item === 'string' && item;
    }) : [];
    setText('ov-game-locales', locales.length ? locales.join('、') : '—');
    setText('ov-game-tables', String(num(game.tableCount)));
    var profileState = textOf(game.profileState) || '—';
    var profileCode = textOf(game.profileCode);
    setText('ov-game-profile', mapText(PROFILE_STATE_TEXT, profileState, profileState) +
      (profileCode && profileCode !== 'ok' ? '（' + codeText(profileCode) + '）' : ''));
    showCode('ov-game-code', textOf(game.textCode) !== 'ok' ? textOf(game.textCode) : '');

    var snapshot = obj(status.snapshot);
    setText('ov-snapshot-state', mapText(SNAPSHOT_STATE_TEXT, textOf(snapshot.state), textOf(snapshot.state) || '—'));
    setText('ov-snapshot-generation', shortHash(textOf(snapshot.generation)));
    setText('ov-snapshot-source', textOf(snapshot.sourceId) || '—');
    setText('ov-snapshot-built', formatTime(num(snapshot.builtAtMillis)));
    setText('ov-snapshot-tables', String(num(snapshot.tableCount)));
    setText('ov-snapshot-schema', String(num(snapshot.runtimeSchema)));
    showCode('ov-snapshot-code', textOf(snapshot.code) !== 'ok' ? textOf(snapshot.code) : '');

    var source = obj(status.source);
    latestSourceId = textOf(source.id);
    setText('ov-source-name', textOf(source.name) || (latestSourceId || '—'));
    setText('ov-source-id', latestSourceId || '—');
    setText('ov-source-kind', textOf(source.kind) ? mapText(KIND_TEXT, textOf(source.kind), textOf(source.kind)) : '—');
    setText('ov-source-tables', String(num(source.tableCount)));
    setText('ov-source-revision', shortHash(textOf(source.revision)));
    setPending(source.pendingActivation === true);

    var job = obj(status.job);
    applyJobView(job);

    showCode('ov-status-code', textOf(status.code) !== 'ok' ? textOf(status.code) : '');

    var jobId = textOf(job.id);
    var jobState = textOf(job.state) || 'idle';
    if (TERMINAL_STATES[jobState]) rememberTerminalJob(jobId);
    if (jobId && jobState !== 'idle' && !TERMINAL_STATES[jobState] && !terminalJobIds[jobId]) {
      ensurePolling(jobId);
    } else if (!jobId) {
      stopPoll();
    }
  }

  function refreshStatus() {
    return invoke(['status'], null).then(function (res) {
      if (!res.ok) {
        showCode('ov-status-code', res.code);
        announce('状态读取失败：' + codeText(res.code));
        return res;
      }
      renderStatus(res.data);
      return res;
    });
  }

  /* =====================================================================
   * 6. 任务轮询
   * ===================================================================== */

  function pollDelay() {
    return document.hidden ? POLL_HIDDEN_MS : POLL_FAST_MS;
  }

  function schedulePoll(delay) {
    if (poll.timer) window.clearTimeout(poll.timer);
    poll.timer = window.setTimeout(pollOnce, Math.max(0, delay));
  }

  function stopPoll() {
    poll.active = false;
    poll.id = '';
    poll.failures = 0;
    if (poll.timer) {
      window.clearTimeout(poll.timer);
      poll.timer = 0;
    }
  }

  function rememberTerminalJob(jobId) {
    var id = textOf(jobId);
    if (!id || terminalJobIds[id]) return;
    var keys = Object.keys(terminalJobIds);
    if (keys.length >= 20) delete terminalJobIds[keys[0]];
    terminalJobIds[id] = true;
  }

  function ensurePolling(jobId) {
    var id = textOf(jobId);
    if (!id) return;
    if (poll.active && poll.id === id) return;
    poll.id = id;
    poll.active = true;
    poll.failures = 0;
    schedulePoll(0);
  }

  function pollOnce() {
    poll.timer = 0;
    if (!poll.active || !poll.id) return;

    var id = poll.id;
    invoke(['job', 'status'], { id: id }).then(function (res) {
      if (!poll.active || poll.id !== id) return;

      if (res.ok) {
        poll.failures = 0;
        var state = textOf(res.data.state) || '';
        applyJobView(res.data);
        if (TERMINAL_STATES[state]) {
          stopPoll();
          rememberTerminalJob(id);
          if (state === 'done') {
            announce('任务完成，快照已发布');
          } else if (state === 'failed') {
            announce('任务失败：' + codeText(textOf(res.data.errorCode) || textOf(res.data.code)));
          } else {
            announce('任务已取消');
          }
          refreshStatus();
          return;
        }
      } else {
        poll.failures += 1;
        showCode('job-code', res.code);
        if (res.code === 'job_missing' || poll.failures >= POLL_MAX_FAILURES) {
          stopPoll();
          announce('任务状态读取中止：' + codeText(res.code));
          return;
        }
      }

      if (poll.active) schedulePoll(pollDelay());
    });
  }

  /* =====================================================================
   * 7. 来源列表
   * ===================================================================== */

  function clearChildren(node) {
    if (!node) return;
    while (node.firstChild) node.removeChild(node.firstChild);
  }

  function resetPendingRemove() {
    if (pendingRemove.timer) window.clearTimeout(pendingRemove.timer);
    pendingRemove.id = '';
    pendingRemove.timer = 0;
  }

  function renderSourceList(data) {
    var list = obj(data);
    var sources = Array.isArray(list.sources) ? list.sources : [];
    var selectedId = textOf(list.selectedId);
    var ul = $('source-list');
    if (!ul) return;

    clearChildren(ul);

    if (!sources.length) {
      var empty = document.createElement('li');
      empty.className = 'source-empty';
      empty.textContent = '没有已安装的汉化源。';
      ul.appendChild(empty);
      return;
    }

    sources.forEach(function (rawSource) {
      var source = obj(rawSource);
      var id = textOf(source.id);
      var selected = !!id && id === selectedId;
      var revision = textOf(source.revision);
      var updated = num(source.updatedAtMillis);

      var li = document.createElement('li');
      li.className = 'source-item' + (selected ? ' is-selected' : '');

      var head = document.createElement('div');
      head.className = 'source-head';

      var name = document.createElement('p');
      name.className = 'source-name';
      name.textContent = textOf(source.name) || id || '未命名来源';
      head.appendChild(name);

      if (selected) {
        var badge = document.createElement('span');
        badge.className = 'badge';
        badge.textContent = '当前';
        head.appendChild(badge);
      }
      li.appendChild(head);

      var metaParts = [
        mapText(KIND_TEXT, textOf(source.kind), textOf(source.kind) || '类型未知'),
        'ID ' + (id || '—'),
        num(source.tableCount) + ' 表'
      ];
      if (revision) metaParts.push('修订 ' + shortHash(revision));
      if (updated > 0) metaParts.push('更新于 ' + formatTime(updated));

      var meta = document.createElement('p');
      meta.className = 'source-meta';
      meta.textContent = metaParts.join(' · ');
      li.appendChild(meta);

      var actions = document.createElement('div');
      actions.className = 'btn-row';

      var selectBtn = document.createElement('button');
      selectBtn.type = 'button';
      selectBtn.className = 'btn btn-small';
      selectBtn.textContent = selected ? '已选用' : '选用';
      selectBtn.disabled = selected || !id;
      selectBtn.setAttribute('aria-label', '选用汉化源 ' + (textOf(source.name) || id || ''));
      selectBtn.addEventListener('click', function () {
        selectSource(id);
      });
      actions.appendChild(selectBtn);

      var removeBtn = document.createElement('button');
      removeBtn.type = 'button';
      removeBtn.className = 'btn btn-small btn-danger';
      removeBtn.textContent = '删除';
      removeBtn.disabled = !id;
      removeBtn.setAttribute('aria-label', '删除汉化源 ' + (textOf(source.name) || id || ''));
      removeBtn.addEventListener('click', function () {
        askRemoveSource(id, name.textContent, removeBtn);
      });
      actions.appendChild(removeBtn);

      li.appendChild(actions);
      ul.appendChild(li);
    });
  }

  function loadSources() {
    setHidden('sources-hint', false);
    setText('sources-hint', '正在读取来源列表…');
    return invoke(['sources', 'list'], null).then(function (res) {
      if (!res.ok) {
        showCode('sources-code', res.code);
        setText('sources-hint', '无法读取来源列表。');
        announce('来源列表读取失败：' + codeText(res.code));
        return res;
      }
      showCode('sources-code', '');
      renderSourceList(res.data);
      setHidden('sources-hint', true);
      return res;
    });
  }

  function selectSource(id) {
    if (!id) return Promise.resolve(null);
    resetPendingRemove();
    return invoke(['sources', 'select'], { id: id }).then(function (res) {
      if (!res.ok) {
        showCode('sources-code', res.code);
        announce('切换汉化源失败：' + codeText(res.code));
        return res;
      }
      var effective = textOf(res.data.effective);
      showCode('sources-code', '');
      announce('已切换汉化源，' + (effective === 'next_launch' ? '下次启动生效' : '等待后台确认'));
      return refreshStatus().then(function () {
        return loadSources();
      });
    });
  }

  function askRemoveSource(id, displayName, button) {
    if (!id) return;
    if (pendingRemove.id === id) {
      resetPendingRemove();
      button.textContent = '删除';
      removeSource(id);
      return;
    }
    resetPendingRemove();
    pendingRemove.id = id;
    button.textContent = '确认删除';
    announce('再次点按以确认删除 ' + displayName);
    pendingRemove.timer = window.setTimeout(function () {
      pendingRemove.id = '';
      button.textContent = '删除';
    }, 5000);
  }

  function removeSource(id) {
    return invoke(['sources', 'remove'], { id: id }).then(function (res) {
      if (!res.ok) {
        showCode('sources-code', res.code);
        announce('删除汉化源失败：' + codeText(res.code));
        return res;
      }
      renderSourceList(res.data);
      announce('已删除汉化源 ' + id);
      return refreshStatus();
    });
  }

  /* =====================================================================
   * 8. 长任务：update start / snapshot build / job cancel
   * ===================================================================== */

  function adoptJob(data, fallbackState) {
    var job = obj(data);
    var jobId = textOf(job.jobId) || textOf(job.id);
    if (!jobId) return '';
    applyJobView({
      id: jobId,
      state: textOf(job.state) || fallbackState || 'queued',
      stage: '',
      done: 0,
      total: 0,
      code: 'ok',
      errorCode: ''
    });
    ensurePolling(jobId);
    return jobId;
  }

  function startUpdate(mode, url, expectedSha256) {
    var args = { mode: mode };
    if (mode === 'url') args.url = url;
    if (mode === 'release' && url) {
      args.assetUrl = url;
      if (expectedSha256) args.expectedSha256 = expectedSha256;
    }
    var codeSlot = mode === 'url' ? 'url-code' : 'job-code';
    showCode(codeSlot, '');
    return invoke(['update', 'start'], args).then(function (res) {
      if (!res.ok) {
        showCode(codeSlot, res.code);
        announce('启动更新失败：' + codeText(res.code));
        return res;
      }
      var jobId = adoptJob(res.data, 'queued');
      if (!jobId) {
        showCode(codeSlot, 'protocol_invalid');
        return res;
      }
      announce('已提交后台任务 ' + jobId);
      return res;
    });
  }

  function startPatRelease(pat) {
    if (!validPat(pat)) {
      setImportNote('github-pat-hint', 'PAT 格式无效，请检查是否含有空格或换行。');
      return Promise.resolve(null);
    }
    if (typeof window.fetch !== 'function' || typeof window.AbortController !== 'function') {
      setImportNote('github-pat-hint', '当前 WebView 不支持直接请求 GitHub API。');
      return Promise.resolve(null);
    }
    setImportNote('github-pat-hint', '正在通过 PAT 查询 GitHub Release…');
    var controller = new window.AbortController();
    var timer = window.setTimeout(function () { controller.abort(); }, 15000);
    return window.fetch(RELEASE_API, {
      method: 'GET',
      headers: {
        'Accept': 'application/vnd.github+json',
        'Authorization': 'Bearer ' + pat
      },
      cache: 'no-store',
      credentials: 'omit',
      redirect: 'error',
      referrerPolicy: 'no-referrer',
      signal: controller.signal
    }).then(function (response) {
      if (!response.ok) {
        var message = response.status === 401
          ? 'PAT 无效或已过期。'
          : response.status === 429 || response.headers.get('x-ratelimit-remaining') === '0'
            ? 'GitHub API 配额已用尽，请稍后再试。'
            : 'GitHub API 拒绝请求，请检查 PAT 是否有效及网络状态。';
        throw { userMessage: message };
      }
      return response.json();
    }).then(function (release) {
      var asset = selectReleaseAsset(release);
      if (!asset) throw { userMessage: '最新 Release 中没有符合格式的汉化归档。' };
      setImportNote('github-pat-hint', '已通过 PAT 获取 Release 元数据；归档由后台下载。');
      return startUpdate('release', asset.url, asset.digest);
    }).catch(function (error) {
      var message = error && error.userMessage
        ? error.userMessage : 'GitHub API 请求失败或超时，请检查网络后重试。';
      setImportNote('github-pat-hint', message);
      announce(message);
      return null;
    }).finally(function () {
      window.clearTimeout(timer);
    });
  }

  function buildSnapshot() {
    if (!latestSourceId) {
      showCode('job-code', 'source_missing');
      announce('没有可用的汉化源');
      return Promise.resolve(null);
    }
    showCode('job-code', '');
    return invoke(['snapshot', 'build'], { sourceId: latestSourceId }).then(function (res) {
      if (!res.ok) {
        showCode('job-code', res.code);
        announce('启动快照构建失败：' + codeText(res.code));
        return res;
      }
      var jobId = adoptJob(res.data, 'queued');
      if (!jobId) {
        showCode('job-code', 'protocol_invalid');
        return res;
      }
      announce('已提交快照构建任务 ' + jobId);
      return res;
    });
  }

  function cancelJob() {
    var id = jobView.id;
    if (!id) return Promise.resolve(null);
    return invoke(['job', 'cancel'], { id: id }).then(function (res) {
      if (!res.ok) {
        showCode('job-code', res.code);
        announce('取消任务失败：' + codeText(res.code));
        return res;
      }
      /* 后台运行中的任务只接受了取消请求，须等 job status 确认终态。 */
      announce('已提交取消请求');
      return refreshStatus();
    });
  }

  /* =====================================================================
   * 9. 本地包导入（路径式）
   *
   * 守护进程以 root 运行，直接读取用户给的绝对路径并复制，不经 WebView 上传。
   *   invoke(['update', 'start'], { mode: 'path', path: '/sdcard/Download/x.7z' })
   * ===================================================================== */

  function setImportPhase(text) {
    setText('import-phase', text);
  }

  function setImportNote(id, message) {
    var node = $(id);
    if (!node) return;
    if (!message) {
      node.hidden = true;
      node.textContent = '';
      return;
    }
    node.hidden = false;
    node.textContent = message;
  }

  function startPathImport(rawPath) {
    var value = textOf(rawPath).trim();
    showCode('import-code', '');
    setImportPhase('准备导入');
    if (value.length === 0) {
      setImportPhase('路径为空');
      showCode('import-code', 'invalid_argument');
      announce('请填写汉化包的绝对路径');
      return Promise.resolve(null);
    }
    if (value.charAt(0) !== '/' || value.length > 4096 ||
        value.split('/').indexOf('..') >= 0 || /[\x00-\x1f]/.test(value)) {
      setImportPhase('路径无效');
      showCode('import-code', 'invalid_argument');
      announce('请填写设备上的绝对文件路径');
      return Promise.resolve(null);
    }
    setImportPhase('已提交，后台复制中');
    return invoke(['update', 'start'], { mode: 'path', path: value }).then(function (res) {
      if (!res.ok) {
        var detail = describeExecFailure();
        setImportPhase(detail ? '导入失败（' + detail + '）' : '导入失败');
        showCode('import-code', res.code);
        announce('导入失败：' + codeText(res.code));
        return res;
      }
      var jobId = textOf(res.data.jobId);
      if (!jobId) {
        setImportPhase('导入失败');
        showCode('import-code', 'protocol_invalid');
        announce('导入失败：后台没有返回任务 ID');
        return res;
      }
      applyJobView({
        id: jobId,
        state: 'queued',
        stage: '',
        done: 0,
        total: 0,
        code: 'ok',
        errorCode: ''
      });
      ensurePolling(jobId);
      setImportPhase('已提交：' + jobId);
      announce('已提交路径导入任务');
      return res;
    });
  }

  /* =====================================================================
   * 10. 诊断
   * ===================================================================== */

  function renderDiagnostics(data) {
    var diag = obj(data);
    var runtime = obj(diag.runtime);
    var build = obj(diag.build);
    var log = Array.isArray(diag.log) ? diag.log : [];

    setText('diag-runtime-generation', shortHash(textOf(runtime.generation)));
    setText('diag-hits', num(runtime.hits) + ' / ' + num(runtime.misses));
    setText('diag-font', textOf(runtime.fontState)
      ? mapText(FONT_TEXT, textOf(runtime.fontState), textOf(runtime.fontState))
      : '—');
    setText('diag-compat', textOf(runtime.compatState)
      ? mapText(COMPAT_TEXT, textOf(runtime.compatState), textOf(runtime.compatState))
      : '—');
    setText('diag-runtime-code', codeText(textOf(runtime.code) || 'ok'));

    var lastGen = textOf(build.lastGeneration);
    var lastCode = textOf(build.lastCode);
    setText('diag-build-last', (lastGen ? shortHash(lastGen) : '—') + (lastCode ? ' · ' + codeText(lastCode) : ''));
    setText('diag-build-counts', '构建 ' + num(build.generations) + ' 次 · 丢弃 ' + num(build.discarded));
    showCode('diag-code', textOf(diag.code) !== 'ok' ? textOf(diag.code) : '');

    var list = $('diag-log');
    if (!list) return;
    clearChildren(list);

    if (!log.length) {
      var empty = document.createElement('li');
      empty.className = 'log-empty';
      empty.textContent = '暂无记录。';
      list.appendChild(empty);
      return;
    }

    log.slice(0, 200).forEach(function (rawEntry) {
      var entry = obj(rawEntry);
      var li = document.createElement('li');
      li.className = 'log-row';
      /* 只渲染阶段、计数与错误码，不显示任何疑似正文的字段 */
      li.textContent = [
        formatTimeShort(num(entry.atMillis)),
        mapText(STAGE_TEXT, textOf(entry.stage), textOf(entry.stage) || '未知阶段'),
        codeText(textOf(entry.code) || 'ok'),
        '计数 ' + num(entry.count)
      ].join(' · ');
      list.appendChild(li);
    });
  }

  function loadDiagnostics() {
    return invoke(['diagnostics'], null).then(function (res) {
      if (!res.ok) {
        showCode('diag-code', res.code);
        announce('诊断读取失败：' + codeText(res.code));
        return res;
      }
      renderDiagnostics(res.data);
      return res;
    });
  }

  /* =====================================================================
   * 11. 标签页
   * ===================================================================== */

  function activateTab(name, options) {
    var target = TAB_NAMES.indexOf(name) >= 0 ? name : 'overview';
    activeTab = target;

    TAB_NAMES.forEach(function (tab) {
      var button = $('tab-' + tab);
      var panel = $('panel-' + tab);
      var on = tab === target;
      if (button) {
        button.setAttribute('aria-selected', on ? 'true' : 'false');
        button.tabIndex = on ? 0 : -1;
        if (on) button.classList.add('is-active');
        else button.classList.remove('is-active');
      }
      if (panel) panel.hidden = !on;
    });

    storeSet(PREF_TAB, target);

    if (options && options.load) {
      if (target === 'sources') loadSources();
      if (target === 'diagnostics') loadDiagnostics();
    }
  }

  function bindTabs() {
    var nav = document.querySelector('.tabs');
    if (!nav) return;

    nav.addEventListener('click', function (event) {
      var node = event.target;
      var button = node && node.closest ? node.closest('[data-tab]') : null;
      if (!button) return;
      activateTab(button.getAttribute('data-tab'), { load: true });
    });

    nav.addEventListener('keydown', function (event) {
      var key = event.key;
      if (key !== 'ArrowRight' && key !== 'ArrowLeft' && key !== 'Home' && key !== 'End') return;
      var index = TAB_NAMES.indexOf(activeTab);
      if (key === 'ArrowRight') index = (index + 1) % TAB_NAMES.length;
      if (key === 'ArrowLeft') index = (index - 1 + TAB_NAMES.length) % TAB_NAMES.length;
      if (key === 'Home') index = 0;
      if (key === 'End') index = TAB_NAMES.length - 1;
      event.preventDefault();
      var next = TAB_NAMES[index];
      activateTab(next, { load: true });
      var button = $('tab-' + next);
      if (button) button.focus();
    });
  }

  /* =====================================================================
   * 12. 事件绑定与启动
   * ===================================================================== */

  function on(id, type, handler) {
    var node = $(id);
    if (node) node.addEventListener(type, handler);
  }

  function bindEvents() {
    bindTabs();

    on('sources-refresh', 'click', function () {
      withBusy($('sources-refresh'), function () {
        return loadSources();
      });
    });

    on('btn-update-release', 'click', function () {
      withBusy($('btn-update-release'), function () {
        var input = $('github-pat-input');
        var pat = textOf(input && input.value);
        setImportNote('github-pat-hint', '');
        return pat ? startPatRelease(pat.trim()) : startUpdate('release');
      });
    });

    on('btn-pat-save', 'click', function () {
      var input = $('github-pat-input');
      var pat = textOf(input && input.value).trim();
      if (!validPat(pat)) {
        setImportNote('github-pat-hint', 'PAT 格式无效，请检查是否含有空格或换行。');
        return;
      }
      try {
        window.localStorage.setItem(SAVED_GITHUB_PAT, pat);
        input.value = pat;
        setImportNote('github-pat-hint', 'PAT 已保存，下次打开会自动填入。');
      } catch (err) {
        setImportNote('github-pat-hint', '保存失败：当前 WebView 本地存储不可用。');
      }
    });

    on('btn-pat-clear', 'click', function () {
      var input = $('github-pat-input');
      if (input) input.value = '';
      try {
        window.localStorage.removeItem(SAVED_GITHUB_PAT);
        setImportNote('github-pat-hint', 'PAT 已从当前页面和保存记录中清除。');
      } catch (err) {
        setImportNote('github-pat-hint', '输入框已清除，但本地存储不可用；无法确认保存记录已删除。');
      }
    });

    on('btn-update-inbox', 'click', function () {
      withBusy($('btn-update-inbox'), function () {
        return startUpdate('inbox');
      });
    });

    on('btn-snapshot-build', 'click', function () {
      withBusy($('btn-snapshot-build'), function () {
        return buildSnapshot();
      });
    });

    on('btn-job-cancel', 'click', function () {
      withBusy($('btn-job-cancel'), function () {
        return cancelJob();
      });
    });

    on('btn-import-path', 'click', function () {
      withBusy($('btn-import-path'), function () {
        return startPathImport($('path-input') && $('path-input').value);
      });
    });

    on('btn-update-url', 'click', function () {
      var input = $('url-input');
      var raw = textOf(input && input.value).trim();
      setImportNote('url-hint', '');
      showCode('url-code', '');
      if (!/^https:\/\/\S+$/i.test(raw)) {
        setImportNote('url-hint', '请输入以 https:// 开头的归档地址。');
        announce('归档地址必须是 HTTPS');
        return;
      }
      withBusy($('btn-update-url'), function () {
        return startUpdate('url', raw);
      });
    });

    on('diag-refresh', 'click', function () {
      withBusy($('diag-refresh'), function () {
        return loadDiagnostics();
      });
    });

    on('diag-log-details', 'toggle', function () {
      var details = $('diag-log-details');
      storeSet(PREF_DIAG_OPEN, details && details.open ? '1' : '0');
    });

    document.addEventListener('visibilitychange', function () {
      if (poll.active) schedulePoll(pollDelay());
    });

    window.addEventListener('beforeunload', function () {
      stopPoll();
    });
  }

  function restorePreferences() {
    restoreSavedPat();
    var savedTab = storeGet(PREF_TAB);
    activateTab(TAB_NAMES.indexOf(savedTab) >= 0 ? savedTab : 'overview', { load: false });

    if (storeGet(PREF_DIAG_OPEN) === '0') {
      var details = $('diag-log-details');
      if (details) details.open = false;
    }
  }

  function init() {
    bindEvents();
    restorePreferences();

    /* 首次加载只用一个 status 请求完成初始渲染 */
    refreshStatus();
  }

  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', init);
  } else {
    init();
  }

  /* 调试入口（只读，便于审计） */
  window.bybus = Object.freeze({
    invoke: invoke,
    refresh: function () {
      return refreshStatus();
    },
    diagnostics: function () {
      return loadDiagnostics();
    },
    state: function () {
      return {
        tab: activeTab,
        job: { id: jobView.id, state: jobView.state },
        polling: poll.active,
        bridge: bridgeApi() ? 'kernelsu' : 'unresolved'
      };
    }
  });
})();
