#!/usr/bin/env node
'use strict';

const fs = require('fs');
const path = require('path');

const GLOBAL_HDR = 0x10;
const BLOCK_REC = 0x4210;
const BLOCK_HDR = 0x10;
const BLOCK_PAYLOAD = 0x4200;
const PAGE_MAIN = 512;
const PAGES_PER_BLOCK = 32;

const NAND_BLOCKS = 8192;
const FULL_MAIN_SIZE = 0x8000000;
const SMF_SIZE = 0x700000;
const ROOT_SIZE = 0x3500000;
const HOME_SIZE = 0x4400000;

function parseArgs(argv) {
    const args = { outDir: 'flash/.cache/dbk' };
    const positional = [];
    for (let i = 0; i < argv.length; i++) {
        if (argv[i] === '--out-dir') {
            args.outDir = argv[++i];
        } else {
            positional.push(argv[i]);
        }
    }
    if (positional.length !== 1) {
        console.error('usage: dbk-extract.js <path-to.dbk> [--out-dir DIR]');
        process.exit(1);
    }
    args.dbk = positional[0];
    return args;
}

function payloadToMain(payload) {
    if (payload.length !== BLOCK_PAYLOAD) {
        throw new Error('invalid payload size');
    }
    return payload.subarray(0, PAGES_PER_BLOCK * PAGE_MAIN);
}

function main() {
    const args = parseArgs(process.argv.slice(2));
    fs.mkdirSync(args.outDir, { recursive: true });

    const data = fs.readFileSync(args.dbk);
    const expected = GLOBAL_HDR + NAND_BLOCKS * BLOCK_REC;
    if (data.length !== expected) {
        console.error(`unexpected dbk size: got ${data.length}, expected ${expected}`);
        process.exit(1);
    }

    if (data.toString('ascii', 0, 12) !== '[I]JFFS2TOP:') {
        console.error('unexpected global header magic');
        process.exit(1);
    }

    const fullMain = Buffer.alloc(FULL_MAIN_SIZE);
    const headersPath = path.join(args.outDir, 'block_headers.bin');
    const headersFd = fs.openSync(headersPath, 'w');

    let off = GLOBAL_HDR;
    for (let idx = 0; idx < NAND_BLOCKS; idx++) {
        const rec = data.subarray(off, off + BLOCK_REC);
        if (rec.length !== BLOCK_REC) {
            throw new Error(`short record at block ${idx}`);
        }
        const hdr = rec.subarray(0, BLOCK_HDR);
        const payload = rec.subarray(BLOCK_HDR);
        fs.writeSync(headersFd, hdr);

        const blockMain = payloadToMain(payload);
        const start = idx * (PAGES_PER_BLOCK * PAGE_MAIN);
        blockMain.copy(fullMain, start);

        off += BLOCK_REC;
    }
    fs.closeSync(headersFd);

    const fullMainPath = path.join(args.outDir, 'full_main.bin');
    fs.writeFileSync(fullMainPath, fullMain);

    const smf = fullMain.subarray(0, SMF_SIZE);
    const root = fullMain.subarray(SMF_SIZE, SMF_SIZE + ROOT_SIZE);
    const home = fullMain.subarray(SMF_SIZE + ROOT_SIZE, SMF_SIZE + ROOT_SIZE + HOME_SIZE);

    const smfPath = path.join(args.outDir, 'smf_main.bin');
    const rootPath = path.join(args.outDir, 'root_main.bin');
    const homePath = path.join(args.outDir, 'home_main.bin');
    fs.writeFileSync(smfPath, smf);
    fs.writeFileSync(rootPath, root);
    fs.writeFileSync(homePath, home);

    console.log(`dbk: ${args.dbk}`);
    console.log(`size: ${data.length} bytes`);
    console.log(`full_main: ${fullMainPath} (${fullMain.length} bytes)`);
    console.log(`smf_main: ${smfPath} (${smf.length} bytes)`);
    console.log(`root_main: ${rootPath} (${root.length} bytes)`);
    console.log(`home_main: ${homePath} (${home.length} bytes)`);
    console.log(`headers: ${headersPath} (${fs.statSync(headersPath).size} bytes)`);
}

main();
