#!/usr/bin/env node
'use strict';

const fs = require('fs');
const zlib = require('zlib');

function parseArgs(argv) {
    const args = {
        src: 'modules/initramfs/splash-src.png',
        out: 'modules/initramfs/splash.ppm.gz',
        width: 640,
        height: 480,
        logoHeight: 360,
        colors: 32,
        budget: 40000,
        rawOut: 'rootfs/usr/share/piko/splash.raw',
    };
    for (let i = 0; i < argv.length; i++) {
        const val = () => argv[++i];
        switch (argv[i]) {
            case '--src': args.src = val(); break;
            case '--out': args.out = val(); break;
            case '--width': args.width = parseInt(val(), 10); break;
            case '--height': args.height = parseInt(val(), 10); break;
            case '--logo-height': args.logoHeight = parseInt(val(), 10); break;
            case '--colors': args.colors = parseInt(val(), 10); break;
            case '--budget': args.budget = parseInt(val(), 10); break;
            case '--raw-out': args.rawOut = val(); break;
            default:
                console.error(`make-splash: unknown argument ${argv[i]}`);
                process.exit(1);
        }
    }
    return args;
}

function paeth(a, b, c) {
    const p = a + b - c;
    const pa = Math.abs(p - a);
    const pb = Math.abs(p - b);
    const pc = Math.abs(p - c);
    if (pa <= pb && pa <= pc) return a;
    if (pb <= pc) return b;
    return c;
}

function decodePNG(buf) {
    const sig = Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]);
    if (!buf.subarray(0, 8).equals(sig)) {
        throw new Error('not a PNG file');
    }

    let offset = 8;
    let ihdr = null;
    let palette = null;
    const idatChunks = [];

    while (offset < buf.length) {
        const length = buf.readUInt32BE(offset);
        const type = buf.toString('ascii', offset + 4, offset + 8);
        const data = buf.subarray(offset + 8, offset + 8 + length);
        offset += 8 + length + 4;

        if (type === 'IHDR') {
            ihdr = {
                width: data.readUInt32BE(0),
                height: data.readUInt32BE(4),
                bitDepth: data.readUInt8(8),
                colorType: data.readUInt8(9),
                interlace: data.readUInt8(12),
            };
        } else if (type === 'PLTE') {
            palette = data;
        } else if (type === 'IDAT') {
            idatChunks.push(data);
        } else if (type === 'IEND') {
            break;
        }
    }

    if (!ihdr) throw new Error('missing IHDR');
    if (ihdr.bitDepth !== 8) throw new Error(`unsupported PNG bit depth ${ihdr.bitDepth} (only 8 supported)`);
    if (ihdr.interlace !== 0) throw new Error('interlaced PNGs are not supported');

    const channelsByType = { 0: 1, 2: 3, 3: 1, 4: 2, 6: 4 };
    const channels = channelsByType[ihdr.colorType];
    if (!channels) throw new Error(`unsupported PNG color type ${ihdr.colorType}`);

    const raw = zlib.inflateSync(Buffer.concat(idatChunks));
    const { width, height } = ihdr;
    const stride = width * channels;

    const unfiltered = Buffer.alloc(height * stride);
    let rawOff = 0;
    for (let y = 0; y < height; y++) {
        const filterType = raw[rawOff++];
        const rowStart = y * stride;
        const prevRowStart = (y - 1) * stride;
        for (let x = 0; x < stride; x++) {
            const val = raw[rawOff++];
            const a = x >= channels ? unfiltered[rowStart + x - channels] : 0;
            const b = y > 0 ? unfiltered[prevRowStart + x] : 0;
            const c = y > 0 && x >= channels ? unfiltered[prevRowStart + x - channels] : 0;
            let out;
            switch (filterType) {
                case 0: out = val; break;
                case 1: out = val + a; break;
                case 2: out = val + b; break;
                case 3: out = val + ((a + b) >> 1); break;
                case 4: out = val + paeth(a, b, c); break;
                default: throw new Error(`unsupported PNG filter type ${filterType}`);
            }
            unfiltered[rowStart + x] = out & 0xff;
        }
    }

    const rgba = Buffer.alloc(width * height * 4);
    for (let i = 0, px = 0; px < width * height; px++, i += channels) {
        let r, g, b, a = 255;
        if (ihdr.colorType === 0) {
            r = g = b = unfiltered[i];
        } else if (ihdr.colorType === 2) {
            r = unfiltered[i]; g = unfiltered[i + 1]; b = unfiltered[i + 2];
        } else if (ihdr.colorType === 3) {
            const idx = unfiltered[i];
            r = palette[idx * 3]; g = palette[idx * 3 + 1]; b = palette[idx * 3 + 2];
        } else if (ihdr.colorType === 4) {
            r = g = b = unfiltered[i]; a = unfiltered[i + 1];
        } else {
            r = unfiltered[i]; g = unfiltered[i + 1]; b = unfiltered[i + 2]; a = unfiltered[i + 3];
        }
        rgba[px * 4] = r; rgba[px * 4 + 1] = g; rgba[px * 4 + 2] = b; rgba[px * 4 + 3] = a;
    }

    return { width, height, rgba };
}

