import {test, expect} from "@playwright/test";
import {createHash} from "node:crypto";

const image = Buffer.alloc(512, 0xe9);
const manifest = {version:"test", builds:[{board:"esp32s3-touch-amoled-175", title:"Waveshare ESP32-S3-Touch-AMOLED-1.75", summary:"Round display and microphone", ready_made:true, chip:"ESP32-S3", flash_size:"16MB", psram:"octal", image:{path:"test.bin", size:image.length, sha256:createHash("sha256").update(image).digest("hex")}, settings:{offset:64,size:64}}]};

async function board(page) {
  await page.route("**/firmware/manifest.json", route => route.fulfill({json:manifest}));
  await page.route("**/firmware/test.bin", route => route.fulfill({body:image}));
  await page.addInitScript(() => {
    window.boardSettings = {};
    window.boardPaired = false;
    let controller;
    const encoder = new TextEncoder();
    const port = {
      async open() {
        this.readable = new ReadableStream({start(c) {controller=c;}});
        this.writable = new WritableStream({write(bytes) {
          const line = new TextDecoder().decode(bytes).trim();
          let reply;
          if (line.startsWith("set ")) {
            const [,key,value] = /^set (\w+) (.*)$/.exec(line);
            window.boardSettings[key] = JSON.parse(value);
            reply = `@ok ${key}`;
          } else if (line === "status") {
            reply = "@status " + JSON.stringify({device_id:"hg-test", board:"esp32s3-touch-amoled-175", firmware:"test", network:true, paired:window.boardPaired, phase:window.boardPaired ? "online" : "pairing", pairing_code:window.boardPaired ? "" : "ABC123"});
          } else if (line === "diag") {
            reply = '@diag {"device_id":"hg-test","firmware":"test"}';
          } else if (line === "diag log") {
            reply = "test board log\n@log end";
          }
          controller.enqueue(encoder.encode(`gadget>${line}\n${reply}\n`));
        }});
      },
      async close() {}, async setSignals() {}, getInfo() {return {usbVendorId:123};},
    };
    Object.defineProperty(navigator,"serial",{value:{async requestPort(){return port;}, async getPorts(){return [port];}, addEventListener(){}}});
  });
  await page.route("**/lib/flasher.js", route => route.fulfill({contentType:"text/javascript", body:`
    export async function connect() { return {loader:{}, transport:{async disconnect(){}}, chip:{name:"ESP32-S3",flashSize:"16MB",psram:8,features:["Embedded PSRAM 8MB (AP_3v3)"]}}; }
    export async function write(loader,image,parts,progress) { window.writtenParts=parts; progress(1); }
    export async function restart() {}
  `}));
}

test("installer exposes the release's firmware license archive", async ({page}) => {
  await page.route("**/firmware/manifest.json", route => route.fulfill({
    json: {...manifest, licenses: {path: "hermes-gadget-test-licenses.zip"}},
  }));
  await page.goto("/installer.html");
  const licenses = page.getByRole("link", {name: "Firmware licenses"});
  await expect(licenses).toBeVisible();
  await expect(licenses).toHaveAttribute("href", "firmware/hermes-gadget-test-licenses.zip");
});

test("board setup prepares Hermes, installs with settings preserved, and pairs", async ({page, context}) => {
  await board(page);
  await context.grantPermissions(["clipboard-read","clipboard-write"]);
  await page.goto("/installer.html#server=ws%3A%2F%2F192.168.1.20%3A8765%2Fgadget");
  await page.getByText("Waveshare ESP32-S3-Touch-AMOLED-1.75",{exact:true}).click();
  await page.getByRole("button",{name:/Continue to Hermes/}).click();
  await expect(page.getByRole("heading",{name:"Prepare your Hermes"})).toBeVisible();
  await page.getByRole("button",{name:"Copy",exact:true}).nth(1).click();
  expect(await page.evaluate(()=>navigator.clipboard.readText())).toBe("hermes gateway setup");
  await page.getByRole("button",{name:"Back",exact:true}).click();
  await expect(page.getByRole("radio")).toBeChecked();
  await page.getByRole("button",{name:/Continue to Hermes/}).click();
  await page.getByRole("button",{name:/Hermes is ready/}).click();
  await page.getByRole("button",{name:"Connect and install"}).click();
  await expect(page.getByRole("heading",{name:"Connect it to Wi-Fi and Hermes"})).toBeVisible();
  expect(await page.evaluate(()=>window.writtenParts)).toEqual([{start:0,end:64},{start:128,end:512}]);
  await page.getByLabel("Wi-Fi network",{exact:true}).fill("Home WiFi");
  await page.getByLabel("Wi-Fi password",{exact:true}).fill("test-secret");
  await page.getByLabel("Device name optional",{exact:true}).fill("Kitchen Shelf");
  await page.getByRole("button",{name:"Save to the board"}).click();
  await expect(page.locator("#pair-code")).toHaveText("ABC123");
  expect(await page.evaluate(()=>window.boardSettings)).toEqual({wifi_ssid:"Home WiFi",wifi_pass:"test-secret",server:"ws://192.168.1.20:8765/gadget",name:"Kitchen Shelf"});
  await page.evaluate(()=>{window.boardPaired=true;});
  await expect(page.locator("#pair-done")).toBeVisible();
  await expect(page.locator("#update-command")).toHaveText('hermes gadget update "Kitchen Shelf" --latest');
  await expect(page.locator("#log")).not.toContainText("test-secret");
  await page.getByRole("button",{name:"Edit connection settings"}).click();
  await expect(page.getByLabel("Wi-Fi network",{exact:true})).toHaveValue("Home WiFi");
});

test("an existing gadget can skip flashing, validate settings, and save diagnostics", async ({page}) => {
  await board(page);
  await page.goto("/installer.html");
  await page.getByRole("button",{name:/Manage an existing gadget/}).click();
  await page.getByRole("button",{name:"Save to the board"}).click();
  await expect(page.getByRole("alert")).toHaveText("Enter the name of your Wi-Fi network.");
  await page.getByText("Something else",{exact:true}).click();
  const download = page.waitForEvent("download");
  await page.getByRole("button",{name:"Save a diagnostics report"}).click();
  expect((await download).suggestedFilename()).toMatch(/^hermes-gadget-diag-hg-test-/);
  await expect(page.locator("#diag-status")).toContainText("Attach it to your issue");
});

test("unsupported browsers cannot start USB operations", async ({page}) => {
  await page.addInitScript(()=>{delete Navigator.prototype.serial;});
  await page.route("**/firmware/manifest.json",route=>route.fulfill({json:manifest}));
  await page.goto("/installer.html");
  await expect(page.getByText("This browser can't talk to USB devices.",{exact:true})).toBeVisible();
  await expect(page.getByRole("radio")).toBeDisabled();
  await expect(page.getByRole("button",{name:/Manage an existing gadget/})).toBeDisabled();
});
