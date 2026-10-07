// CameraPlus: the tuning panel. cameraplus.dll owns the panel (rows, values, keys, the controller); this script
// draws it on the left of the screen, with the list of keys on the right, from the DLL's status, and redraws when the
// status's serial changes. It also
// passes the HUD's combat flag to the DLL, and tells it when the panel key or button changes on the MODS page.
(function () {
    'use strict';
    if (window.__CameraPlusInstalled) return;
    window.__CameraPlusInstalled = true;

    var C = window.__CameraPlusConfig || {};
    var DIAG = !!C.diagnostics;
    var URL = 'coui://base/__cameraplus__.json';
    var LOG_URL = 'coui://base/__cameraplus_log__.json';
    var MOD_ID = 'cameraplus', KEY_OPTION = 'panel_hotkey', PAD_OPTION = 'panel_pad', PAD2_OPTION = 'panel_pad2';
    var OPEN_MS = 50, CLOSED_MS = 150;  // how often the status is read, with the panel open / closed
    var YELLOW = '#fbe732';             // the game's objective yellow (RadarPlus uses it too)
    var V = function (px) { return (px / 10.8).toFixed(5) + 'vh'; };  // 1080p pixels to vh
    var CSS =
        '.cp-panel{position:absolute;left:3.7037037037vh;top:50%;transform:translateY(-50%);width:44.444444vh;' +
        'padding:2.2222222vh 2.2222222vh 1.8518518vh;box-sizing:border-box;background-color:rgba(13,13,13,0.95);' +
        'border-left:0.37037037vh solid rgb(232,232,232);color:rgb(232,232,232);z-index:100000;pointer-events:none}' +
        '.cp-title{display:flex;flex-direction:row;font-size:1.4814815vh;letter-spacing:0.37037037vh;font-weight:700;' +
        'margin-bottom:1.6666667vh}' +
        // The gap between the words is a margin: Gameface drops a space at the end of CAMERA.
        '.cp-title__camera{opacity:0.7}.cp-title__plus{margin-left:0.7407407vh;color:' + YELLOW + '}' +
        '.cp-row{display:flex;justify-content:space-between;align-items:center;height:3.8888889vh;padding:0 1.1111111vh;' +
        'font-size:1.9444444vh}' +
        '.cp-row--sel{background-color:rgb(232,232,232);color:rgb(13,13,13)}' +
        '.cp-value{display:flex;flex-direction:row;align-items:center;font-variant-numeric:tabular-nums;white-space:nowrap}' +
        '.cp-arrow--left{margin-right:0.9259259vh}.cp-arrow--right{margin-left:0.9259259vh}' +
        '.cp-hint{margin-top:1.6666667vh;font-size:1.4814815vh;line-height:2.2222222vh;opacity:0.6}' +
        '.cp-hidden{display:none !important}' +
        // The list of keys (right), as MusicVideo's editor has it.
        '.cp-keys{position:absolute;right:3.7037037037vh;top:50%;transform:translateY(-50%);width:' + V(440) + ';' +
        'padding:' + V(18) + ' ' + V(20) + ' ' + V(10) + ';box-sizing:border-box;background-color:rgba(13,13,13,0.95);' +
        'border-right:0.37037037vh solid rgb(232,232,232);color:rgb(232,232,232);z-index:100000;pointer-events:none}' +
        '.cp-kgroup{margin-bottom:' + V(12) + '}' +
        '.cp-ktitle{font-size:' + V(13) + ';letter-spacing:' + V(3) + ';font-weight:700;color:' + YELLOW + ';margin-bottom:' + V(6) + '}' +
        '.cp-krow{display:flex;flex-direction:row;align-items:center;height:' + V(24) + '}' +
        '.cp-kkeys{display:flex;flex-direction:row;align-items:center;width:' + V(170) + ';flex-shrink:0}' +
        '.cp-kcap{display:flex;flex-direction:row;align-items:center;justify-content:center;min-width:' + V(21) + ';height:' + V(19) + ';' +
        'padding:0 ' + V(5) + ';margin-right:' + V(4) + ';box-sizing:border-box;border:1px solid rgba(232,232,232,0.55);' +
        'border-radius:' + V(3) + ';background-color:rgba(232,232,232,0.12);font-size:' + V(12) + ';font-weight:700;white-space:nowrap}' +
        '.cp-kor{font-size:' + V(12) + ';opacity:0.55;margin-right:' + V(4) + ';white-space:nowrap}' +
        '.cp-kdesc{font-size:' + V(15) + ';white-space:nowrap}';

    var logCount = 0;
    function log(msg) {
        if (!DIAG || ++logCount > 200) return;
        try {
            var x = new XMLHttpRequest();
            x.open('GET', LOG_URL + '?m=' + encodeURIComponent(String(msg).slice(0, 1400)), true);
            x.send();
        } catch (e) {}
    }
    function el(tag, cls, text) {
        var n = document.createElement(tag);
        if (cls) n.className = cls;
        if (text !== undefined) n.textContent = text;
        return n;
    }
    function remove(n) { if (n && n.parentNode) n.parentNode.removeChild(n); }
    function connected(n) { for (var i = 0; n && i < 64; i++, n = n.parentNode) if (n === document.body) return true; return false; }

    // ---------------------------------------------------------------- the panel
    var panel = null, keys = null, lastSerial = -1;

    function ensure() {
        if (panel && keys && connected(panel) && connected(keys)) return panel;
        if (!document.body) return null;
        if (!document.getElementById('cp-style')) {
            var st = el('style');
            st.id = 'cp-style';
            st.textContent = CSS;
            (document.head || document.body).appendChild(st);
        }
        remove(panel);
        remove(keys);
        panel = el('div', 'cp-panel cp-hidden');
        keys = el('div', 'cp-keys cp-hidden');
        document.body.appendChild(panel);
        document.body.appendChild(keys);
        lastSerial = -1;
        return panel;
    }

    function hints(p, s) {
        if (s.capture) {
            p.appendChild(el('div', 'cp-hint', s.pad ? 'Press the new zoom button (B cancels)'
                : 'Press the new zoom key or button (Esc cancels)'));
            return;
        }
        p.appendChild(el('div', 'cp-hint', 'Showing the ' + (s.edit || '') + ' camera while this is open'));
        if (s.keyList === false) p.appendChild(el('div', 'cp-hint', s.pad ? 'Y: show the buttons' : 'I: show the keys'));
    }

    // ---------------------------------------------------------------- the list of keys (right)
    // Each row: key caps ({w: ...} is a plain word between them), then what they do. The keyboard's keys or the
    // controller's buttons, whichever drove the panel last; the panel and zoom keys and buttons are the ones set, and
    // the wheel and touchpad show while they zoom.
    function keyGroups(s) {
        var here, play = [];
        if (s.pad) {
            here = [[['D-pad', { w: 'up / down' }], 'Choose a setting'],
                [['D-pad', { w: 'left / right' }], 'Change it'],
                [['LB'], 'Finer steps (hold)'],
                [['A'], 'Set a button, switch, reset'],
                [['X'], "Back to the game's value"],
                [['Sticks'], 'Move and look while you tune']];
            if (s.touch) here.push([['Touchpad'], "Swipe: this camera's distance"]);
            here.push([['Y'], 'Hide this list'], [['B'], 'Close']);
            if (s.zoomButton) play.push([[s.zoomButton], s.zoomToggle ? 'Zoom on / off' : 'Zoom (hold)']);
            if (s.touch) play.push([['Touchpad'], 'Swipe: camera closer / further']);
            if (s.button) play.push([s.button2 ? [s.button, { w: '+' }, s.button2] : [s.button], 'Open this panel']);
        } else {
            here = [[['Up', 'Down'], 'Choose a setting'],
                [['Left', 'Right'], 'Change it'],
                [['Shift'], 'Finer steps (hold)'],
                [['Enter'], 'Set a key, switch, reset'],
                [['Delete'], "Back to the game's value"],
                [['Mouse'], 'Look around while you tune']];
            if (s.wheel) here.push([['Wheel'], "This camera's distance"]);
            here.push([['I'], 'Hide this list'], [[s.key || 'F1', { w: 'or' }, 'Esc'], 'Close']);
            if (s.zoomKey) play.push([[s.zoomKey], s.zoomToggle ? 'Zoom on / off' : 'Zoom (hold)']);
            if (s.wheel) play.push([['Wheel'], 'Camera closer / further']);
            if (s.key) play.push([[s.key], 'Open this panel']);
        }
        return [{ title: 'THIS PANEL', rows: here }, { title: 'IN PLAY', rows: play }];
    }

    function renderKeys(s) {
        while (keys.firstChild) keys.removeChild(keys.firstChild);
        keyGroups(s).forEach(function (g) {
            if (!g.rows.length) return;
            var group = el('div', 'cp-kgroup');
            group.appendChild(el('div', 'cp-ktitle', g.title));
            g.rows.forEach(function (r) {
                var row = el('div', 'cp-krow'), caps = el('div', 'cp-kkeys');
                r[0].forEach(function (k) {
                    caps.appendChild(typeof k === 'string' ? el('div', 'cp-kcap', k) : el('div', 'cp-kor', k.w));
                });
                row.appendChild(caps);
                row.appendChild(el('div', 'cp-kdesc', r[1]));
                group.appendChild(row);
            });
            keys.appendChild(group);
        });
        keys.className = s.keyList === false ? 'cp-keys cp-hidden' : 'cp-keys';
    }

    function render(s) {
        var p = ensure();
        if (!p) return;
        if (!s.open) {
            if (p.className.indexOf('cp-hidden') < 0) p.className = 'cp-panel cp-hidden';
            if (keys.className.indexOf('cp-hidden') < 0) keys.className = 'cp-keys cp-hidden';
            lastSerial = s.serial;
            return;
        }
        if (s.serial === lastSerial && p.className.indexOf('cp-hidden') < 0) return;
        lastSerial = s.serial;
        while (p.firstChild) p.removeChild(p.firstChild);
        var title = el('div', 'cp-title');
        title.appendChild(el('span', 'cp-title__camera', 'CAMERA'));
        title.appendChild(el('span', 'cp-title__plus', 'PLUS'));
        p.appendChild(title);
        (s.rows || []).forEach(function (r, i) {
            var sel = i === s.sel;
            var row = el('div', 'cp-row' + (sel ? ' cp-row--sel' : ''));
            row.appendChild(el('span', 'cp-label', r[0]));
            var value = el('div', 'cp-value');
            if (sel) value.appendChild(el('span', 'cp-arrow cp-arrow--left', '<'));
            value.appendChild(el('span', 'cp-value__text', r[1]));
            if (sel) value.appendChild(el('span', 'cp-arrow cp-arrow--right', '>'));
            row.appendChild(value);
            p.appendChild(row);
        });
        hints(p, s);
        p.className = 'cp-panel';
        renderKeys(s);
    }

    // ---------------------------------------------------------------- the MODS page
    // The panel key and buttons are Mod Settings Menu key options (1.7.1): it shows the key's name or the button's icon
    // and keeps their codes (window.CMM.value). A change made there reaches the DLL with the next status reads, until
    // the DLL reports it. Only changes: the first codes the menu gives can come from before the DLL moved an old slider
    // choice over, and the DLL has the saved ones already.
    var seen = null, want = {};
    function menuCode(key) {
        var v;
        try { if (window.CMM && typeof window.CMM.value === 'function') v = window.CMM.value(MOD_ID, key); } catch (e) { v = undefined; }
        return typeof v === 'number' && isFinite(v) ? Math.round(v) : null;
    }
    function menuParams() {
        var k = menuCode(KEY_OPTION), p = menuCode(PAD_OPTION), p2 = menuCode(PAD2_OPTION), q = '';
        if (!seen) {
            if (k === null && p === null && p2 === null) return '';
            seen = { key: k, pad: p, pad2: p2 };
        }
        if (k !== null && k !== seen.key) want.key = seen.key = k;
        if (p !== null && p !== seen.pad) want.pad = seen.pad = p;
        if (p2 !== null && p2 !== seen.pad2) want.pad2 = seen.pad2 = p2;
        if (want.key !== undefined) q += '&pk=' + want.key;
        if (want.pad !== undefined) q += '&pp=' + want.pad;
        if (want.pad2 !== undefined) q += '&pp2=' + want.pad2;
        return q;
    }

    // ---------------------------------------------------------------- the DLL
    // The HUD's combat flag (as RadarPlus reads it), sent with each status read from the view that has it: the
    // DLL switches to the combat camera while it's set (and a moment after).
    function combatParam() {
        var m = window.hud_sonar_in_combat;
        if (!m || m.value === undefined) return '';
        return '&c=' + (m.value ? 1 : 0);
    }

    // One status read at a time; the next is timed from the answer (fast while the panel is open).
    function poll() {
        var done = false;
        function next(open) {
            if (done) return;
            done = true;
            setTimeout(poll, open ? OPEN_MS : CLOSED_MS);
        }
        try {
            var x = new XMLHttpRequest();
            x.open('GET', URL + '?a=status' + combatParam() + menuParams() + '&n=' + Date.now(), true);
            x.onreadystatechange = function () {
                if (x.readyState !== 4) return;
                var s = null;
                try {
                    s = JSON.parse(x.responseText);
                    if (s.panelKey === want.key) delete want.key;
                    if (s.panelPad === want.pad) delete want.pad;
                    if (s.panelPad2 === want.pad2) delete want.pad2;
                    render(s);
                } catch (e) { log('render: ' + e); }
                next(!!(s && s.open));
            };
            x.send();
        } catch (e) {
            log('poll: ' + e);
            next(false);
        }
    }

    log('CameraPlus ' + (C.version || '?') + ' script started (hook ' + C.hook + ')');
    poll();
})();