function resizeBilinear(src, srcW, srcH, dstW, dstH) {
    const dst = Buffer.alloc(dstW * dstH * 4);
    const xRatio = srcW / dstW;
    const yRatio = srcH / dstH;

    for (let dy = 0; dy < dstH; dy++) {
        const sy = Math.min(srcH - 1, (dy + 0.5) * yRatio - 0.5);
        const sy0 = Math.max(0, Math.floor(sy));
        const sy1 = Math.min(srcH - 1, sy0 + 1);
        const fy = sy - sy0;

        for (let dx = 0; dx < dstW; dx++) {
            const sx = Math.min(srcW - 1, (dx + 0.5) * xRatio - 0.5);
            const sx0 = Math.max(0, Math.floor(sx));
            const sx1 = Math.min(srcW - 1, sx0 + 1);
            const fx = sx - sx0;

            const p00 = (sy0 * srcW + sx0) * 4;
            const p01 = (sy0 * srcW + sx1) * 4;
            const p10 = (sy1 * srcW + sx0) * 4;
            const p11 = (sy1 * srcW + sx1) * 4;

            const di = (dy * dstW + dx) * 4;
            for (let c = 0; c < 4; c++) {
                const top = src[p00 + c] * (1 - fx) + src[p01 + c] * fx;
                const bottom = src[p10 + c] * (1 - fx) + src[p11 + c] * fx;
                dst[di + c] = Math.round(top * (1 - fy) + bottom * fy);
            }
        }
    }
    return dst;
}

function compositeOnWhite(rgba, w, h) {
    const rgb = Buffer.alloc(w * h * 3);
    for (let px = 0; px < w * h; px++) {
        const a = rgba[px * 4 + 3] / 255;
        for (let c = 0; c < 3; c++) {
            const src = rgba[px * 4 + c];
            rgb[px * 3 + c] = Math.round(src * a + 255 * (1 - a));
        }
    }
    return rgb;
}

function medianCutQuantize(rgb, w, h, maxColors) {
    const histogram = new Map();
    for (let px = 0; px < w * h; px++) {
        const key = (rgb[px * 3] << 16) | (rgb[px * 3 + 1] << 8) | rgb[px * 3 + 2];
        histogram.set(key, (histogram.get(key) || 0) + 1);
    }

    let entries = [...histogram.entries()].map(([key, count]) => ({
        r: (key >> 16) & 0xff, g: (key >> 8) & 0xff, b: key & 0xff, count,
    }));

    if (entries.length <= maxColors) {
        return entries.map(({ r, g, b }) => [r, g, b]);
    }

    function boxRange(box) {
        let rMin = 255, rMax = 0, gMin = 255, gMax = 0, bMin = 255, bMax = 0;
        for (const e of box) {
            if (e.r < rMin) rMin = e.r; if (e.r > rMax) rMax = e.r;
            if (e.g < gMin) gMin = e.g; if (e.g > gMax) gMax = e.g;
            if (e.b < bMin) bMin = e.b; if (e.b > bMax) bMax = e.b;
        }
        const rRange = rMax - rMin, gRange = gMax - gMin, bRange = bMax - bMin;
        if (rRange >= gRange && rRange >= bRange) return { channel: 'r', range: rRange };
        if (gRange >= bRange) return { channel: 'g', range: gRange };
        return { channel: 'b', range: bRange };
    }

    const boxes = [entries];
    while (boxes.length < maxColors) {
        let splitIdx = -1, splitRange = -1, splitChannel = null;
        for (let i = 0; i < boxes.length; i++) {
            if (boxes[i].length < 2) continue;
            const { channel, range } = boxRange(boxes[i]);
            if (range > splitRange) { splitRange = range; splitIdx = i; splitChannel = channel; }
        }
        if (splitIdx === -1) break;

        const box = boxes[splitIdx];
        box.sort((a, b) => a[splitChannel] - b[splitChannel]);
        const totalCount = box.reduce((s, e) => s + e.count, 0);
        let acc = 0, mid = box.length >> 1;
        for (let i = 0; i < box.length; i++) {
            acc += box[i].count;
            if (acc >= totalCount / 2) { mid = Math.max(1, i); break; }
        }
        boxes.splice(splitIdx, 1, box.slice(0, mid), box.slice(mid));
    }

    return boxes.map((box) => {
        let r = 0, g = 0, b = 0, count = 0;
        for (const e of box) { r += e.r * e.count; g += e.g * e.count; b += e.b * e.count; count += e.count; }
        return [Math.round(r / count), Math.round(g / count), Math.round(b / count)];
    });
}

