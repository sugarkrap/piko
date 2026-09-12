'use strict';

const zlib = require('zlib');
const jpeg = require('jpeg-js');

const paeth = (a, b, c) => {
    const p = a + b - c;
    const pa = Math.abs(p - a);
    const pb = Math.abs(p - b);
    const pc = Math.abs(p - c);
    if (pa <= pb && pa <= pc) return a;
    if (pb <= pc) return b;
    return c;
};

const decodePNG = (buf) => {
    const sig = Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]);
    if (!buf.subarray(0, 8).equals(sig)) throw new Error('not a PNG file');

    let offset = 8;
    let ihdr = null;
    let palette = null;
    let trns = null;
    const idatChunks = [];

    while (offset < buf.length) {
        const length = buf.readUInt32BE(offset);
        const type = buf.toString('ascii', offset + 4, offset + 8);
        const data = buf.subarray(offset + 8, offset + 8 + length);
        offset += 12 + length;
        if (type === 'IHDR') {
            ihdr = {
                width: data.readUInt32BE(0),
                height: data.readUInt32BE(4),
                bitDepth: data[8],
                colorType: data[9],
                interlace: data[12],
            };
        } else if (type === 'PLTE') palette = Buffer.from(data);
        else if (type === 'tRNS') trns = Buffer.from(data);
        else if (type === 'IDAT') idatChunks.push(Buffer.from(data));
        else if (type === 'IEND') break;
    }

    if (!ihdr) throw new Error('missing IHDR');
    if (ihdr.bitDepth !== 8) throw new Error(`unsupported PNG bit depth ${ihdr.bitDepth}`);
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
        const prevRowStart = rowStart - stride;
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

    const data = Buffer.alloc(width * height * 4);
    for (let px = 0, i = 0; px < width * height; px++, i += channels) {
        let r, g, b, a = 255;
        switch (ihdr.colorType) {
            case 0: r = g = b = unfiltered[i]; break;
            case 2: r = unfiltered[i]; g = unfiltered[i + 1]; b = unfiltered[i + 2]; break;
            case 3: {
                const idx = unfiltered[i];
                r = palette[idx * 3]; g = palette[idx * 3 + 1]; b = palette[idx * 3 + 2];
                if (trns && idx < trns.length) a = trns[idx];
                break;
            }
            case 4: r = g = b = unfiltered[i]; a = unfiltered[i + 1]; break;
            default:
                r = unfiltered[i]; g = unfiltered[i + 1];
                b = unfiltered[i + 2]; a = unfiltered[i + 3];
        }
        const o = px * 4;
        data[o] = r; data[o + 1] = g; data[o + 2] = b; data[o + 3] = a;
    }
    return { width, height, data };
};

const decodeJPEG = (buf) => {
    const out = jpeg.decode(buf, { useTArray: true, formatAsRGBA: true });
    return { width: out.width, height: out.height, data: Buffer.from(out.data) };
};

const decodeImage = (buf) => {
    if (buf[0] === 0xff && buf[1] === 0xd8) return decodeJPEG(buf);
    return decodePNG(buf);
};

const lanczos = (x, a) => {
    if (x === 0) return 1;
    const ax = Math.abs(x);
    if (ax >= a) return 0;
    const px = Math.PI * x;
    return (a * Math.sin(px) * Math.sin(px / a)) / (px * px);
};

const weights = (srcN, dstN, a) => {
    const scale = dstN / srcN;
    const support = scale < 1 ? a / scale : a;
    const rows = [];
    for (let i = 0; i < dstN; i++) {
        const centre = (i + 0.5) / scale;
        let first = Math.floor(centre - support);
        let last = Math.ceil(centre + support);
        if (first < 0) first = 0;
        if (last > srcN) last = srcN;
        const w = [];
        let sum = 0;
        for (let j = first; j < last; j++) {
            const t = scale < 1 ? (j + 0.5 - centre) * scale : (j + 0.5 - centre);
            const v = lanczos(t, a);
            w.push(v);
            sum += v;
        }
        if (sum !== 0) for (let k = 0; k < w.length; k++) w[k] /= sum;
        rows.push({ first, w });
    }
    return rows;
};

