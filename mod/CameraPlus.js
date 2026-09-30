// CameraPlus: the tuning panel. cameraplus.dll owns the panel (rows, values, keys, the controller); this script
// draws it on the left of the screen from the DLL's status, and redraws when the status's serial changes. It also
// passes the HUD's combat flag to the DLL, and on the MODS page shows the panel key's and button's names next to
// their sliders and tells the DLL when they move.
(function () {
    'use strict';
    if (window.__CameraPlusInstalled) return;
    window.__CameraPlusInstalled = true;

    var C = window.__CameraPlusConfig || {};
    var DIAG = !!C.diagnostics;
    var URL = 'coui://base/__cameraplus__.json';
    var LOG_URL = 'coui://base/__cameraplus_log__.json';
    var MOD_ID = 'cameraplus', KEY_OPTION = 'panel_key', BUTTON_OPTION = 'panel_button';
    var KEYS = C.keys || [];  // the Panel key slider's key names, by position (0 = Off)
    var PAD = C.pad || [];    // the Panel button slider's button names, by position (0 = Off)
    var OPEN_MS = 50, CLOSED_MS = 150;  // how often the status is read, with the panel open / closed
    var YELLOW = '#fbe732';             // the game's objective yellow (RadarPlus uses it too)
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
        '.cp-hidden{display:none !important}';

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
    function model(k, d) { var m = window[k]; return m && m.value !== undefined ? m.value : d; }
    function hex(s) { var o = ''; for (var i = 0; i < s.length; i++) o += ('0' + s.charCodeAt(i).toString(16)).slice(-2); return o; }

    // ---------------------------------------------------------------- the panel
    var panel = null, lastSerial = -1;

    function ensure() {
        if (panel && connected(panel)) return panel;
        if (!document.body) return null;
        if (!document.getElementById('cp-style')) {
            var st = el('style');
            st.id = 'cp-style';
            st.textContent = CSS;
            (document.head || document.body).appendChild(st);
        }
        panel = el('div', 'cp-panel cp-hidden');
        document.body.appendChild(panel);
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
        if (s.pad) {
            p.appendChild(el('div', 'cp-hint', 'D-pad: choose and change · hold LB: finer'));
            p.appendChild(el('div', 'cp-hint', 'A: select · X: game value · B: close'));
        } else {
            p.appendChild(el('div', 'cp-hint', 'Up / Down: choose · Left / Right: change · Shift: finer'));
            p.appendChild(el('div', 'cp-hint', 'Delete: game value · ' + (s.key || 'F1') + ' / Esc: close'));
        }
    }

    function render(s) {
        var p = ensure();
        if (!p) return;
        if (!s.open) {
            if (p.className.indexOf('cp-hidden') < 0) p.className = 'cp-panel cp-hidden';
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
    }

    // ---------------------------------------------------------------- the MODS page
    // The sliders' positions live in Mod Settings Menu (window.CMM); without it, the DLL's.
    var sent = { key: typeof C.panelKey === 'number' ? C.panelKey : 2, button: typeof C.panelButton === 'number' ? C.panelButton : 15 };
    function menuPosition(key, count, fallback) {
        var v;
        try { if (window.CMM && typeof window.CMM.value === 'function') v = window.CMM.value(MOD_ID, key); } catch (e) { v = undefined; }
        if (typeof v === 'number' && isFinite(v) && Math.round(v) >= 0 && Math.round(v) < count) return Math.round(v);
        return fallback;
    }
    function keyIndex() { return menuPosition(KEY_OPTION, KEYS.length, sent.key); }
    function buttonIndex() { return menuPosition(BUTTON_OPTION, PAD.length, sent.button); }

    // Mod Settings Menu shows a slider's position as a number; next to ours we show the key or button's name
    // instead, in a copy of the number's element that follows its classes (focus and section colours). Mod Settings
    // Menu names an option cmm_<hex of the mod id>_<hex of the option id>.
    function binding(option) { return 'cmm_' + hex(MOD_ID) + '_' + hex(option); }
    var sliders = [
        { binding: binding(KEY_OPTION), name: function () { var i = keyIndex(); return i > 0 && KEYS[i] ? KEYS[i] : 'Off'; } },
        { binding: binding(BUTTON_OPTION), name: function () { var i = buttonIndex(); return i > 0 && PAD[i] ? PAD[i] : 'Off'; } }
    ];
    sliders.forEach(function (s) { s.src = null; s.el = null; });
    var sliderSearch = 0;
    function sliderFor(node) {
        var at = node.attributes;
        for (var j = 0; at && j < at.length; j++) {
            var v = at[j] && at[j].value;
            if (typeof v !== 'string' || v.indexOf('cmm_') === -1) continue;
            for (var k = 0; k < sliders.length; k++) if (v.indexOf(sliders[k].binding + '_') !== -1) return sliders[k];
        }
        return null;
    }
    function tickSliders() {
        var open = model('ui_stacks_menu_options_active', false) && model('ui_stacks_menu_options_states_mods_active', false);
        if (!open) return;
        var t = Date.now(), missing = false;
        sliders.forEach(function (s) {
            if (s.src && !connected(s.src)) s.src = s.el = null;
            missing = missing || !s.src;
        });
        if (missing && t - sliderSearch >= 300) {
            sliderSearch = t;
            var values = document.querySelectorAll('.options-slider__value');
            for (var i = 0; i < values.length; i++) {
                var s = sliderFor(values[i]);
                if (!s || s.src || !values[i].parentNode) continue;
                // A page copy taken while ours showed has an old name in it: out, and the number back.
                var old = values[i].parentNode.querySelectorAll('.cp-key');
                for (var j = 0; j < old.length; j++) remove(old[j]);
                s.src = values[i];
                s.el = el('div');
                values[i].parentNode.insertBefore(s.el, values[i].nextSibling);
            }
        }
        sliders.forEach(function (s) {
            if (!s.src) return;
            var cls = s.src.className + ' cp-key';
            if (s.el.className !== cls) s.el.className = cls;
            var name = s.name();
            if (s.el.textContent !== name) s.el.textContent = name;
            if (s.src.style.display !== 'none') s.src.style.display = 'none';
        });
    }
    // The sliders' positions when they differ from what the DLL has.
    function menuParams() {
        var k = keyIndex(), b = buttonIndex(), q = '';
        if (k !== sent.key) q += '&k=' + k;
        if (b !== sent.button) q += '&b=' + b;
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
            tickSliders();
            var x = new XMLHttpRequest();
            x.open('GET', URL + '?a=status' + combatParam() + menuParams() + '&n=' + Date.now(), true);
            x.onreadystatechange = function () {
                if (x.readyState !== 4) return;
                var s = null;
                try {
                    s = JSON.parse(x.responseText);
                    if (typeof s.panelKey === 'number') sent.key = s.panelKey;
                    if (typeof s.panelButton === 'number') sent.button = s.panelButton;
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
