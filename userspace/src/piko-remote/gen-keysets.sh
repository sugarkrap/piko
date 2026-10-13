#!/bin/sh
set -eu

src="$1/pirkeysetmanager.cpp"

echo '#include <vector>'
echo '#include "pirkeysetmetadata.h"'
grep '^#include "keysets/' "$src"
echo 'void piko_keysets(std::vector<PIRKeysetMetaData *> &out)'
echo '{'
echo '    unsigned int counter = 0;'
sed -n 's/^ *setupKeyset(new \([A-Za-z0-9_]*\)(++counter));.*$/    out.push_back(new \1(++counter));/p' "$src"
echo '}'
