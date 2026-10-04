import { Marked } from "marked";
import { readFile, writeFile, mkdir, cp } from "node:fs/promises";
import { join, posix } from "node:path";

const REPO = "https://github.com/Adolanium/hermes-gadget-sdk";
export const groups = [
  ["Get started", [["getting-started", "Choose your path"], ["desktop", "Try the simulator"], ["setup-board", "Set up a board"], ["connect-hermes", "Connect Hermes"]]],
  ["Use your gadget", [["using-gadget", "Talk, type, and interrupt"], ["troubleshooting", "Fix a problem"], ["simulator", "Simulator controls"]]],
  ["Build with the SDK", [["development", "Development and tests"], ["porting", "Add a board or action"], ["faces", "Customize the face"], ["hardware", "Hardware and wiring"]]],
  ["Reference", [["hardware-validation", "Hardware verification"], ["protocol", "Protocol"], ["architecture", "Architecture"], ["hermes-integration", "Hermes integration"]]],
];
const pages = new Set(groups.flatMap(([, entries]) => entries.map(([id]) => id)));
const escape = text => String(text).replace(/[&<>"']/g, c => ({"&":"&amp;", "<":"&lt;", ">":"&gt;", '"':"&quot;", "'":"&#39;"})[c]);
const slug = text => text.toLowerCase().replace(/<[^>]*>/g, "").replace(/[^\p{L}\p{N}_\-\s]/gu, "").replace(/ /g, "-");

export function docLink(href) {
  if (/^(?:[a-z]+:|#|\/)/i.test(href)) return href;
  const [path, hash] = href.split("#");
  const suffix = hash ? `#${hash}` : "";
  const target = posix.normalize(path);
  if (target.startsWith("images/")) return target + suffix;
  if (target.endsWith(".md") && pages.has(target.slice(0, -3))) return target.slice(0, -3) + ".html" + suffix;
  return `${REPO}/blob/main/${posix.normalize("docs/" + path)}${suffix}`;
}

export function renderDoc(id, markdown) {
  const headings = [], counts = new Map();
  const parser = new Marked({
    renderer: {
      heading({tokens, depth}) {
        const text = this.parser.parseInline(tokens);
        const base = slug(text), count = counts.get(base) ?? 0;
        counts.set(base, count + 1);
        const anchor = count ? `${base}-${count}` : base;
        if (depth === 2) headings.push([anchor, text]);
        return `<h${depth} id="${escape(anchor)}">${text}</h${depth}>\n`;
      },
      link({href, title, tokens}) {
        return `<a href="${escape(docLink(href))}"${title ? ` title="${escape(title)}"` : ""}>${this.parser.parseInline(tokens)}</a>`;
      },
      image({href, text}) {
        return `<img src="${escape(docLink(href))}" alt="${escape(text)}" loading="lazy">`;
      },
    },
  });
  const content = parser.parse(markdown);
  const title = markdown.match(/^# (.+)$/m)?.[1] ?? id;
  const nav = groups.map(([name, entries]) => `<section><h2>${name}</h2>${entries.map(([key, label]) => `<a href="${key}.html"${key === id ? ' aria-current="page"' : ""}>${label}</a>`).join("")}</section>`).join("");
  return `<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>${escape(title)} · Hermes Gadget</title><meta name="description" content="${escape(title)} for Hermes Gadget. Setup, controls, and development guides.">
<link rel="icon" href="../img/logo.png"><link rel="stylesheet" href="../docs.css"><script type="module" src="../docs.js"></script></head>
<body><a class="skip-link" href="#content">Skip to content</a>
<header><a class="brand" href="../"><img src="../img/logo.png" width="32" height="32" alt="">Hermes Gadget</a><button id="search-open" type="button" hidden>Search the docs <kbd>Ctrl K</kbd></button><nav aria-label="Project"><a href="../installer.html">Installer</a><a href="${REPO}">GitHub</a></nav></header>
<div class="docs-layout"><aside class="sidebar"><details open><summary>Browse the docs</summary><nav aria-label="Documentation">${nav}</nav></details></aside>
<main id="content" tabindex="-1"><p class="breadcrumb"><a href="getting-started.html">Docs</a> / ${escape(title)}</p><article>${content}</article><footer><a href="${REPO}/blob/main/docs/${id}.md">Edit this page on GitHub</a><p>Independent community project. Not affiliated with Nous Research.</p></footer></main>
<aside class="toc"><p>On this page</p><nav aria-label="On this page">${headings.map(([anchor, text]) => `<a href="#${escape(anchor)}">${text}</a>`).join("")}</nav><a href="troubleshooting.html">Need help?</a></aside></div>
<dialog id="search-dialog"><form method="dialog"><label for="search-input">Search the docs</label><button aria-label="Close search">Close</button></form><input id="search-input" type="search" placeholder="Pairing, microphone, Wi-Fi..." autocomplete="off"><p id="search-status" role="status"></p><ul id="search-results"></ul></dialog>
</body></html>`;
}

export async function buildDocs(repo, out) {
  const dir = join(out, "docs"), index = [];
  await mkdir(dir, {recursive:true});
  for (const id of pages) {
    const markdown = await readFile(join(repo, "docs", `${id}.md`), "utf8");
    await writeFile(join(dir, `${id}.html`), renderDoc(id, markdown));
    const title = markdown.match(/^# (.+)$/m)[1];
    index.push({title, url:`${id}.html`, text:markdown.replace(/[#*`|]/g, "").replace(/\s+/g, " ")});
  }
  await writeFile(join(dir, "index.html"), renderDoc("getting-started", await readFile(join(repo, "docs", "getting-started.md"), "utf8")));
  await writeFile(join(dir, "search.json"), JSON.stringify(index));
  await cp(join(repo, "docs", "images"), join(dir, "images"), {recursive:true});
  console.log(`build: ${pages.size} documentation pages`);
}
