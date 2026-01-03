// Hot-reload test script
// Demonstrates loading v1, then hot-swapping to v2

const fs = require('fs');

async function main() {
    console.log('=== jank Hot-Reload Test ===\n');
    console.log('Loading main module...');

    const createModule = require('./main.js');

    const Module = await createModule({
        print: (text) => console.log('[WASM]', text),
        printErr: (text) => console.error('[WASM ERR]', text)
    });

    // Write side modules to virtual FS
    Module.FS.writeFile('/ggg_v1.wasm', fs.readFileSync('./ggg_v1.wasm'));
    Module.FS.writeFile('/ggg_v2.wasm', fs.readFileSync('./ggg_v2.wasm'));

    console.log('Main module ready!\n');

    // Test 1: Load v1 (+ 48)
    console.log('--- Test 1: Loading v1 (ggg adds 48) ---');
    const startV1 = performance.now();
    const loadResult1 = Module._load_module(Module.stringToNewUTF8('/ggg_v1.wasm'));
    const loadTimeV1 = performance.now() - startV1;
    console.log(`Load time: ${loadTimeV1.toFixed(2)}ms`);

    if (loadResult1 === 0) {
        const result1 = Module._call_ggg(10);
        console.log(`call_ggg(10) = ${result1} (expected: 58 = 10+48)`);
        console.log(result1 === 58 ? 'PASS\n' : 'FAIL\n');
    } else {
        console.log('FAIL - module did not load\n');
    }

    // Test 2: Hot-reload v2 (+ 49)
    console.log('--- Test 2: Hot-reload to v2 (ggg adds 49) ---');
    const startV2 = performance.now();
    const loadResult2 = Module._load_module(Module.stringToNewUTF8('/ggg_v2.wasm'));
    const loadTimeV2 = performance.now() - startV2;
    console.log(`Load time: ${loadTimeV2.toFixed(2)}ms`);

    if (loadResult2 === 0) {
        const result2 = Module._call_ggg(10);
        console.log(`call_ggg(10) = ${result2} (expected: 59 = 10+49)`);
        console.log(result2 === 59 ? 'PASS\n' : 'FAIL\n');
    } else {
        console.log('FAIL - module did not load\n');
    }

    // Summary
    console.log('=== Summary ===');
    console.log(`Side module load time: ~${((loadTimeV1 + loadTimeV2) / 2).toFixed(2)}ms`);
    console.log(`Side module size: ${fs.statSync('./ggg_v1.wasm').size} bytes`);
    console.log('\nFor comparison:');
    console.log('- emcc compile time: ~170ms (measured earlier)');
    console.log('- Total hot-reload time: compile + network + load');
    console.log('- Goal: < 200ms for REPL-like experience');
}

main().catch(console.error);
