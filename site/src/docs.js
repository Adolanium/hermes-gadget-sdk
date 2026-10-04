const dialog = document.getElementById("search-dialog");
const input = document.getElementById("search-input");
const status = document.getElementById("search-status");
const results = document.getElementById("search-results");
let index;
let request = 0;

document.getElementById("search-open").hidden = false;
document.getElementById("search-open").addEventListener("click", () => {
  dialog.showModal();
  input.focus();
});
document.addEventListener("keydown", event => {
  if ((event.ctrlKey || event.metaKey) && event.key.toLowerCase() === "k") {
    event.preventDefault();
    if (!dialog.open) dialog.showModal();
    input.focus();
  }
});
input.addEventListener("input", async () => {
  const current = ++request;
  const query = input.value.trim().toLowerCase();
  results.replaceChildren();
  if (!query) { status.textContent = "Type a topic to find a guide."; return; }
  status.textContent = "Searching...";
  try {
    if (!index) {
      const response = await fetch("search.json");
      if (!response.ok) throw new Error("Search index unavailable");
      index = await response.json();
    }
    if (current !== request) return;
    const words = query.split(/\s+/);
    const matches = index.filter(page => words.every(word => page.text.toLowerCase().includes(word)))
      .sort((a, b) => Number(b.title.toLowerCase().includes(query)) - Number(a.title.toLowerCase().includes(query)));
    status.textContent = matches.length ? `${matches.length} guides found.` : "No matches. Try a shorter term, or browse the guides.";
    for (const page of matches) {
      const item = document.createElement("li"), link = document.createElement("a"), preview = document.createElement("p");
      link.href = page.url;
      link.textContent = page.title;
      const start = Math.max(0, page.text.toLowerCase().indexOf(words[0]) - 45);
      preview.textContent = (start ? "…" : "") + page.text.slice(start, start + 170) + "…";
      item.append(link, preview);
      results.append(item);
    }
  } catch {
    if (current === request) status.textContent = "Search could not load. Browse the guides or try again.";
  }
});

for (const pre of document.querySelectorAll("pre")) {
  const button = document.createElement("button");
  button.type = "button";
  button.className = "copy-code";
  button.textContent = "Copy commands";
  button.addEventListener("click", async () => {
    try {
      await navigator.clipboard.writeText(pre.querySelector("code").textContent);
      button.textContent = "Copied";
    } catch {
      button.textContent = "Select the text to copy";
    }
    setTimeout(() => { button.textContent = "Copy commands"; }, 2000);
  });
  pre.append(button);
}

// Markdown stays readable on GitHub and without JavaScript. Tabs enhance only the desktop guide.
const platforms = ["windows", "macos", "linux"];
const headings = platforms.map(id => document.querySelector(`article h3#${id}`));
if (headings.every(Boolean)) {
  const tabs = document.createElement("div");
  tabs.className = "os-tabs";
  tabs.setAttribute("role", "tablist");
  tabs.setAttribute("aria-label", "Your operating system");
  headings[0].before(tabs);
  const panels = headings.map((heading, i) => {
    const panel = document.createElement("section");
    panel.id = `platform-${platforms[i]}`;
    panel.setAttribute("role", "tabpanel");
    panel.setAttribute("aria-labelledby", `tab-${platforms[i]}`);
    heading.before(panel);
    let node = heading;
    do {
      const next = node.nextSibling;
      panel.append(node);
      node = next;
    } while (node && !(node.nodeType === 1 && (/^H[23]$/.test(node.tagName)
      || node.querySelector(":scope > strong")?.textContent === "You know it worked when:")));
    const button = document.createElement("button");
    button.type = "button";
    button.id = `tab-${platforms[i]}`;
    button.textContent = heading.textContent;
    button.setAttribute("role", "tab");
    button.setAttribute("aria-controls", panel.id);
    button.addEventListener("click", () => select(i));
    button.addEventListener("keydown", event => {
      const target = {ArrowRight:(i+1)%3, ArrowLeft:(i+2)%3, Home:0, End:2}[event.key];
      if (target === undefined) return;
      event.preventDefault();
      select(target);
      tabs.children[target].focus();
    });
    tabs.append(button);
    return panel;
  });
  function select(index) {
    panels.forEach((panel, i) => {
      panel.hidden = i !== index;
      tabs.children[i].setAttribute("aria-selected", String(i === index));
      tabs.children[i].tabIndex = i === index ? 0 : -1;
    });
  }
  function fromHash() {
    const i = platforms.indexOf(location.hash.slice(1));
    if (i >= 0) select(i);
    return i;
  }
  select(/Mac/i.test(navigator.platform) ? 1 : /Linux/i.test(navigator.platform) ? 2 : 0);
  fromHash();
  window.addEventListener("hashchange", fromHash);
}
