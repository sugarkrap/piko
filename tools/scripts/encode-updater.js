#!/usr/bin/env node
'use strict';

const fs = require('fs');

const CONFIRMED_MAPPING = {
    0x0a: 0x2f, 0x20: 0x41, 0x21: 0x22, 0x23: 0x24, 0x2d: 0x2e,
    0x2f: 0xc7, 0x61: 0xe9, 0x62: 0x03, 0x63: 0xb0, 0x64: 0x60,
    0x65: 0xec, 0x66: 0x39, 0x68: 0x73, 0x69: 0xa8, 0x6b: 0x12,
    0x6c: 0x31, 0x6d: 0xe4, 0x6e: 0x30, 0x6f: 0x25, 0x70: 0xe7,
    0x72: 0x9d, 0x73: 0x6e, 0x74: 0x3e, 0x75: 0x50, 0x76: 0x77,
};

function main() {
    const infile = process.argv[2] || 'flash/updater-uncoded.sh';
    const outfile = process.argv[3] || 'build/flash/updater-encoded.sh';

    const plaintext = fs.readFileSync(infile);

    const missing = [...new Set(plaintext)].filter((b) => !(b in CONFIRMED_MAPPING)).sort((a, b) => a - b);
    if (missing.length > 0) {
        const rendered = missing.map((b) => `0x${b.toString(16).padStart(2, '0')} (${JSON.stringify(String.fromCharCode(b))})`).join(', ');
        process.stderr.write(
            'encode-updater: input uses byte(s) not in the confirmed mapping: ' + rendered + '\n' +
            '  Extend CONFIRMED_MAPPING via the known-plaintext technique --\n' +
            '  an unconfirmed mapping produces a corrupt updater.sh that silently fails on\n' +
            "  Cacko's recovery menu (no verification path exists there).\n"
        );
        process.exit(1);
    }

    const encoded = Buffer.from([...plaintext].map((b) => CONFIRMED_MAPPING[b]));
    fs.writeFileSync(outfile, encoded);

    console.log(`${infile} -> ${outfile} (${plaintext.length} bytes)`);
    console.log('first 10 bytes:', encoded.subarray(0, 10).toString('hex').match(/../g).join(' '));
}

main();
