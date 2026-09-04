#!/usr/bin/env node
'use strict';

const fs = require('fs');
const path = require('path');
const zlib = require('zlib');
const img = require('./piko-image');

const SHIPPED_PREFIX = 'J2ME_';

const BACKGROUND_W = 1024;
const BACKGROUND_H = 600;
const SHADER_TAGS = ['ADV', 'SMOOTH-ADV', 'LCD-GRID', 'GDV', 'Guest'];

const CANVAS_BUCKETS = [
    { tag: '4x3', w: 320, h: 240 },
    { tag: '5x4', w: 220, h: 176 },
    { tag: '1x1', w: 240, h: 240 },
];

const readZip = (buf) => {
    const end = (() => {
        for (let i = buf.length - 22; i >= 0 && i > buf.length - 66000; i--) {
            if (buf.readUInt32LE(i) === 0x06054b50) return i;
        }
        throw new Error('not a zip file (no end-of-central-directory)');
    })();
    const count = buf.readUInt16LE(end + 10);
    let off = buf.readUInt32LE(end + 16);
    const entries = new Map();
    for (let i = 0; i < count; i++) {
        if (buf.readUInt32LE(off) !== 0x02014b50) throw new Error('bad central directory');
        const method = buf.readUInt16LE(off + 10);
        const nameLen = buf.readUInt16LE(off + 28);
        const extraLen = buf.readUInt16LE(off + 30);
        const commentLen = buf.readUInt16LE(off + 32);
        const local = buf.readUInt32LE(off + 42);
        const name = buf.toString('utf8', off + 46, off + 46 + nameLen);
        off += 46 + nameLen + extraLen + commentLen;
        if (name.endsWith('/')) continue;
        entries.set(name, { method, local });
    }
    return {
        names: () => [...entries.keys()],
        has: (name) => entries.has(name),
        read: (name) => {
            const e = entries.get(name);
            if (!e) throw new Error(`not in zip: ${name}`);
            const n = buf.readUInt16LE(e.local + 26);
            const x = buf.readUInt16LE(e.local + 28);
            const size = buf.readUInt32LE(e.local + 18);
            const start = e.local + 30 + n + x;
            const raw = buf.subarray(start, size ? start + size : undefined);
            return e.method === 0 ? Buffer.from(raw) : zlib.inflateRawSync(raw);
        },
    };
};

const normalise = (p) => {
    const parts = [];
    for (const seg of p.split('/')) {
        if (seg === '' || seg === '.') continue;
        if (seg === '..') parts.pop();
        else parts.push(seg);
    }
    return parts.join('/');
};

const collect = (zip, file, params, images, depth = 0) => {
    if (depth > 8 || !zip.has(file)) return;
    const here = path.posix.dirname(file);
    for (const raw of zip.read(file).toString('utf8').split('\n')) {
        const line = raw.trim();
        if (!line || line.startsWith('//')) continue;
        if (line.startsWith('#reference')) {
            const ref = line.slice('#reference'.length).trim().replace(/^"|"$/g, '');
            if (ref) collect(zip, normalise(`${here}/${ref}`), params, images, depth + 1);
            continue;
        }
        const eq = line.indexOf('=');
        if (eq < 0) continue;
        const key = line.slice(0, eq).trim();
        const value = line.slice(eq + 1).trim().replace(/^"|"$/g, '');
        if (key.endsWith('Image')) images[key] = normalise(`${here}/${value}`);
        else params[key] = value;
    }
};

const num = (params, key, fallback) => {
    const v = parseFloat(params[key]);
    return Number.isFinite(v) ? v : fallback;
};

const presetOf = (zip, preset) => {
    const params = {};
    const images = {};
    collect(zip, preset, params, images);
    return { params, images };
};

const pickBucket = (aspect) => {
    let best = null;
    let bestDelta = Infinity;
    for (const bucket of CANVAS_BUCKETS) {
        for (const [w, h] of [[bucket.w, bucket.h], [bucket.h, bucket.w]]) {
            const delta = Math.abs(Math.log((w / h) / aspect));
            if (delta < bestDelta) {
                bestDelta = delta;
                best = { tag: bucket.tag, w, h };
            }
        }
    }
    return best;
};

