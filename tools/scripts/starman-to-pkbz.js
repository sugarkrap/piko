#!/usr/bin/env node
'use strict';

const fs = require('fs');
const path = require('path');
const zlib = require('zlib');
const img = require('./piko-image');

const MASTER_W = 960;
const MASTER_H = 720;
const SHADER_TAGS = ['ADV', 'SMOOTH-ADV', 'LCD-GRID', 'GDV', 'Guest'];

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

const bake = (zip, preset, w, h) => {
    const params = {};
    const images = {};
    collect(zip, preset, params, images);
    if (!images.DeviceImage || !zip.has(images.DeviceImage)) return null;

    const device = img.decodeImage(zip.read(images.DeviceImage));

    let scale = num(params, 'HSM_NON_INTEGER_SCALE', 100) / 100;
    scale *= num(params, 'HSM_NON_INTEGER_SCALE_OFFSET', 100) / 100;
    const screenH = scale * h;
    const screenW = num(params, 'HSM_ASPECT_RATIO_EXPLICIT', 4 / 3) * screenH;
    const screenCx = w / 2;
    const screenCy = h / 2;

    const deviceH = screenH * num(params, 'HSM_DEVICE_SCALE', 100) / 100;
    const deviceW = (device.width / device.height) * deviceH;
    const deviceCy = screenCy - (num(params, 'HSM_DEVICE_POS_Y', 0) / 100) * screenH;
    const deviceCx = screenCx + (num(params, 'HSM_DEVICE_POS_X', 0) / 100) * screenW;

    const out = img.blank(w, h);
    if (images.BackgroundImage && zip.has(images.BackgroundImage)) {
        const bg = img.cover(img.decodeImage(zip.read(images.BackgroundImage)), w, h);
        bg.data.copy(out.data);
    }

    const boxW = Math.max(1, Math.round(deviceW));
    const boxH = Math.max(1, Math.round(deviceH));
    const originX = Math.round(deviceCx - boxW / 2);
    const originY = Math.round(deviceCy - boxH / 2);
    img.blit(out, img.resample(device, boxW, boxH), originX, originY);

    if (images.DecalImage && zip.has(images.DecalImage)) {
        const decal = img.decodeImage(zip.read(images.DecalImage));
        if (decal.width === device.width && decal.height === device.height) {
            img.blit(out, img.resample(decal, boxW, boxH), originX, originY);
        }
    }

    const rect = {
        x: Math.round(screenCx - screenW / 2),
        y: Math.round(screenCy - screenH / 2),
        w: Math.max(1, Math.round(screenW)),
        h: Math.max(1, Math.round(screenH)),
    };
    for (let y = rect.y; y < rect.y + rect.h; y++) {
        if (y < 0 || y >= h) continue;
        for (let x = rect.x; x < rect.x + rect.w; x++) {
            if (x < 0 || x >= w) continue;
            const o = (y * w + x) * 4;
            out.data[o] = 0; out.data[o + 1] = 0; out.data[o + 2] = 0;
        }
    }
    return { image: out, rect };
};

const toPkbz = (image, rect, source) => {
    const src = Buffer.from(source, 'utf8');
    const head = Buffer.alloc(36);
    head.write('PKBZ', 0, 'ascii');
    head.writeUInt32LE(1, 4);
    head.writeUInt32LE(image.width, 8);
    head.writeUInt32LE(image.height, 12);
    head.writeUInt32LE(rect.x >>> 0, 16);
    head.writeUInt32LE(rect.y >>> 0, 20);
    head.writeUInt32LE(rect.w, 24);
    head.writeUInt32LE(rect.h, 28);
    head.writeUInt32LE(src.length, 32);
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

const packRoot = (zip) => {
    for (const name of zip.names()) {
        const i = name.indexOf('/Presets/');
        if (i >= 0) return name.slice(0, i);
    }
    return null;
};

const main = () => {
    const argv = process.argv.slice(2);
    if (argv.length < 2) {
        console.error('Usage: starman-to-pkbz.js <pack.zip> <outdir> [WxH]');
        process.exit(2);
    }
    const [src, outdir] = argv;
    let width = MASTER_W;
    let height = MASTER_H;
    if (argv[2]) {
        const m = argv[2].toLowerCase().split('x');
        width = parseInt(m[0], 10);
        height = parseInt(m[1], 10);
    }

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
    const seen = new Map();
    let written = 0, skipped = 0, duplicate = 0;

    for (const preset of presets) {
        const rel = preset.slice(root.length + 1);
        const name = bezelName(preset);
        const baked = bake(zip, preset, width, height);
        if (!baked) {
            skipped++;
            console.log(`    skip ${rel} (procedural bezel, no device image)`);
            continue;
        }
        const key = zlib.crc32(baked.image.data).toString(16) + ':' + baked.image.data.length;
        if (seen.has(key)) {
            duplicate++;
            console.log(`    skip ${rel} (identical to ${seen.get(key)})`);
            continue;
        }
        seen.set(key, name);
        fs.writeFileSync(path.join(outdir, `${name}.pkbz`),
                         toPkbz(baked.image, baked.rect, rel));
        written++;
        console.log(`    ${name}  ${width}x${height} hole ${baked.rect.w}x${baked.rect.h}`);
    }

    console.log(`==> ${written} bezel(s) in ${outdir}, ${skipped} procedural, ${duplicate} duplicate`);
    process.exit(written ? 0 : 1);
};

main();
