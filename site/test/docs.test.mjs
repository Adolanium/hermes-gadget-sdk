import assert from "node:assert/strict";
import { test } from "node:test";
import { mkdtemp, mkdir, readFile, writeFile, rm } from "node:fs/promises";
import { join } from "node:path";
import { tmpdir } from "node:os";
import { buildDocs, docLink, groups, renderDoc, validateDocs } from "../scripts/docs.mjs";

test("published guides keep links to guides, anchors, images, and repository files", () => {
  assert.equal(docLink("desktop.md#windows"), "desktop.html#windows");
  assert.equal(docLink("tailscale-funnel.md#connect-the-gadget"), "tailscale-funnel.html#connect-the-gadget");
  assert.equal(docLink("images/screen-ready.png"), "images/screen-ready.png");
  assert.equal(docLink("../python/hermes_gadget/sim/window.py"), "https://github.com/Adolanium/hermes-gadget-sdk/blob/main/python/hermes_gadget/sim/window.py");
  assert.equal(docLink("https://example.com/a.md"), "https://example.com/a.md");
  assert.equal(docLink("#ready"), "#ready");
  const html = renderDoc("desktop", '# Try it\n\n## Build\n\n[Windows](desktop.md#windows)\n\n## Build\n\n```bash\necho "<ready>"\n```');
  assert.match(html, /<h2 id="build">Build<\/h2>/);
  assert.match(html, /<h2 id="build-1">Build<\/h2>/);
  assert.match(html, /href="desktop.html#windows">Windows/);
  assert.match(html, /echo &quot;&lt;ready&gt;&quot;/);
  assert.match(html, /href="desktop.html" aria-current="page"/);
});

async function fixture(t) {
  const repo = await mkdtemp(join(tmpdir(), "hermes-docs-"));
  t.after(() => rm(repo, {recursive:true, force:true}));
  await mkdir(join(repo, "docs", "images"), {recursive:true});
  for (const [, entries] of groups) {
    for (const [id] of entries) await writeFile(join(repo, "docs", `${id}.md`), `# ${id}\n`);
  }
  return repo;
}

test("the docs build publishes every registered guide in navigation and search", async t => {
  const repo = await fixture(t), out = join(repo, "output");
  await writeFile(join(repo, "README.md"), '[Start](docs/desktop.md?plain=1#install-1)\n<img src="docs/images/picture.png">');
  await writeFile(join(repo, "docs", "images", "picture.png"), "test image");
  await writeFile(join(repo, "docs", "desktop.md"), '# Desktop\n\n## Install\n\n## Install\n\n[Again][ref]\n\n[ref]: #install-1\n\n```md\n[example](missing.md)\n```');
  await buildDocs(repo, out);
  const index = JSON.parse(await readFile(join(out, "docs", "search.json"), "utf8"));
  assert.equal(index.find(page => page.url === "desktop.html").title, "Desktop");
  for (const [, entries] of groups) {
    for (const [id] of entries) {
      const html = await readFile(join(out, "docs", `${id}.html`), "utf8");
      assert.ok(html.includes(`href="${id}.html" aria-current="page"`), id);
      assert.equal(index.filter(page => page.url === `${id}.html`).length, 1, id);
    }
  }
});

test("a new guide must be registered before the build can publish it", async t => {
  const repo = await fixture(t);
  await writeFile(join(repo, "docs", "new-board.md"), "# New board\n");
  await assert.rejects(buildDocs(repo, join(repo, "output")), /docs\/new-board.md: missing navigation\/search registration/);
});

test("missing guides, local files, and section anchors identify their source", async t => {
  const repo = await fixture(t);
  await rm(join(repo, "docs", "desktop.md"));
  await writeFile(join(repo, "README.md"), '[Guide](docs/desktop.md)\n[Section](docs/linux.md#missing)\n<img src="missing.png">');
  await assert.rejects(validateDocs(repo), error => {
    assert.match(error.message, /docs\/desktop.md: registered guide does not exist/);
    assert.match(error.message, /README.md: missing link target: docs\/desktop.md/);
    assert.match(error.message, /README.md: missing anchor: docs\/linux.md#missing/);
    assert.match(error.message, /README.md: missing link target: missing.png/);
    return true;
  });
});