const KEYPAD_RUN_ROWS = 12;
const KEYPAD_STDDEV_MARGIN = 10;

const rowStddev = (image, box, y) => {
    let sum = 0, sumsq = 0, n = 0;
    for (let x = box.x; x < box.x + box.w; x++) {
        const o = (y * image.width + x) * 4;
        if (image.data[o + 3] < 128) continue;
        const lum = (image.data[o] * 299 + image.data[o + 1] * 587
            + image.data[o + 2] * 114) / 1000;
        sum += lum;
        sumsq += lum * lum;
        n++;
    }
    if (n === 0) return 0;
    const mean = sum / n;
    return Math.sqrt(Math.max(0, sumsq / n - mean * mean));
};

const keypadTop = (image, box) => {
    const rows = [];
    for (let y = box.y; y < box.y + box.h; y++)
        rows.push(rowStddev(image, box, y));

    const head = rows.slice(0, Math.max(1, Math.floor(rows.length / 4))).slice().sort((a, b) => a - b);
    const baseline = head[Math.floor(head.length / 2)];
    const threshold = baseline + KEYPAD_STDDEV_MARGIN;

    let run = 0;
    for (let i = 0; i < rows.length; i++) {
        if (rows[i] > threshold) {
            run++;
            if (run >= KEYPAD_RUN_ROWS) return box.y + i - run + 1;
        } else {
            run = 0;
        }
    }
    return box.y + box.h;
};

const unionBbox = (a, b) => {
    const x = Math.min(a.x, b.x);
    const y = Math.min(a.y, b.y);
    return { x, y, w: Math.max(a.x + a.w, b.x + b.w) - x, h: Math.max(a.y + a.h, b.y + b.h) - y };
};

const bakeBezel = (zip, preset, target, centreInGlass) => {
    const { params, images } = presetOf(zip, preset);
    if (!images.DeviceImage || !zip.has(images.DeviceImage)) return null;

    const device = img.decodeImage(zip.read(images.DeviceImage));
    const decal = images.DecalImage && zip.has(images.DecalImage)
        ? img.decodeImage(zip.read(images.DecalImage))
        : null;
    const usable = decal !== null
        && decal.width === device.width && decal.height === device.height;

    const screenW = target.w;
    const screenH = target.h;
    const deviceH = screenH * num(params, 'HSM_DEVICE_SCALE', 100) / 100;
    const deviceW = (device.width / device.height) * deviceH;
    const sf = deviceW / device.width;

    let box = img.alphaBbox(device);
    if (usable) box = unionBbox(box, img.alphaBbox(decal));

    const assetW = Math.max(1, Math.round(box.w * sf));
    const assetH = Math.max(1, Math.round(box.h * sf));
    const out = img.blank(assetW, assetH, 0, 0, 0, 0);
    img.blit(out, img.resample(img.crop(device, box), assetW, assetH), 0, 0);
    if (usable)
        img.blit(out, img.resample(img.crop(decal, box), assetW, assetH), 0, 0);

    const holeX = deviceW / 2 - screenW / 2 - (num(params, 'HSM_DEVICE_POS_X', 0) / 100) * screenW;
    let holeY = deviceH / 2 - screenH / 2 + (num(params, 'HSM_DEVICE_POS_Y', 0) / 100) * screenH;

    if (centreInGlass) {
        const glassBottom = keypadTop(device, box) * sf;
        const glassTop = box.y * sf;
        const centred = glassTop + (glassBottom - glassTop) / 2 - screenH / 2;
        if (centred > glassTop && centred + screenH < glassBottom + screenH)
            holeY = centred;
    }

    return {
        image: out,
        rect: {
            x: Math.round(holeX - box.x * sf),
            y: Math.round(holeY - box.y * sf),
            w: Math.max(1, Math.round(screenW)),
            h: Math.max(1, Math.round(screenH)),
        },
        offset: { x: Math.round(box.x * sf), y: Math.round(box.y * sf) },
        background: images.BackgroundImage && zip.has(images.BackgroundImage)
            ? images.BackgroundImage
            : null,
    };
};

