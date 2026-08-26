#!/usr/bin/env node
'use strict';

const fs = require('fs');
const path = require('path');
const opentype = require('opentype.js');

const RANGES = [[0x20, 0x7e], [0xa0, 0xff]];
const ATLAS_W = 256;
const SS = 4;

const parseArgs = (argv) => {
    const args = { src: null, size: 16, out: null };
    const rest = [];
    for (let i = 0; i < argv.length; i++) {
        if (argv[i] === '--size') args.size = parseInt(argv[++i], 10);
        else if (argv[i].startsWith('--')) {
            console.error(`font-to-pkfn: unknown argument ${argv[i]}`);
            process.exit(1);
        } else rest.push(argv[i]);
    }
    if (rest.length !== 2) {
        console.error('Usage: font-to-pkfn.js [--size N] <font.ttf> <out.pkfn>');
        process.exit(2);
    }
    args.src = rest[0];
    args.out = rest[1];
    return args;
};

const flatten = (path2d, scale) => {
    const contours = [];
    let cur = null;
    let x = 0, y = 0;
    const steps = 8;
    const push = (px, py) => cur.push([px * scale, py * scale]);
    for (const cmd of path2d.commands) {
        switch (cmd.type) {
            case 'M':
                if (cur && cur.length) contours.push(cur);
                cur = [];
                x = cmd.x; y = cmd.y;
                push(x, y);
                break;
            case 'L':
                x = cmd.x; y = cmd.y;
                push(x, y);
                break;
            case 'Q':
                for (let i = 1; i <= steps; i++) {
                    const t = i / steps, u = 1 - t;
                    push(u * u * x + 2 * u * t * cmd.x1 + t * t * cmd.x,
                         u * u * y + 2 * u * t * cmd.y1 + t * t * cmd.y);
                }
                x = cmd.x; y = cmd.y;
                break;
            case 'C':
                for (let i = 1; i <= steps; i++) {
                    const t = i / steps, u = 1 - t;
                    push(u * u * u * x + 3 * u * u * t * cmd.x1 + 3 * u * t * t * cmd.x2 + t * t * t * cmd.x,
                         u * u * u * y + 3 * u * u * t * cmd.y1 + 3 * u * t * t * cmd.y2 + t * t * t * cmd.y);
                }
                x = cmd.x; y = cmd.y;
                break;
            case 'Z':
                if (cur && cur.length) contours.push(cur);
                cur = null;
                break;
            default:
                break;
        }
    }
    if (cur && cur.length) contours.push(cur);
    return contours;
};

const rasterise = (contours, ox, oy, w, h) => {
    const cov = Buffer.alloc(w * h, 0);
    if (!contours.length) return cov;
    const acc = new Uint16Array(w);
    const xs = [];
    for (let py = 0; py < h; py++) {
        acc.fill(0);
        for (let s = 0; s < SS; s++) {
            const sy = oy + py + (s + 0.5) / SS;
            xs.length = 0;
            for (const c of contours) {
                for (let i = 0; i < c.length; i++) {
                    const a = c[i], b = c[(i + 1) % c.length];
                    const y0 = a[1], y1 = b[1];
                    if ((y0 <= sy && y1 > sy) || (y1 <= sy && y0 > sy)) {
                        const t = (sy - y0) / (y1 - y0);
                        xs.push([a[0] + t * (b[0] - a[0]), y1 > y0 ? 1 : -1]);
                    }
                }
            }
            if (!xs.length) continue;
            xs.sort((p, q) => p[0] - q[0]);
            let wind = 0, spanStart = 0;
            for (const [cx, dir] of xs) {
                if (wind === 0) spanStart = cx;
                wind += dir;
                if (wind !== 0) continue;
                for (let sx = 0; sx < SS * w; sx++) {
                    const px = ox + sx / SS + 0.5 / SS;
                    if (px >= spanStart && px < cx) acc[(sx / SS) | 0] += 1;
                }
            }
        }
        const full = SS * SS;
        for (let px = 0; px < w; px++) {
            const v = acc[px] > full ? full : acc[px];
            cov[py * w + px] = Math.round((v * 255) / full);
        }
    }
    return cov;
};

