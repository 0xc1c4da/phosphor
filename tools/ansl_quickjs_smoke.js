// Run inside the app's embedded QuickJS (where globalThis.ANSL is registered).
// This file is for reference / manual eval (e.g. paste into editor or JS_Eval in a harness).

const { map } = ANSL.modules.num;
const { sdBox } = ANSL.modules.sdf;
const v2 = ANSL.modules.vec2;

if (ANSL.version !== "1.1") throw new Error("bad version: " + ANSL.version);
if (map(0.5, 0, 1, 10, 20) !== 15) throw new Error("bad map");

const p = { x: 1, y: 2 };
const q = { x: 4, y: 6 };
const r = v2.sub(q, p);
if (r.x !== 3 || r.y !== 4) throw new Error("bad vec2.sub");

// NOTE: ANSL's sdBox implementation (as in ansl/src/modules/sdf.js) returns 0 inside the box,
// not a negative signed distance.
const d0 = sdBox({ x: 0, y: 0 }, { x: 1, y: 1 });
if (d0 !== 0) throw new Error("bad sdf.sdBox (inside should be 0): " + d0);

const d1 = sdBox({ x: 2, y: 0 }, { x: 1, y: 1 });
if (!(d1 > 0)) throw new Error("bad sdf.sdBox (outside should be >0): " + d1);

print("OK: ansl quickjs smoke passed");



