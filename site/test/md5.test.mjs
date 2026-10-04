import assert from "node:assert/strict";
import { createHash, randomBytes } from "node:crypto";
import { test } from "node:test";

import { md5 } from "../src/lib/md5.js";

const reference = (bytes) => createHash("md5").update(bytes).digest("hex");

test("matches RFC 1321's test suite", () => {
  const vectors = {
    "": "d41d8cd98f00b204e9800998ecf8427e",
    a: "0cc175b9c0f1b6a831c399e269772661",
    abc: "900150983cd24fb0d6963f7d28e17f72",
    "message digest": "f96b697d7cb7938d525a2f31aaf161d0",
    "12345678901234567890123456789012345678901234567890123456789012345678901234567890":
      "57edf4a22be3c955ac49da2e2107b67a",
  };
  for (const [text, digest] of Object.entries(vectors)) assert.equal(md5(new TextEncoder().encode(text)), digest);
});

test("matches Node's MD5 at every padding boundary and for a firmware-sized image", () => {
  for (let length = 0; length <= 200; length++) {
    const bytes = new Uint8Array(randomBytes(length));
    assert.equal(md5(bytes), reference(bytes), `length ${length}`);
  }
  const image = new Uint8Array(randomBytes(1_300_000));
  assert.equal(md5(image), reference(image));
  assert.equal(md5(image.subarray(36864, 1_000_000)), reference(image.subarray(36864, 1_000_000)));
});
