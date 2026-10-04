import assert from "node:assert/strict";
import { test } from "node:test";
import { docLink, renderDoc } from "../scripts/docs.mjs";

test("published guides keep links to guides, anchors, images, and repository files", () => {
  assert.equal(docLink("desktop.md#windows"), "desktop.html#windows");
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
