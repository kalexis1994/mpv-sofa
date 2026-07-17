// focus.js — Spatial D-pad focus engine for TV navigation.
//
// Any visible element with the `.focusable` class participates. Arrow keys
// move a virtual focus ring (the `.nav-focused` class) to the geometrically
// nearest focusable in that direction — no per-screen index bookkeeping.
//
// Interactive controls (input / select) support an "edit mode": pressing OK
// on them gives real DOM focus (so the webOS virtual keyboard appears for
// text fields, and Left/Right adjust sliders / change selects natively).
// While editing, the key router hands every key to the native control except
// Back / OK, which exit edit mode.
//
// Overlays (e.g. the main menu) push a scope so only their items are
// reachable; popping restores the previous scope and its last focus.
class FocusEngine {
    constructor() {
        this.scopes = [];               // stack of scope root elements
        this.current = null;            // element wearing .nav-focused
        this.editing = null;            // element in edit mode, or null
        this.lastFocus = new WeakMap(); // scope element -> last focused child
    }

    get scope() {
        return this.scopes[this.scopes.length - 1] || document.body;
    }

    // Replace the whole scope stack (used when switching top-level screens).
    setScope(rootEl, preferSelector) {
        if (this.current) this.lastFocus.set(this.scope, this.current);
        this.exitEdit();
        this._clear();
        this.scopes = [rootEl];
        this.refresh(preferSelector);
    }

    // Layer a new scope on top (used when an overlay opens).
    pushScope(rootEl, preferSelector) {
        if (this.current) this.lastFocus.set(this.scope, this.current);
        this.exitEdit();
        this._clear();
        this.scopes.push(rootEl);
        this.refresh(preferSelector);
    }

    // Remove the top scope and restore focus underneath.
    popScope() {
        this.exitEdit();
        this._clear();
        if (this.scopes.length > 1) this.scopes.pop();
        this.refresh();
    }

    scopeIs(rootEl) {
        return this.scope === rootEl;
    }

    _visible(el) {
        if (!el || !el.isConnected) return false;
        const r = el.getBoundingClientRect();
        if (r.width === 0 && r.height === 0) return false;
        for (let n = el; n && n !== document.body; n = n.parentElement) {
            const s = getComputedStyle(n);
            if (s.display === 'none' || s.visibility === 'hidden') return false;
        }
        return true;
    }

    candidates() {
        return Array.from(this.scope.querySelectorAll('.focusable')).filter(el =>
            this._visible(el) &&
            !el.classList.contains('disabled') &&
            !el.hasAttribute('disabled') &&
            el.getAttribute('aria-disabled') !== 'true');
    }

    // Re-scan candidates and (re)establish a sensible focus.
    refresh(preferSelector) {
        const cands = this.candidates();
        if (!cands.length) { this._clear(); return; }
        if (this.current && cands.includes(this.current)) {
            this._setFocus(this.current);
            return;
        }
        let target = null;
        if (preferSelector) {
            const q = this.scope.querySelector(preferSelector);
            if (q && cands.includes(q)) target = q;
        }
        if (!target) {
            const restore = this.lastFocus.get(this.scope);
            if (restore && cands.includes(restore)) target = restore;
        }
        this._setFocus(target || cands[0]);
    }

    _clear() {
        if (this.current) this.current.classList.remove('nav-focused');
        this.current = null;
    }

    _setFocus(el) {
        if (!el) return;
        if (this.current && this.current !== el) {
            this.current.classList.remove('nav-focused');
        }
        this.current = el;
        el.classList.add('nav-focused');
        this.lastFocus.set(this.scope, el);
        try { el.scrollIntoView({ block: 'nearest', inline: 'nearest' }); } catch (e) {}
    }

    move(dir) {
        if (this.editing) return false;
        const cands = this.candidates();
        if (!cands.length) return false;
        if (!this.current || !cands.includes(this.current)) {
            this._setFocus(cands[0]);
            return true;
        }
        const next = this._nearest(this.current, cands, dir);
        if (next) { this._setFocus(next); return true; }
        return false;
    }

    // Geometric nearest-neighbour in a direction.
    //
    // Candidates whose perpendicular projection OVERLAPS the current element
    // (i.e. they sit in the same column when moving up/down, or the same row
    // when moving left/right) are preferred and ranked purely by the gap in
    // the travel direction — so a vertically-stacked control that happens to
    // be indented is never skipped for a better-aligned one further away.
    // Only when nothing overlaps do we fall back to a distance-plus-offset
    // score for diagonal reaches.
    _nearest(current, cands, dir) {
        const cur = current.getBoundingClientRect();
        const horiz = (dir === 'left' || dir === 'right');
        const ov = (a0, a1, b0, b1) => Math.max(0, Math.min(a1, b1) - Math.max(a0, b0));
        const cc = { x: cur.left + cur.width / 2, y: cur.top + cur.height / 2 };

        let overlapBest = null, overlapGap = Infinity;
        let anyBest = null, anyScore = Infinity;

        for (const el of cands) {
            if (el === current) continue;
            const r = el.getBoundingClientRect();
            let gap, overlap;
            if (dir === 'down')       { if (r.top    < cur.bottom - 2) continue; gap = r.top - cur.bottom;   overlap = ov(cur.left, cur.right, r.left, r.right); }
            else if (dir === 'up')    { if (r.bottom > cur.top + 2)    continue; gap = cur.top - r.bottom;   overlap = ov(cur.left, cur.right, r.left, r.right); }
            else if (dir === 'right') { if (r.left   < cur.right - 2)  continue; gap = r.left - cur.right;   overlap = ov(cur.top, cur.bottom, r.top, r.bottom); }
            else                      { if (r.right  > cur.left + 2)   continue; gap = cur.left - r.right;   overlap = ov(cur.top, cur.bottom, r.top, r.bottom); }
            gap = Math.max(0, gap);

            if (overlap > 0) {
                if (gap < overlapGap) { overlapGap = gap; overlapBest = el; }
            }
            const rc = { x: r.left + r.width / 2, y: r.top + r.height / 2 };
            const perp = horiz ? Math.abs(rc.y - cc.y) : Math.abs(rc.x - cc.x);
            const score = gap + perp * 2;
            if (score < anyScore) { anyScore = score; anyBest = el; }
        }
        return overlapBest || anyBest;
    }

    // OK/Enter: edit interactive controls, click everything else.
    activate() {
        const el = this.current;
        if (!el) return;
        if (this.editing) { this.exitEdit(); return; }
        if (el.tagName === 'INPUT' || el.tagName === 'SELECT') {
            this.enterEdit(el);
        } else {
            el.click();
        }
    }

    enterEdit(el) {
        this.editing = el;
        el.classList.add('editing');
        try { el.focus({ preventScroll: true }); } catch (e) { el.focus(); }
    }

    exitEdit() {
        if (!this.editing) return;
        const el = this.editing;
        el.classList.remove('editing');
        try { el.blur(); } catch (e) {}
        this.editing = null;
    }
}