const rotateCcw = (image, rect, offset) => {
    const w = image.width;
    const h = image.height;
    const out = img.blank(h, w, 0, 0, 0, 0);

    for (let y = 0; y < w; y++) {
        for (let x = 0; x < h; x++) {
            const from = (x * w + (w - 1 - y)) * 4;
            const to = (y * h + x) * 4;
            out.data[to] = image.data[from];
            out.data[to + 1] = image.data[from + 1];
            out.data[to + 2] = image.data[from + 2];
            out.data[to + 3] = image.data[from + 3];
        }
    }

    return {
        image: out,
        rect: { x: rect.y, y: w - rect.x - rect.w, w: rect.h, h: rect.w },
        offset: { x: offset.y, y: offset.x },
    };
};

const toPkbz = (baked, source) => {
    const src = Buffer.from(source, 'utf8');
    const head = Buffer.alloc(44);
    head.write('PKBZ', 0, 'ascii');
    head.writeUInt32LE(2, 4);
    head.writeUInt32LE(baked.image.width, 8);
    head.writeUInt32LE(baked.image.height, 12);
    head.writeUInt32LE(baked.rect.x >>> 0, 16);
    head.writeUInt32LE(baked.rect.y >>> 0, 20);
    head.writeUInt32LE(baked.rect.w, 24);
    head.writeUInt32LE(baked.rect.h, 28);
    head.writeUInt32LE(baked.offset.x >>> 0, 32);
    head.writeUInt32LE(baked.offset.y >>> 0, 36);
    head.writeUInt32LE(src.length, 40);
    return Buffer.concat([head, src, img.rgb565(baked.image), img.alpha8(baked.image)]);
};

const toPkbg = (image, source) => {
    const src = Buffer.from(source, 'utf8');
    const head = Buffer.alloc(20);
    head.write('PKBG', 0, 'ascii');
    head.writeUInt32LE(1, 4);
    head.writeUInt32LE(image.width, 8);
    head.writeUInt32LE(image.height, 12);
    head.writeUInt32LE(src.length, 16);
    return Buffer.concat([head, src, img.rgb565(image)]);
};

const bezelName = (preset) => {
    const stem = path.posix.basename(preset).replace(/\.slangp$/, '');
    const tags = [...stem.matchAll(/\[([^\]]*)\]/g)]
        .map((m) => m[1])
        .filter((t) => !SHADER_TAGS.includes(t));
    const base = stem.replace(/-?\[[^\]]*\]/g, '');
    return [base, ...tags].filter(Boolean).join('-').replace(/[^A-Za-z0-9._-]/g, '_');
};

const collapsedName = (name) => name.replace(/-\d{1,4}x\d{1,4}(?=-|$)/, '');

const backgroundName = (file) =>
    path.posix.basename(file).replace(/\.[^.]+$/, '').replace(/[^A-Za-z0-9._-]/g, '_');

const packRoot = (zip) => {
    for (const name of zip.names()) {
        const i = name.indexOf('/Presets/');
        if (i >= 0) return name.slice(0, i);
    }
    return null;
};