const resample = (img, dstW, dstH, a = 3) => {
    if (img.width === dstW && img.height === dstH) return img;
    const cols = weights(img.width, dstW, a);
    const tmp = Buffer.alloc(dstW * img.height * 4);
    for (let y = 0; y < img.height; y++) {
        const srcRow = y * img.width * 4;
        const dstRow = y * dstW * 4;
        for (let x = 0; x < dstW; x++) {
            const { first, w } = cols[x];
            let r = 0, g = 0, b = 0, al = 0;
            for (let k = 0; k < w.length; k++) {
                const o = srcRow + (first + k) * 4;
                const wk = w[k];
                r += img.data[o] * wk;
                g += img.data[o + 1] * wk;
                b += img.data[o + 2] * wk;
                al += img.data[o + 3] * wk;
            }
            const o = dstRow + x * 4;
            tmp[o] = clamp8(r); tmp[o + 1] = clamp8(g);
            tmp[o + 2] = clamp8(b); tmp[o + 3] = clamp8(al);
        }
    }
    const rows = weights(img.height, dstH, a);
    const out = Buffer.alloc(dstW * dstH * 4);
    for (let y = 0; y < dstH; y++) {
        const { first, w } = rows[y];
        const dstRow = y * dstW * 4;
        for (let x = 0; x < dstW; x++) {
            let r = 0, g = 0, b = 0, al = 0;
            for (let k = 0; k < w.length; k++) {
                const o = (first + k) * dstW * 4 + x * 4;
                const wk = w[k];
                r += tmp[o] * wk;
                g += tmp[o + 1] * wk;
                b += tmp[o + 2] * wk;
                al += tmp[o + 3] * wk;
            }
            const o = dstRow + x * 4;
            out[o] = clamp8(r); out[o + 1] = clamp8(g);
            out[o + 2] = clamp8(b); out[o + 3] = clamp8(al);
        }
    }
    return { width: dstW, height: dstH, data: out };
};

const clamp8 = (v) => {
    const n = Math.round(v);
    return n < 0 ? 0 : n > 255 ? 255 : n;
};

const cover = (img, w, h) => {
    const srcAr = img.width / img.height;
    const dstAr = w / h;
    const size = srcAr > dstAr
        ? [Math.max(1, Math.round(h * srcAr)), h]
        : [w, Math.max(1, Math.round(w / srcAr))];
    const scaled = resample(img, size[0], size[1]);
    const left = (size[0] - w) >> 1;
    const top = (size[1] - h) >> 1;
    const out = Buffer.alloc(w * h * 4);
    for (let y = 0; y < h; y++) {
        scaled.data.copy(out, y * w * 4,
                         ((top + y) * size[0] + left) * 4,
                         ((top + y) * size[0] + left + w) * 4);
    }
    return { width: w, height: h, data: out };
};

const blit = (dst, src, x0, y0) => {
    for (let y = 0; y < src.height; y++) {
        const dy = y0 + y;
        if (dy < 0 || dy >= dst.height) continue;
        for (let x = 0; x < src.width; x++) {
            const dx = x0 + x;
            if (dx < 0 || dx >= dst.width) continue;
            const so = (y * src.width + x) * 4;
            const a = src.data[so + 3];
            if (a === 0) continue;
            const do_ = (dy * dst.width + dx) * 4;
            if (a === 255) {
                src.data.copy(dst.data, do_, so, so + 4);
                continue;
            }
            const da = dst.data[do_ + 3];
            const oa = a + ((da * (255 - a)) / 255 | 0);
            for (let c = 0; c < 3; c++) {
                const num = src.data[so + c] * a * 255 + dst.data[do_ + c] * da * (255 - a);
                dst.data[do_ + c] = oa === 0 ? 0 : (num / (255 * oa) | 0);
            }
            dst.data[do_ + 3] = oa;
        }
    }
};

