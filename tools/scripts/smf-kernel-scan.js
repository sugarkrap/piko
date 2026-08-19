#!/usr/bin/env node
'use strict';

const fs = require('fs');

const GLOBAL_HDR = 0x10;
const BLOCK_REC = 0x4210;
const BLOCK_HDR = 0x10;
const PAGE_MAIN = 512;
const PAGE_OOB = 16;
const PAGES_PER_BLOCK = 32;
const ERASE_SIZE = PAGES_PER_BLOCK * PAGE_MAIN;
const OOB_AREA = PAGES_PER_BLOCK * PAGE_OOB;
const NAND_BLOCKS = 8192;
const SMF_SIZE = 0x700000;
const SMF_BLOCKS = SMF_SIZE / ERASE_SIZE;

const ZIMAGE_MAGIC = 0x016f2818;
const ZIMAGE_MAGIC_OFF = 0x24;

function parseArgs(argv) {
    const args = { physical: false };
    const positional = [];
    for (let i = 0; i < argv.length; i++) {
        if (argv[i] === '--physical') args.physical = true;
        else positional.push(argv[i]);
    }
    if (positional.length !== 1) {
        console.error('usage: smf-kernel-scan.js <recovery.dbk | smf_main.bin> [--physical]');
        process.exit(1);
    }
    args.input = positional[0];
    return args;
}

function popcount16(v) {
    let c = 0;
    while (v) { c += v & 1; v >>= 1; }
    return c;
}

function decodeOobLogical(us) {
    if (us === 0xffff) return -1;
    if (popcount16(us) & 1) return -1;
    return (us >> 1) & 0x3ff;
}

function blockLogical(oob) {
    const c = [
        oob[8] | (oob[9] << 8),
        oob[10] | (oob[11] << 8),
        oob[12] | (oob[13] << 8),
    ].map(decodeOobLogical);
    if (c[0] >= 0 && c[0] === c[1]) return c[0];
    if (c[0] >= 0 && c[0] === c[2]) return c[0];
    if (c[1] >= 0 && c[1] === c[2]) return c[1];
    return c.find((v) => v >= 0) ?? -1;
}

function readDbk(file) {
    const data = fs.readFileSync(file);
    const expected = GLOBAL_HDR + NAND_BLOCKS * BLOCK_REC;
    if (data.length !== expected) {
        console.error(`smf-kernel-scan: unexpected dbk size ${data.length}, expected ${expected}`);
        process.exit(1);
    }
    if (data.toString('ascii', 0, 12) !== '[I]JFFS2TOP:') {
        console.error('smf-kernel-scan: unexpected global header magic');
        process.exit(1);
    }
    const blocks = [];
    let off = GLOBAL_HDR;
    for (let idx = 0; idx < NAND_BLOCKS; idx++) {
        const payload = data.subarray(off + BLOCK_HDR, off + BLOCK_REC);
        blocks.push({
            main: payload.subarray(0, ERASE_SIZE),
            oob: payload.subarray(ERASE_SIZE, ERASE_SIZE + OOB_AREA),
        });
        off += BLOCK_REC;
    }
    return blocks;
}

function buildLogicalSmf(blocks) {
    const out = Buffer.alloc(SMF_SIZE, 0xff);
    const seen = new Map();
    let mapped = 0;
    for (let phys = 0; phys < SMF_BLOCKS; phys++) {
        const logical = blockLogical(blocks[phys].oob);
        if (logical < 0 || logical >= SMF_BLOCKS) continue;
        seen.set(logical, phys);
        blocks[phys].main.copy(out, logical * ERASE_SIZE);
        mapped++;
    }
    return { image: out, mapped, distinct: seen.size };
}

function buildPhysicalSmf(blocks) {
    const out = Buffer.alloc(SMF_SIZE, 0xff);
    for (let phys = 0; phys < SMF_BLOCKS; phys++) {
        blocks[phys].main.copy(out, phys * ERASE_SIZE);
    }
    return { image: out, mapped: SMF_BLOCKS, distinct: SMF_BLOCKS };
}

function findKernels(image) {
    const hits = [];
    for (let off = 0; off + ZIMAGE_MAGIC_OFF + 4 <= image.length; off += ERASE_SIZE) {
        if (image.readUInt32LE(off + ZIMAGE_MAGIC_OFF) === ZIMAGE_MAGIC) {
            const start = image.readUInt32LE(off + ZIMAGE_MAGIC_OFF + 4);
            const end = image.readUInt32LE(off + ZIMAGE_MAGIC_OFF + 8);
            hits.push({ off, kind: 'zImage', size: end - start });
        } else if (image[off] === 0x1f && image[off + 1] === 0x8b && image[off + 2] === 0x08) {
            hits.push({ off, kind: 'gzip', size: null });
        }
    }
    return hits;
}

function hex(n) { return `0x${n.toString(16).toUpperCase()}`; }

function main() {
    const args = parseArgs(process.argv.slice(2));
    const stat = fs.statSync(args.input);

    let built;
    if (stat.size === SMF_SIZE) {
        const raw = fs.readFileSync(args.input);
        built = { image: raw, mapped: SMF_BLOCKS, distinct: SMF_BLOCKS };
        console.log(`input: ${args.input} (raw smf image, physical order)`);
    } else {
        const blocks = readDbk(args.input);
        built = args.physical ? buildPhysicalSmf(blocks) : buildLogicalSmf(blocks);
        console.log(`input: ${args.input} (dbk, ${args.physical ? 'physical' : 'FTL logical'} order)`);
        console.log(`blocks placed: ${built.mapped}, distinct logical: ${built.distinct} of ${SMF_BLOCKS}`);
    }

    const hits = findKernels(built.image);
    console.log('');
    if (hits.length === 0) {
        console.log('no kernel image found at any eraseblock boundary');
        if (!args.physical) console.log('try --physical, the FTL map may be unreadable in this dump');
        return;
    }

    console.log(`${hits.length} kernel image(s) at eraseblock boundaries:`);
    for (const h of hits) {
        const size = h.size === null ? '' : `  size ${h.size} (${(h.size / 1048576).toFixed(3)} MiB)`;
        console.log(`  ${hex(h.off).padEnd(10)} block ${String(h.off / ERASE_SIZE).padStart(3)}  ${h.kind}${size}`);
    }

    if (hits.length < 2) return;
    console.log('');
    console.log('gaps between consecutive images:');
    const gaps = [];
    for (let i = 1; i < hits.length; i++) {
        const g = hits[i].off - hits[i - 1].off;
        gaps.push(g);
        console.log(`  ${hex(hits[i - 1].off)} -> ${hex(hits[i].off)}  ${g} bytes  ${g / ERASE_SIZE} blocks  ${(g / 1048576).toFixed(4)} MiB`);
    }
    const uniform = gaps.every((g) => g === gaps[0]);
    console.log('');
    console.log(uniform
        ? `slot pitch is uniform: ${gaps[0]} bytes (${gaps[0] / ERASE_SIZE} eraseblocks) -- that is the real mtd1 budget`
        : 'slot pitch is not uniform, the smallest gap bounds the budget');
}

main();
