// Bundles src/plugin.jsx into ../desktop/plugin.js: one ESM file whose only imports are the three
// specifiers Hermes Desktop resolves (@hermes/plugin-sdk, react, react/jsx-runtime).
import { build } from 'esbuild'
import { readFileSync, statSync } from 'node:fs'
import { dirname, join } from 'node:path'
import { fileURLToPath } from 'node:url'

const root = join(dirname(fileURLToPath(import.meta.url)), '..')
const outfile = join(root, '..', 'desktop', 'plugin.js')
const { version } = JSON.parse(readFileSync(join(root, 'package.json'), 'utf8'))

await build({
  entryPoints: [join(root, 'src', 'plugin.jsx')],
  outfile,
  bundle: true,
  format: 'esm',
  jsx: 'automatic',
  target: 'es2022',
  minifySyntax: true,
  minifyWhitespace: true,
  legalComments: 'eof',
  external: ['react', 'react/jsx-runtime', '@hermes/plugin-sdk'],
  loader: { '.png': 'dataurl' },
  banner: { js: `// Hermes Gadget ${version} — built from desktop-src/ (npm ci && npm run build). Do not edit.` },
  logLevel: 'warning'
})
console.log(`wrote ${outfile} (${(statSync(outfile).size / 1024).toFixed(0)} KiB)`)
