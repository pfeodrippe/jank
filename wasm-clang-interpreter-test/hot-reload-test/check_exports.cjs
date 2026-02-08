// Check what's exported from eita.wasm

async function check() {
  const modulePath = '/Users/pfeodrippe/dev/jank/compiler+runtime/build-wasm/eita.js';
  console.log(`Loading module: ${modulePath}`);

  const Module = await import(modulePath);
  const mod = await Module.default();

  console.log('\nExported functions:');
  Object.keys(mod).filter(k => k.startsWith('jank_export_')).forEach(k => {
    console.log(`  ${k}: ${typeof mod[k]}`);
  });

  console.log('\nChecking specific exports:');
  console.log(`  mod.jank_export_ggg: ${typeof mod.jank_export_ggg}`);
  console.log(`  mod._jank_export_ggg: ${typeof mod._jank_export_ggg}`);

  console.log('\nAll exported symbols containing "ggg":');
  Object.keys(mod).filter(k => k.toLowerCase().includes('ggg')).forEach(k => {
    console.log(`  ${k}: ${typeof mod[k]}`);
  });
}

check().catch(err => console.error(err));