const main = () => {
    const argv = process.argv.slice(2);
    if (argv.length < 3) {
        console.error('Usage: starman-to-pkbz.js <pack.zip> <bezeldir> <backgrounddir>');
        process.exit(2);
    }
    const [src, outdir, bgdir] = argv;

    const zip = readZip(fs.readFileSync(src));
    const root = packRoot(zip);
    if (root === null) {
        console.error(`starman-to-pkbz: no Presets/ directory inside ${src}`);
        process.exit(1);
    }

    const presets = zip.names()
        .filter((n) => n.startsWith(`${root}/Presets/`) && n.endsWith('.slangp'))
        .sort((a, b) => {
            const la = bezelName(a).length;
            const lb = bezelName(b).length;
            return la !== lb ? la - lb : a.localeCompare(b);
        });

    fs.mkdirSync(outdir, { recursive: true });
    fs.mkdirSync(bgdir, { recursive: true });

    const seen = new Map();
    const written = new Map();
    const backgrounds = new Map();
    const defaults = [];
    let count = 0, skipped = 0, duplicate = 0;

    const plan = [];
    for (const preset of presets) {
        const rel = preset.slice(root.length + 1);
        const name = bezelName(preset);
        const { params, images } = presetOf(zip, preset);
        if (!images.DeviceImage || !zip.has(images.DeviceImage)) {
            skipped++;
            console.log(`    skip ${rel} (procedural bezel, no device image)`);
            continue;
        }
        if (!name.startsWith(SHIPPED_PREFIX)) {
            skipped++;
            console.log(`    skip ${rel} (${name} needs emulation this device cannot run)`);
            continue;
        }
        const aspect = num(params, 'HSM_ASPECT_RATIO_EXPLICIT', 4 / 3);
        const rotate = aspect <= 1;
        const bucket = pickBucket(rotate ? 1 / aspect : aspect);
        plan.push({ preset, rel, name, rotate, bucket, out: collapsedName(name) });
    }

    const widest = new Map();
    for (const p of plan) {
        const best = widest.get(p.out);
        if (best === undefined || p.bucket.w * p.bucket.h > best.bucket.w * best.bucket.h)
            widest.set(p.out, p);
    }

    for (const p of plan) {
        if (widest.get(p.out) !== p) {
            duplicate++;
            console.log(`    skip ${p.rel} (${p.out} framed from ${widest.get(p.out).rel})`);
            continue;
        }
        const { preset, rel, bucket, rotate } = p;
        const target = rotate ? { w: bucket.h, h: bucket.w } : { w: bucket.w, h: bucket.h };

        let baked = bakeBezel(zip, preset, target, 1);
        if (baked === null) {
            skipped++;
            console.log(`    skip ${rel} (procedural bezel, no device image)`);
            continue;
        }
        if (rotate) {
            const turned = rotateCcw(baked.image, baked.rect, baked.offset);
            baked = { ...baked, ...turned };
        }

        const key = zlib.crc32(baked.image.data).toString(16) + ':'
            + baked.image.data.length + ':' + (baked.background || '');
        if (seen.has(key)) {
            duplicate++;
            console.log(`    skip ${rel} (identical to ${seen.get(key)})`);
            continue;
        }

        const outName = p.out;
        if (written.has(outName)) {
            duplicate++;
            console.log(`    skip ${rel} (${outName} already written by ${written.get(outName)})`);
            continue;
        }
        seen.set(key, outName);
        written.set(outName, rel);

        fs.writeFileSync(path.join(outdir, `${outName}.pkbz`), toPkbz(baked, rel));
        count++;
        console.log(`    ${outName}  ${baked.image.width}x${baked.image.height}`
            + ` hole ${baked.rect.w}x${baked.rect.h} at ${baked.rect.x},${baked.rect.y}`);

        if (baked.background !== null) {
            const bg = backgroundName(baked.background);
            if (!backgrounds.has(bg)) {
                const cover = img.cover(img.decodeImage(zip.read(baked.background)),
                                        BACKGROUND_W, BACKGROUND_H);
                fs.writeFileSync(path.join(bgdir, `${bg}.pkbg`),
                                 toPkbg(cover, baked.background.slice(root.length + 1)));
                backgrounds.set(bg, true);
                console.log(`    background ${bg}  ${BACKGROUND_W}x${BACKGROUND_H}`);
            }
            defaults.push(`${outName}|${bg}`);
        }
    }

    fs.writeFileSync(path.join(outdir, 'defaults.cfg'), `${defaults.join('\n')}\n`);

    console.log(`==> ${count} bezel(s) in ${outdir}, ${backgrounds.size} background(s)`
        + ` in ${bgdir}, ${skipped} procedural, ${duplicate} duplicate`);
    process.exit(count ? 0 : 1);
};

main();
