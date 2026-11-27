/*
 * iHeart Radio Station Manager - logo engine
 *
 * Turns any station logo into a Cyberpunk 2077 radio icon: one flat white shape on transparent (the game tints it),
 * like base\gameplay\gui\common\icons\radiostations_icons. Pure functions on RGBA arrays, so the same code runs in
 * the app (canvas) and in tests (node).
 *
 *   analyze(rgba, w, h)            background removal + colour clusters
 *   candidates(an, opts)           several ways to draw the logo as one colour, best first
 *   fit(ink, w, h, size)           crop + centre into a size x size alpha map (the icon)
 */
(function (root, factory) {
  if (typeof module === "object" && module.exports) module.exports = factory();
  else root.LogoCore = factory();
})(typeof self !== "undefined" ? self : this, function () {
  "use strict";

  const lumOf = (r, g, b) => (0.2126 * r + 0.7152 * g + 0.0722 * b) / 255;
  const dist2 = (a, i, b, j) => {
    const dr = a[i] - b[j], dg = a[i + 1] - b[j + 1], db = a[i + 2] - b[j + 2];
    return dr * dr + dg * dg + db * db;
  };
  const smooth = (e0, e1, x) => {
    const t = Math.min(1, Math.max(0, (x - e0) / (e1 - e0)));
    return t * t * (3 - 2 * t);
  };

  // ------------------------------------------------------------------ background
  // Transparent logos: the alpha channel. Opaque logos (a white / coloured / gradient square): grown from the image
  // border through small colour steps, so smooth gradients go but the logo's hard edges stop it. The same colour
  // inside the logo, not connected to the border, stays.
  function foreground(rgba, w, h) {
    const n = w * h, a = new Float32Array(n);
    let transparent = 0;
    for (let i = 0; i < n; i++) { a[i] = rgba[i * 4 + 3] / 255; if (a[i] < 0.1) transparent++; }
    if (transparent / n > 0.04) return { alpha: a, bgRemoved: "alpha" };

    const STEP = 14 * 14 * 3;          // max colour change between two neighbouring background pixels
    const DRIFT = 90 * 90 * 3;         // max distance from the border colour it started from
    const bg = new Uint8Array(n), origin = new Int32Array(n).fill(-1), queue = new Int32Array(n);
    let qh = 0, qt = 0;
    const seed = (p) => { if (!bg[p]) { bg[p] = 1; origin[p] = p; queue[qt++] = p; } };
    for (let x = 0; x < w; x++) { seed(x); seed((h - 1) * w + x); }
    for (let y = 0; y < h; y++) { seed(y * w); seed(y * w + w - 1); }
    while (qh < qt) {
      const p = queue[qh++], x = p % w, y = (p / w) | 0, o = origin[p];
      const visit = (q) => {
        if (bg[q]) return;
        if (dist2(rgba, q * 4, rgba, p * 4) > STEP) return;
        if (dist2(rgba, q * 4, rgba, o * 4) > DRIFT) return;
        bg[q] = 1; origin[q] = o; queue[qt++] = q;
      };
      if (x > 0) visit(p - 1); if (x < w - 1) visit(p + 1); if (y > 0) visit(p - w); if (y < h - 1) visit(p + w);
    }
    if (qt / n < 0.05) return { alpha: a, bgRemoved: "none" };   // the logo fills the whole picture
    // soft edge: pixels next to the background fade by how close their colour is to it
    for (let p = 0; p < n; p++) {
      if (bg[p]) { a[p] = 0; continue; }
      const x = p % w, y = (p / w) | 0;
      let near = -1;
      if (x > 0 && bg[p - 1]) near = p - 1; else if (x < w - 1 && bg[p + 1]) near = p + 1;
      else if (y > 0 && bg[p - w]) near = p - w; else if (y < h - 1 && bg[p + w]) near = p + w;
      if (near >= 0) a[p] = Math.min(a[p], Math.min(1, Math.sqrt(dist2(rgba, p * 4, rgba, near * 4) / (3 * 70 * 70))));
    }
    return { alpha: a, bgRemoved: "color" };
  }

  // ------------------------------------------------------------------ colour clusters (k-means, merged when close)
  function clusters(rgba, alpha, w, h, kMax) {
    const n = w * h, pts = [];
    for (let i = 0; i < n; i += 3) if (alpha[i] > 0.6) pts.push(i);
    if (pts.length === 0) return { centers: [], labels: new Int8Array(n).fill(-1) };
    // init: spread over luminance
    pts.sort((p, q) => lumOf(rgba[p * 4], rgba[p * 4 + 1], rgba[p * 4 + 2]) - lumOf(rgba[q * 4], rgba[q * 4 + 1], rgba[q * 4 + 2]));
    let k = Math.min(kMax, pts.length);
    let centers = [];
    for (let c = 0; c < k; c++) {
      const p = pts[Math.min(pts.length - 1, Math.floor((c + 0.5) * pts.length / k))];
      centers.push([rgba[p * 4], rgba[p * 4 + 1], rgba[p * 4 + 2]]);
    }
    // a saturated colour that luminance spreading may miss: add the most saturated point as a seed
    let satP = -1, satV = 0;
    for (const p of pts) {
      const r = rgba[p * 4], g = rgba[p * 4 + 1], b = rgba[p * 4 + 2];
      const mx = Math.max(r, g, b), mn = Math.min(r, g, b), s = mx - mn;
      if (s > satV) { satV = s; satP = p; }
    }
    if (satP >= 0 && satV > 80) centers[Math.floor(k / 2)] = [rgba[satP * 4], rgba[satP * 4 + 1], rgba[satP * 4 + 2]];
    for (let it = 0; it < 12; it++) {
      const sum = centers.map(() => [0, 0, 0, 0]);
      for (const p of pts) {
        let bi = 0, bd = Infinity;
        for (let c = 0; c < centers.length; c++) { const d = dist2(rgba, p * 4, centers[c], 0); if (d < bd) { bd = d; bi = c; } }
        const s = sum[bi]; s[0] += rgba[p * 4]; s[1] += rgba[p * 4 + 1]; s[2] += rgba[p * 4 + 2]; s[3]++;
      }
      centers = sum.filter((s) => s[3] > 0).map((s) => [s[0] / s[3], s[1] / s[3], s[2] / s[3]]);
    }
    // merge near-identical clusters
    const merged = [];
    for (const c of centers) {
      if (!merged.some((m) => dist2(m, 0, c, 0) < 55 * 55 * 3)) merged.push(c);
    }
    centers = merged;
    const labels = new Int8Array(n).fill(-1), share = new Float32Array(centers.length);
    let fg = 0;
    for (let i = 0; i < n; i++) {
      if (alpha[i] < 0.35) continue;
      let bi = 0, bd = Infinity;
      for (let c = 0; c < centers.length; c++) { const d = dist2(rgba, i * 4, centers[c], 0); if (d < bd) { bd = d; bi = c; } }
      labels[i] = bi; share[bi]++; fg++;
    }
    for (let c = 0; c < share.length; c++) share[c] /= Math.max(1, fg);
    return { centers, labels, share };
  }

  function analyze(rgba, w, h) {
    const fg = foreground(rgba, w, h);
    const cl = clusters(rgba, fg.alpha, w, h, 5);
    const lum = new Float32Array(w * h);
    for (let i = 0; i < w * h; i++) lum[i] = lumOf(rgba[i * 4], rgba[i * 4 + 1], rgba[i * 4 + 2]);
    return { w, h, rgba, alpha: fg.alpha, bgRemoved: fg.bgRemoved, lum, ...cl };
  }

  // ------------------------------------------------------------------ helpers on masks
  function dilate(mask, w, h, r) {      // max filter (square), separable
    if (r <= 0) return mask;
    const tmp = new Uint8Array(w * h), out = new Uint8Array(w * h);
    for (let y = 0; y < h; y++) {
      let run = -1e9;
      for (let x = 0; x < w; x++) { if (mask[y * w + x]) run = x; if (x - run <= r) tmp[y * w + x] = 1; }
      run = 1e9;
      for (let x = w - 1; x >= 0; x--) { if (mask[y * w + x]) run = x; if (run - x <= r) tmp[y * w + x] = 1; }
    }
    for (let x = 0; x < w; x++) {
      let run = -1e9;
      for (let y = 0; y < h; y++) { if (tmp[y * w + x]) run = y; if (y - run <= r) out[y * w + x] = 1; }
      run = 1e9;
      for (let y = h - 1; y >= 0; y--) { if (tmp[y * w + x]) run = y; if (run - y <= r) out[y * w + x] = 1; }
    }
    return out;
  }

  // thin cuts where two kept colour regions touch, so shapes stay readable in one colour
  function cutLines(an, keep, gap) {
    const { w, h, labels } = an;
    if (gap <= 0) return null;
    const edge = new Uint8Array(w * h);
    for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) {
      const i = y * w + x, l = labels[i];
      if (l < 0 || !keep[l]) continue;
      const r = x + 1 < w ? labels[i + 1] : -1, d = y + 1 < h ? labels[i + w] : -1;
      if (r >= 0 && r !== l && keep[r]) edge[i] = 1;
      if (d >= 0 && d !== l && keep[d]) edge[i] = 1;
    }
    return dilate(edge, w, h, Math.max(1, Math.round(gap / 2)));
  }

  function otsu(an) {
    const hist = new Float64Array(64);
    let tot = 0;
    for (let i = 0; i < an.w * an.h; i++) if (an.alpha[i] > 0.5) { hist[Math.min(63, Math.floor(an.lum[i] * 64))]++; tot++; }
    let sum = 0; for (let i = 0; i < 64; i++) sum += i * hist[i];
    let sumB = 0, wB = 0, best = 0, t = 32;
    for (let i = 0; i < 64; i++) {
      wB += hist[i]; if (!wB) continue;
      const wF = tot - wB; if (!wF) break;
      sumB += i * hist[i];
      const mB = sumB / wB, mF = (sum - sumB) / wF, v = wB * wF * (mB - mF) * (mB - mF);
      if (v > best) { best = v; t = i; }
    }
    return (t + 0.5) / 64;
  }

  // ------------------------------------------------------------------ candidates
  // Each candidate = one way to draw the logo in a single colour. Scored so the best guess comes first.
  function candidates(an, opts) {
    opts = opts || {};
    const gap = opts.gap === undefined ? Math.max(2, Math.round(an.w / 170)) : opts.gap;
    const { w, h, alpha, labels, centers, share, lum } = an;
    const n = w * h, out = [];
    const nC = centers.length;

    const fromKeep = (keep, key, label) => {
      const ink = new Float32Array(n);
      const cut = cutLines(an, keep, gap);
      for (let i = 0; i < n; i++) {
        const l = labels[i];
        if (l >= 0 && !keep[l]) continue;
        if (cut && cut[i]) continue;
        ink[i] = alpha[i];
      }
      out.push({ key, label, ink });
    };

    // 1. whole logo, colours separated by thin cuts
    fromKeep(new Array(nC).fill(true), "solid", "Full shape");
    // 2. one colour knocked out (e.g. white lettering cut out of a coloured badge, or a dark box removed)
    for (let c = 0; c < nC; c++) {
      if (share[c] < 0.03 || nC < 2) continue;
      const keep = new Array(nC).fill(true); keep[c] = false;
      const L = lumOf(centers[c][0], centers[c][1], centers[c][2]);
      const what = L > 0.75 ? "light" : L < 0.25 ? "dark" : "colour";
      fromKeep(keep, "knock" + c, `Without ${what} parts`);
    }
    // 3. by brightness
    const t = otsu(an);
    const dark = new Float32Array(n), light = new Float32Array(n);
    for (let i = 0; i < n; i++) {
      dark[i] = alpha[i] * smooth(t + 0.08, t - 0.08, lum[i]);
      light[i] = alpha[i] * smooth(t - 0.08, t + 0.08, lum[i]);
    }
    out.push({ key: "dark", label: "Dark lines", ink: dark });
    out.push({ key: "light", label: "Light lines", ink: light });

    // clean up, drop near-duplicates, score, sort
    let fgCount = 0;
    for (let i = 0; i < n; i++) if (alpha[i] > 0.5) fgCount++;
    const uniq = [];
    for (const c of out) {
      despeckle(c.ink, w, h);
      c.stats = stats(c.ink, w, h);
      if (c.stats.cov < 0.01) continue;
      if (uniq.some((u) => similar(u.ink, c.ink))) continue;
      uniq.push(c);
    }
    for (const c of uniq) {
      c.kept = enclosure(an, c.ink, fgCount);
      c.score = score(c.stats, c.key) * (0.25 + 0.75 * c.kept);
    }
    uniq.sort((p, q) => q.score - p.score);
    return uniq;
  }

  // 4-connected components of ink > 0.5: per-pixel label, areas, boxes
  function components(ink, w, h) {
    const n = w * h, lab = new Int32Array(n).fill(-1), areas = [], boxes = [], q = new Int32Array(n);
    for (let s = 0; s < n; s++) {
      if (ink[s] <= 0.5 || lab[s] >= 0) continue;
      const id = areas.length; let qh = 0, qt = 0, area = 0, x0 = w, y0 = h, x1 = 0, y1 = 0;
      lab[s] = id; q[qt++] = s;
      while (qh < qt) {
        const p = q[qh++], x = p % w, y = (p / w) | 0; area++;
        if (x < x0) x0 = x; if (x > x1) x1 = x; if (y < y0) y0 = y; if (y > y1) y1 = y;
        const nb = [x > 0 ? p - 1 : -1, x < w - 1 ? p + 1 : -1, y > 0 ? p - w : -1, y < h - 1 ? p + w : -1];
        for (const r of nb) if (r >= 0 && lab[r] < 0 && ink[r] > 0.5) { lab[r] = id; q[qt++] = r; }
      }
      areas.push(area); boxes.push([x0, y0, x1, y1]);
    }
    return { lab, areas, boxes };
  }

  // remove tiny marks away from the logo (watermark dots, stray pixels)
  function despeckle(ink, w, h) {
    const { lab, areas, boxes } = components(ink, w, h);
    const total = areas.reduce((s, a) => s + a, 0);
    if (!total) return;
    // the logo's main body: its big pieces (or the biggest one)
    const big = Math.min(total * 0.05, Math.max(...areas));
    let X0 = w, Y0 = h, X1 = -1, Y1 = -1;
    areas.forEach((a, i) => { if (a >= big) { const b = boxes[i]; X0 = Math.min(X0, b[0]); Y0 = Math.min(Y0, b[1]); X1 = Math.max(X1, b[2]); Y1 = Math.max(Y1, b[3]); } });
    if (X1 < 0) return;
    const pad = Math.round(Math.max(w, h) * 0.04);
    X0 -= pad; Y0 -= pad; X1 += pad; Y1 += pad;
    const edge = Math.round(Math.max(w, h) * 0.03);
    const drop = areas.map((a, i) => {
      const b = boxes[i];
      const outside = b[2] < X0 || b[0] > X1 || b[3] < Y0 || b[1] > Y1;
      // a small piece in a corner of the picture: the image service's watermark, not the logo
      const corner = (b[2] < edge * 2 || b[0] > w - edge * 2) && (b[3] < edge * 2 || b[1] > h - edge * 2);
      return a < total * 0.0005 || (a < total * 0.03 && (outside || corner));
    });
    for (let i = 0; i < w * h; i++) {
      if (lab[i] >= 0 && drop[lab[i]]) ink[i] = 0;
    }
    // soft edge pixels (<= 0.5) touching a dropped piece go too
    for (let i = 0; i < w * h; i++) {
      if (ink[i] > 0 && ink[i] <= 0.5) {
        const x = i % w, y = (i / w) | 0;
        const nb = [x > 0 ? i - 1 : -1, x < w - 1 ? i + 1 : -1, y > 0 ? i - w : -1, y < h - 1 ? i + w : -1];
        if (nb.some((r) => r >= 0 && lab[r] >= 0 && drop[lab[r]])) ink[i] = 0;
      }
    }
  }

  // mask with its holes filled: everything the outside can't reach without crossing the mask
  function filled(mask, w, h) {
    const n = w * h, out = new Uint8Array(n).fill(1), q = new Int32Array(n);
    let qh = 0, qt = 0;
    const seed = (p) => { if (out[p] && !mask[p]) { out[p] = 0; q[qt++] = p; } };
    for (let x = 0; x < w; x++) { seed(x); seed((h - 1) * w + x); }
    for (let y = 0; y < h; y++) { seed(y * w); seed(y * w + w - 1); }
    while (qh < qt) {
      const p = q[qh++], x = p % w, y = (p / w) | 0;
      if (x > 0) seed(p - 1); if (x < w - 1) seed(p + 1); if (y > 0) seed(p - w); if (y < h - 1) seed(p + w);
    }
    return out;
  }

  // 1 = what was left out of the logo is a box around the rest, or sits inside the rest (cut-out lettering, a
  // badge's fill): the logo still reads. 0 = whole letters went missing.
  function enclosure(an, ink, fgCount) {
    const { w, h, alpha } = an, n = w * h;
    const kept = new Uint8Array(n), removed = new Uint8Array(n);
    let nk = 0, nr = 0;
    for (let i = 0; i < n; i++) {
      if (ink[i] > 0.5) { kept[i] = 1; nk++; } else if (alpha[i] > 0.5) { removed[i] = 1; nr++; }
    }
    if (nr < fgCount * 0.04 || nk === 0) return 1;
    const fr = filled(removed, w, h), fk = filled(kept, w, h);
    let a = 0, b = 0;
    for (let i = 0; i < n; i++) { if (kept[i] && fr[i]) a++; if (removed[i] && fk[i]) b++; }
    return Math.max(a / nk, b / nr);
  }

  function similar(a, b) {
    let inter = 0, uni = 0;
    for (let i = 0; i < a.length; i += 7) { const x = a[i] > 0.5, y = b[i] > 0.5; if (x && y) inter++; if (x || y) uni++; }
    return uni === 0 || inter / uni > 0.96;
  }

  // coverage inside the ink's bounding box, amount of outline (detail), and whether shapes have holes / pieces
  function stats(ink, w, h) {
    let x0 = w, y0 = h, x1 = -1, y1 = -1, on = 0, edges = 0;
    for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) {
      const v = ink[y * w + x] > 0.5;
      if (!v) continue;
      on++;
      if (x < x0) x0 = x; if (x > x1) x1 = x; if (y < y0) y0 = y; if (y > y1) y1 = y;
      if (x === 0 || x === w - 1 || y === 0 || y === h - 1 ||
          ink[y * w + x - 1] <= 0.5 || ink[y * w + x + 1] <= 0.5 || ink[(y - 1) * w + x] <= 0.5 || ink[(y + 1) * w + x] <= 0.5) edges++;
    }
    const box = Math.max(1, (x1 - x0 + 1) * (y1 - y0 + 1));
    return { cov: on / box, detail: edges / Math.sqrt(Math.max(1, on)), on, box: [x0, y0, x1, y1] };
  }

  function score(s, key) {
    // good icons: 20-45 % filled, lots of outline (lettering) but not dust
    const c = s.cov;
    const covScore = c < 0.08 ? c / 0.08 * 0.3 : c < 0.2 ? 0.3 + (c - 0.08) / 0.12 * 0.7 : c <= 0.45 ? 1 : c < 0.85 ? 1 - (c - 0.45) / 0.4 * 0.7 : 0.3;
    const detail = Math.min(s.detail, 40) / 40;
    const bias = key === "solid" ? 0.95 : 1;      // prefer a knocked-out / line version when they're as good
    return covScore * (0.35 + detail) * bias;
  }

  // ------------------------------------------------------------------ final icon
  // crop to the ink, centre in a square with a small margin (like the game's atlas parts); returns Uint8Array alpha
  function fit(ink, w, h, size, opts) {
    opts = opts || {};
    const invert = !!opts.invert;
    const src = new Float32Array(w * h);
    for (let i = 0; i < w * h; i++) src[i] = invert ? 0 : ink[i];
    if (invert) {   // inverted: fill the logo's bounding shape and cut the ink out of it
      const st = stats(ink, w, h);
      const [x0, y0, x1, y1] = st.box;
      for (let y = y0; y <= y1; y++) for (let x = x0; x <= x1; x++) src[y * w + x] = 1 - ink[y * w + x];
    }
    const st = stats(src, w, h);
    const out = new Uint8Array(size * size);
    if (st.on === 0) return out;
    let [x0, y0, x1, y1] = st.box;
    const bw = x1 - x0 + 1, bh = y1 - y0 + 1;
    const margin = Math.round(size * 0.03);
    const scale = Math.min((size - 2 * margin) / bw, (size - 2 * margin) / bh);
    const ow = bw * scale, oh = bh * scale;
    const ox = (size - ow) / 2, oy = (size - oh) / 2;
    // area-averaged resampling (box filter), good for both up and down scaling
    for (let y = 0; y < size; y++) {
      const sy0 = (y - oy) / scale + y0, sy1 = (y + 1 - oy) / scale + y0;
      if (sy1 <= y0 || sy0 >= y1 + 1) continue;
      for (let x = 0; x < size; x++) {
        const sx0 = (x - ox) / scale + x0, sx1 = (x + 1 - ox) / scale + x0;
        if (sx1 <= x0 || sx0 >= x1 + 1) continue;
        let acc = 0, wsum = 0;
        for (let sy = Math.floor(sy0); sy < Math.ceil(sy1); sy++) {
          if (sy < 0 || sy >= h) continue;
          const wy = Math.min(sy + 1, sy1) - Math.max(sy, sy0);
          for (let sx = Math.floor(sx0); sx < Math.ceil(sx1); sx++) {
            if (sx < 0 || sx >= w) continue;
            const wx = Math.min(sx + 1, sx1) - Math.max(sx, sx0);
            acc += src[sy * w + sx] * wx * wy; wsum += wx * wy;
          }
        }
        if (wsum > 0) out[y * size + x] = Math.round(255 * Math.min(1, acc / wsum));
      }
    }
    return out;
  }

  return { analyze, candidates, fit, stats };
});