function applyPalette(rgb, w, h, palette) {
    const out = Buffer.alloc(w * h * 3);
    for (let px = 0; px < w * h; px++) {
        const r = rgb[px * 3], g = rgb[px * 3 + 1], b = rgb[px * 3 + 2];
        let best = 0, bestDist = Infinity;
        for (let i = 0; i < palette.length; i++) {
            const [pr, pg, pb] = palette[i];
            const dist = (r - pr) ** 2 + (g - pg) ** 2 + (b - pb) ** 2;
            if (dist < bestDist) { bestDist = dist; best = i; }
        }
        out[px * 3] = palette[best][0];
        out[px * 3 + 1] = palette[best][1];
        out[px * 3 + 2] = palette[best][2];
    }
    return out;
}

function writePPM(rgb, w, h) {
    const header = Buffer.from(`P6\n${w} ${h}\n255\n`, 'ascii');
    return Buffer.concat([header, rgb]);
}

function gzipReproducible(data) {
    const deflated = zlib.deflateRawSync(data, { level: 9 });
    const header = Buffer.from([0x1f, 0x8b, 0x08, 0x00, 0, 0, 0, 0, 0x00, 0xff]);
    const crc = zlib.crc32(data);
    const footer = Buffer.alloc(8);
    footer.writeUInt32LE(crc >>> 0, 0);
    footer.writeUInt32LE(data.length >>> 0, 4);
    return Buffer.concat([header, deflated, footer]);
}

function main() {
    const args = parseArgs(process.argv.slice(2));

    const png = decodePNG(fs.readFileSync(args.src));

    let logoHeight = Math.min(args.logoHeight, args.height);
    let logoWidth = Math.max(1, Math.round((png.width * logoHeight) / png.height));
    if (logoWidth > args.width) {
        logoWidth = args.width;
        logoHeight = Math.max(1, Math.round((png.height * logoWidth) / png.width));
    }

    const resized = resizeBilinear(png.rgba, png.width, png.height, logoWidth, logoHeight);

    const canvasRGBA = Buffer.alloc(args.width * args.height * 4);
    for (let i = 0; i < canvasRGBA.length; i += 4) {
        canvasRGBA[i] = 255; canvasRGBA[i + 1] = 255; canvasRGBA[i + 2] = 255; canvasRGBA[i + 3] = 255;
    }
    const pasteX = Math.floor((args.width - logoWidth) / 2);
    const pasteY = Math.floor((args.height - logoHeight) / 2);
    for (let y = 0; y < logoHeight; y++) {
        for (let x = 0; x < logoWidth; x++) {
            const srcI = (y * logoWidth + x) * 4;
            const dstI = ((pasteY + y) * args.width + (pasteX + x)) * 4;
            canvasRGBA[dstI] = resized[srcI];
            canvasRGBA[dstI + 1] = resized[srcI + 1];
            canvasRGBA[dstI + 2] = resized[srcI + 2];
            canvasRGBA[dstI + 3] = resized[srcI + 3];
        }
    }

    let canvasRGB = compositeOnWhite(canvasRGBA, args.width, args.height);
    if (args.colors) {
        const palette = medianCutQuantize(canvasRGB, args.width, args.height, args.colors);
        canvasRGB = applyPalette(canvasRGB, args.width, args.height, palette);
    }

    const raw = writePPM(canvasRGB, args.width, args.height);
    const packed = gzipReproducible(raw);
    fs.writeFileSync(args.out, packed);

    console.log(`${args.out}: ${args.width}x${args.height} screen, logo ${logoWidth}x${logoHeight}, ${args.colors || 'true'}-color`);
    console.log(`  uncompressed PPM : ${raw.length} bytes`);
    console.log(`  gzipped (flash)  : ${packed.length} bytes (budget ${args.budget})`);

    if (args.rawOut) {
        const rgb565 = Buffer.alloc(args.width * args.height * 2);
        for (let px = 0, i = 0; px < args.width * args.height; px++, i += 2) {
            const r = canvasRGB[px * 3], g = canvasRGB[px * 3 + 1], b = canvasRGB[px * 3 + 2];
            const v = ((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3);
            rgb565[i] = v & 0xff;
            rgb565[i + 1] = (v >> 8) & 0xff;
        }
        fs.writeFileSync(args.rawOut, rgb565);
        console.log(`${args.rawOut}: raw RGB565 ${rgb565.length} bytes (stage 2, cat straight to /dev/fb0)`);
    }

    if (packed.length > args.budget) {
        console.error(`make-splash: asset is ${packed.length} bytes compressed, over the ${args.budget}-byte budget -- lower --logo-height or --colors.`);
        process.exit(1);
    }
}

main();
