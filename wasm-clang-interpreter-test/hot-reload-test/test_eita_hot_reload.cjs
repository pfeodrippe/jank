// Test hot-reload functionality with eita.jank
//
// This test:
// 1. Loads eita.wasm (with HOT_RELOAD=1 enabled)
// 2. Calls ggg(10) - should return 58 (10+48)
// 3. Loads the patch eita_ggg_patch.wasm via jank_hot_reload_load_patch()
// 4. Calls ggg(10) again - should now return 59 (10+49)

const fs = require('fs');
const path = require('path');

// Patch file location - using auto-generated patch from generate-wasm-patch script
const PATCH_PATH = '/Users/pfeodrippe/dev/jank/wasm-clang-interpreter-test/hot-reload-test/ggg_patch.wasm';

console.log('=== jank WASM Hot-Reload Test (eita.jank) ===\n');

async function test() {
  // Load the main WASM module
  const modulePath = '/Users/pfeodrippe/dev/jank/compiler+runtime/build-wasm/eita.js';
  console.log(`Loading module: ${modulePath}`);

  const Module = await import(modulePath);
  const mod = await Module.default();

  console.log('\n1. Testing original ggg function (should add 48):');

  // Get the exported ggg function (note: emscripten adds underscore prefix)
  const ggg = mod._jank_export_ggg;

  // Test with value 10: expect 10+48 = 58
  const result1 = ggg(10);
  console.log(`   ggg(10) = ${result1} (expected: 58)`);

  if (result1 !== 58) {
    console.log(`   ❌ FAIL: Expected 58, got ${result1}`);
    return false;
  }
  console.log(`   ✅ PASS`);

  console.log('\n2. Loading hot-reload patch (changes +48 to +49):');

  // Copy patch to virtual FS
  const patchBytes = fs.readFileSync(PATCH_PATH);
  const patchVfsPath = '/tmp/eita_ggg_patch.wasm';
  mod.FS.writeFile(patchVfsPath, patchBytes);

  console.log(`   Patch size: ${patchBytes.length} bytes`);
  console.log(`   Loading patch via jank_hot_reload_load_patch()...`);

  // Load the patch
  const loadResult = mod.ccall(
    'jank_hot_reload_load_patch',
    'number',
    ['string'],
    [patchVfsPath]
  );

  if (loadResult !== 0) {
    console.log(`   ❌ FAIL: jank_hot_reload_load_patch returned ${loadResult}`);
    return false;
  }
  console.log(`   ✅ Patch loaded successfully!`);

  console.log('\n3. Testing hot-reloaded ggg function (should now add 49):');

  // Test again with value 10: expect 10+49 = 59
  const result2 = ggg(10);
  console.log(`   ggg(10) = ${result2} (expected: 59)`);

  if (result2 !== 59) {
    console.log(`   ❌ FAIL: Expected 59, got ${result2}`);
    return false;
  }
  console.log(`   ✅ PASS - HOT-RELOAD WORKING!`);

  console.log('\n4. Getting hot-reload statistics:');
  try {
    const statsJson = mod.ccall(
      'jank_hot_reload_get_stats',
      'string',
      [],
      []
    );
    console.log(`   Raw stats: ${statsJson}`);
    if (statsJson && statsJson.startsWith('{')) {
      const stats = JSON.parse(statsJson);
      console.log(`   ${JSON.stringify(stats, null, 2)}`);
    }
  } catch (e) {
    console.log(`   (Stats parsing error: ${e.message})`);
  }

  return true;
}

test()
  .then(success => {
    if (success) {
      console.log('\n🎉 Hot-reload test PASSED! jank REPL-like speed achieved!\n');
      process.exit(0);
    } else {
      console.log('\n❌ Hot-reload test FAILED\n');
      process.exit(1);
    }
  })
  .catch(err => {
    console.error('\n❌ Test error:', err);
    process.exit(1);
  });
