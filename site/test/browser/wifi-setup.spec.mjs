import fs from "node:fs/promises";
import {test, expect} from "@playwright/test";

const html = await fs.readFile(new URL("../../../firmware/esp32/main/wifi_setup.html", import.meta.url), "utf8");

test("phone setup sends the entered credentials and shows a successful connection", async ({page}) => {
  let posted;
  await page.route("http://192.168.4.1/**", async route => {
    if (route.request().url().endsWith("/api/setup")) {
      if (route.request().method() === "POST") posted = route.request().postDataJSON();
      await route.fulfill({json:{nonce:"session-nonce",ssid:"",server:"ws://hermes.local:8765/gadget",state:posted ? "connected" : "ready"}});
    } else await route.fulfill({contentType:"text/html",body:html});
  });
  await page.setViewportSize({width:390,height:844});
  await page.goto("http://192.168.4.1/");
  await page.getByLabel("Wi-Fi network name").fill("Kitchen network");
  await page.getByLabel("Wi-Fi password", {exact:true}).fill("a private password");
  await page.getByRole("button", {name:"Check connection and save"}).click();
  await expect(page.getByRole("status")).toContainText("Wi-Fi connected and settings saved");
  expect(posted).toEqual({nonce:"session-nonce",ssid:"Kitchen network",password:"a private password",server:"ws://hermes.local:8765/gadget"});
  await expect(page.getByLabel("Wi-Fi password", {exact:true})).toHaveValue("");
  expect(await page.evaluate(() => document.documentElement.scrollWidth)).toBe(390);
});

test("phone setup leaves the form available after a failed connection", async ({page}) => {
  let posted = false;
  await page.route("http://192.168.4.1/**", async route => {
    if (route.request().url().endsWith("/api/setup")) {
      if (route.request().method() === "POST") posted = true;
      await route.fulfill({json:{nonce:"session-nonce",ssid:"Kitchen",server:"ws://host/gadget",state:posted ? "failed" : "ready"}});
    } else await route.fulfill({contentType:"text/html",body:html});
  });
  await page.goto("http://192.168.4.1/");
  await page.getByRole("button", {name:"Check connection and save"}).click();
  await expect(page.getByRole("status")).toContainText("Previous settings were kept");
  await expect(page.getByRole("button", {name:"Check connection and save"})).toBeEnabled();
});
