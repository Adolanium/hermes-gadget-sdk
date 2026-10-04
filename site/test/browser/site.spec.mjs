import { test, expect } from "@playwright/test";

test("visitors can choose a path and search the guides", async ({page}) => {
  await page.goto("/");
  await page.getByRole("link", {name:"Try the simulator", exact:true}).click();
  await expect(page.getByRole("heading", {name:"Try the simulator", exact:true})).toBeVisible();
  await page.getByRole("tab", {name:"Windows", exact:true}).click();
  await expect(page.getByRole("tabpanel")).toContainText(".\\.venv\\Scripts\\Activate.ps1");
  await expect(page.getByText(/the build completes and the core tests pass/)).toBeVisible();
  await page.getByRole("tab", {name:"Windows", exact:true}).press("ArrowRight");
  await expect(page.getByRole("tabpanel")).toContainText("source .venv/bin/activate");
  await page.getByRole("button", {name:/Search the docs/}).click();
  await page.getByRole("searchbox").fill("pairing");
  await page.getByRole("dialog").getByRole("link", {name:"Connect Hermes", exact:true}).click();
  await expect(page.getByRole("heading", {name:"Connect Hermes", exact:true})).toBeVisible();
});

test("existing Hermes links keep the server address and reach the installer", async ({page}) => {
  await page.goto("/#server=ws%3A%2F%2F192.168.1.20%3A8765%2Fgadget");
  await expect(page).toHaveURL(/installer.html#server=/);
  await expect(page.getByLabel("Hermes address", {exact:true})).toHaveValue("ws://192.168.1.20:8765/gadget");
});

test("deep links choose the right platform, and copy gives the displayed commands", async ({page, context}) => {
  await context.grantPermissions(["clipboard-read", "clipboard-write"]);
  await page.goto("/docs/desktop.html#linux");
  await expect(page.getByRole("tab", {name:"Linux", exact:true})).toHaveAttribute("aria-selected", "true");
  await page.getByRole("tabpanel").getByRole("button", {name:"Copy commands"}).click();
  expect(await page.evaluate(() => navigator.clipboard.readText())).toContain("source .venv/bin/activate");
});

test("homepage and guides fit a phone without horizontal scrolling", async ({page}) => {
  await page.setViewportSize({width:390,height:844});
  for (const url of ["/", "/docs/desktop.html", "/docs/setup-board.html", "/installer.html"]) {
    await page.goto(url);
    expect(await page.evaluate(() => document.documentElement.scrollWidth)).toBe(390);
  }
});