const main = () => {
    const args = parseArgs(process.argv.slice(2));
    const font = opentype.loadSync(args.src);
    const scale = args.size / font.unitsPerEm;
    const ascent = Math.ceil(font.ascender * scale);
    const descent = Math.ceil(-font.descender * scale);

    const glyphs = [];
    for (const [first, last] of RANGES) {
        for (let cp = first; cp <= last; cp++) {
            const glyph = font.charToGlyph(String.fromCodePoint(cp));
            const advance = Math.round((glyph.advanceWidth || 0) * scale);
            const contours = flatten(glyph.getPath(0, 0, args.size), 1);
            let minX = Infinity, minY = Infinity, maxX = -Infinity, maxY = -Infinity;
            for (const c of contours) for (const [px, py] of c) {
                if (px < minX) minX = px;
                if (px > maxX) maxX = px;
                if (py < minY) minY = py;
                if (py > maxY) maxY = py;
            }
            if (!contours.length || !isFinite(minX)) {
                glyphs.push({ cp, w: 0, h: 0, bx: 0, by: 0, advance, cov: Buffer.alloc(0) });
                continue;
            }
            const ox = Math.floor(minX), oy = Math.floor(minY);
            const w = Math.ceil(maxX) - ox + 1;
            const h = Math.ceil(maxY) - oy + 1;
            glyphs.push({ cp, w, h, bx: ox, by: oy, advance,
                          cov: rasterise(contours, ox, oy, w, h) });
        }
    }

    let ax = 0, ay = 0, rowH = 0;
    for (const g of glyphs) {
        if (g.w && ax + g.w > ATLAS_W) { ay += rowH + 1; ax = 0; rowH = 0; }
        g.ax = ax; g.ay = ay;
        if (g.w) { ax += g.w + 1; if (g.h > rowH) rowH = g.h; }
    }
    const atlasH = Math.max(1, ay + rowH);
    const atlas = Buffer.alloc(ATLAS_W * atlasH, 0);
    for (const g of glyphs) {
        for (let r = 0; r < g.h; r++) {
            g.cov.copy(atlas, (g.ay + r) * ATLAS_W + g.ax, r * g.w, (r + 1) * g.w);
        }
    }

    const head = Buffer.alloc(22);
    head.write('PKFN', 0, 'ascii');
    head.writeUInt32LE(1, 4);
    head.writeUInt16LE(args.size, 8);
    head.writeUInt16LE(ascent + descent, 10);
    head.writeUInt16LE(ascent, 12);
    head.writeUInt16LE(descent, 14);
    head.writeUInt16LE(ATLAS_W, 16);
    head.writeUInt16LE(atlasH, 18);
    head.writeUInt16LE(glyphs.length, 20);

    const table = Buffer.alloc(glyphs.length * 18);
    glyphs.forEach((g, i) => {
        const o = i * 18;
        table.writeUInt32LE(g.cp, o);
        table.writeUInt16LE(g.ax, o + 4);
        table.writeUInt16LE(g.ay, o + 6);
        table.writeUInt16LE(g.w, o + 8);
        table.writeUInt16LE(g.h, o + 10);
        table.writeInt16LE(g.bx, o + 12);
        table.writeInt16LE(g.by, o + 14);
        table.writeInt16LE(g.advance, o + 16);
    });

    fs.writeFileSync(args.out, Buffer.concat([head, table, atlas]));
    console.log(`==> ${path.basename(args.out)}: ${glyphs.length} glyphs, `
                + `atlas ${ATLAS_W}x${atlasH}, line ${ascent + descent}, `
                + `${head.length + table.length + atlas.length} bytes`);
};

main();
