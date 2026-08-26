#!/usr/bin/env node
'use strict';

const fs = require('fs');
const path = require('path');
const img = require('./piko-image');

const NAME_LEN = 16;
const PIECE_LEN = 40;
const FLAG_ALPHA = 1;

const lerp = (a, b, t) => a + (b - a) * t;

const clamp8 = (v) => (v < 0 ? 0 : v > 255 ? 255 : Math.round(v));

const gradient = (w, h, top, bottom, opts = {}) => {
    const gloss = opts.gloss === undefined ? 0.30 : opts.gloss;
    const topEdge = opts.topEdge === undefined ? 40 : opts.topEdge;
    const bottomEdge = opts.bottomEdge === undefined ? 30 : opts.bottomEdge;
    const px = Buffer.alloc(w * h * 3);

    for (let y = 0; y < h; y++) {
        const t = h > 1 ? y / (h - 1) : 0;
        let r = lerp(top[0], bottom[0], t);
        let g = lerp(top[1], bottom[1], t);
        let b = lerp(top[2], bottom[2], t);
        if (gloss > 0 && y < h / 2) {
            const a = gloss * (1 - y / Math.max(1, h / 2 - 1));
            r += (255 - r) * a;
            g += (255 - g) * a;
            b += (255 - b) * a;
        }
        if (y === 0) { r += topEdge; g += topEdge; b += topEdge; }
        if (y === h - 1) { r -= bottomEdge; g -= bottomEdge; b -= bottomEdge; }
        for (let x = 0; x < w; x++) {
            const o = (y * w + x) * 3;
            px[o] = clamp8(r); px[o + 1] = clamp8(g); px[o + 2] = clamp8(b);
        }
    }
    return { w, h, px };
};

const iconPiece = (name, file, size) => {
    let im = img.decodeImage(fs.readFileSync(file));
    if (size && (im.width !== size || im.height !== size))
        im = img.resample(im, size, size);
    const px = Buffer.alloc(im.width * im.height * 3);
    const alpha = Buffer.alloc(im.width * im.height);
    for (let i = 0; i < im.width * im.height; i++) {
        px[i * 3] = im.data[i * 4];
        px[i * 3 + 1] = im.data[i * 4 + 1];
        px[i * 3 + 2] = im.data[i * 4 + 2];
        alpha[i] = im.data[i * 4 + 3];
    }
    return { name, w: im.width, h: im.height, px, alpha, l: 0, t: 0, r: 0, b: 0 };
};

const rgb565 = (piece) => {
    const out = Buffer.alloc(piece.w * piece.h * 2);
    for (let i = 0, o = 0; i < piece.w * piece.h; i++, o += 2) {
        const p = i * 3;
        out.writeUInt16LE(((piece.px[p] & 0xf8) << 8)
                          | ((piece.px[p + 1] & 0xfc) << 3)
                          | (piece.px[p + 2] >> 3), o);
    }
    return out;
};

const encode = (pieces) => {
    const head = Buffer.alloc(12);
    head.write('PKUI', 0, 'ascii');
    head.writeUInt32LE(2, 4);
    head.writeUInt16LE(pieces.length, 8);
    head.writeUInt16LE(0, 10);

    const table = Buffer.alloc(pieces.length * PIECE_LEN);
    const blobs = [];
    let offset = 0;
    pieces.forEach((p, i) => {
        const o = i * PIECE_LEN;
        const bytes = p.alpha ? Buffer.concat([rgb565(p), p.alpha]) : rgb565(p);
        table.write(p.name.slice(0, NAME_LEN - 1), o, 'ascii');
        table.writeUInt16LE(p.w, o + 16);
        table.writeUInt16LE(p.h, o + 18);
        table.writeUInt16LE(p.l, o + 20);
        table.writeUInt16LE(p.t, o + 22);
        table.writeUInt16LE(p.r, o + 24);
        table.writeUInt16LE(p.b, o + 26);
        table.writeUInt16LE(p.alpha ? FLAG_ALPHA : 0, o + 28);
        table.writeUInt16LE(0, o + 30);
        table.writeUInt32LE(offset, o + 32);
        table.writeUInt32LE(bytes.length, o + 36);
        offset += bytes.length;
        blobs.push(bytes);
    });
    return Buffer.concat([head, table, ...blobs]);
};

const main = () => {
    const argv = process.argv.slice(2);
    const icons = [];
    const rest = [];
    for (let i = 0; i < argv.length; i++) {
        if (argv[i] === '--icon') {
            const spec = argv[++i];
            const eq = spec.indexOf('=');
            if (eq < 0) {
                console.error(`ui-bake: --icon wants name=path, got ${spec}`);
                process.exit(1);
            }
            icons.push({ name: spec.slice(0, eq), file: spec.slice(eq + 1) });
        } else rest.push(argv[i]);
    }
    if (rest.length !== 1) {
        console.error('Usage: ui-bake.js [--icon name=file.png ...] <out.pkui>');
        process.exit(2);
    }

    const bar = gradient(64, 56, [0x3c, 0x41, 0x49], [0x0b, 0x0d, 0x10],
                         { gloss: 0.10, topEdge: 26, bottomEdge: 6 });

    const pieces = [{ name: 'bar', ...bar, l: 16, t: 0, r: 16, b: 0 }];
    for (const ic of icons)
        pieces.push(iconPiece(ic.name, ic.file, 24));

    const blob = encode(pieces);
    fs.writeFileSync(rest[0], blob);
    console.log(`==> ${path.basename(rest[0])}: ${pieces.length} pieces, `
                + pieces.map((p) => `${p.name} ${p.w}x${p.h}${p.alpha ? '+a' : ''}`).join(', ')
                + `, ${blob.length} bytes`);
};

main();