const alphaBbox = (img) => {
    let minx = img.width, miny = img.height, maxx = -1, maxy = -1;
    for (let y = 0; y < img.height; y++) {
        for (let x = 0; x < img.width; x++) {
            if (img.data[(y * img.width + x) * 4 + 3] === 0) continue;
            if (x < minx) minx = x;
            if (x > maxx) maxx = x;
            if (y < miny) miny = y;
            if (y > maxy) maxy = y;
        }
    }
    if (maxx < minx || maxy < miny) return { x: 0, y: 0, w: img.width, h: img.height };
    return { x: minx, y: miny, w: maxx - minx + 1, h: maxy - miny + 1 };
};

const crop = (img, rect) => {
    const out = Buffer.alloc(rect.w * rect.h * 4);
    for (let y = 0; y < rect.h; y++) {
        img.data.copy(out, y * rect.w * 4,
                      ((rect.y + y) * img.width + rect.x) * 4,
                      ((rect.y + y) * img.width + rect.x + rect.w) * 4);
    }
    return { width: rect.w, height: rect.h, data: out };
};

const alpha8 = (img) => {
    const out = Buffer.alloc(img.width * img.height);
    for (let i = 0; i < img.width * img.height; i++)
        out[i] = img.data[i * 4 + 3];
    return out;
};

const rgb565 = (img) => {
    const out = Buffer.alloc(img.width * img.height * 2);
    for (let i = 0, o = 0; i < img.width * img.height; i++, o += 2) {
        const p = i * 4;
        out.writeUInt16LE(((img.data[p] & 0xf8) << 8)
                          | ((img.data[p + 1] & 0xfc) << 3)
                          | (img.data[p + 2] >> 3), o);
    }
    return out;
};

const encodePNG = (img) => {
    const raw = Buffer.alloc((img.width * 3 + 1) * img.height);
    for (let y = 0; y < img.height; y++) {
        const rowStart = y * (img.width * 3 + 1);
        raw[rowStart] = 0;
        for (let x = 0; x < img.width; x++) {
            const s = (y * img.width + x) * 4;
            const d = rowStart + 1 + x * 3;
            raw[d] = img.data[s];
            raw[d + 1] = img.data[s + 1];
            raw[d + 2] = img.data[s + 2];
        }
    }
    const chunk = (type, data) => {
        const len = Buffer.alloc(4);
        len.writeUInt32BE(data.length);
        const td = Buffer.concat([Buffer.from(type, 'ascii'), data]);
        const crc = Buffer.alloc(4);
        crc.writeUInt32BE(zlib.crc32(td));
        return Buffer.concat([len, td, crc]);
    };
    const ihdr = Buffer.alloc(13);
    ihdr.writeUInt32BE(img.width, 0);
    ihdr.writeUInt32BE(img.height, 4);
    ihdr[8] = 8; ihdr[9] = 2;
    return Buffer.concat([
        Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]),
        chunk('IHDR', ihdr),
        chunk('IDAT', zlib.deflateSync(raw, { level: 9 })),
        chunk('IEND', Buffer.alloc(0)),
    ]);
};

const blank = (w, h, r = 0, g = 0, b = 0, a = 255) => {
    const data = Buffer.alloc(w * h * 4);
    for (let i = 0; i < w * h; i++) {
        const o = i * 4;
        data[o] = r; data[o + 1] = g; data[o + 2] = b; data[o + 3] = a;
    }
    return { width: w, height: h, data };
};

module.exports = {
    decodePNG, decodeJPEG, decodeImage, encodePNG,
    resample, cover, blit, rgb565, blank,
    alphaBbox, crop, alpha8,
};
