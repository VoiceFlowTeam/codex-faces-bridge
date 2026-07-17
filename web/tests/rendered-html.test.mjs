import assert from "node:assert/strict";
import { access, readFile } from "node:fs/promises";
import test from "node:test";

async function render() {
  const workerUrl = new URL("../dist/server/index.js", import.meta.url);
  workerUrl.searchParams.set("test", `${process.pid}-${Date.now()}`);
  const { default: worker } = await import(workerUrl.href);

  return worker.fetch(
    new Request("https://codex-micro.test/", {
      headers: { accept: "text/html", host: "codex-micro.test" },
    }),
    {
      ASSETS: {
        fetch: async () => new Response("Not found", { status: 404 }),
      },
    },
    {
      waitUntil() {},
      passThroughOnException() {},
    },
  );
}

test("server-renders the Codex Micro simulator", async () => {
  const response = await render();
  assert.equal(response.status, 200);
  assert.match(response.headers.get("content-type") ?? "", /^text\/html\b/i);

  const html = await response.text();
  assert.match(html, /<title>Codex Micro — Interactive Web Simulator<\/title>/i);
  assert.match(html, /Codex Micro interactive simulator/i);
  assert.match(html, /aria-label="Reasoning effort"/i);
  assert.match(html, /aria-label="Start recording"/i);
  assert.match(html, /og:image/i);
  assert.doesNotMatch(html, /codex-preview|Your site is taking shape|react-loading-skeleton/i);
});

test("ships production assets and removes the starter preview", async () => {
  const [page, layout, css, packageJson] = await Promise.all([
    readFile(new URL("../app/page.tsx", import.meta.url), "utf8"),
    readFile(new URL("../app/layout.tsx", import.meta.url), "utf8"),
    readFile(new URL("../app/globals.css", import.meta.url), "utf8"),
    readFile(new URL("../package.json", import.meta.url), "utf8"),
  ]);

  assert.match(page, /RotaryDial/);
  assert.match(page, /THREAD_COLORS/);
  assert.match(page, /aria-live/);
  assert.match(page, /aria-valuenow/);
  assert.doesNotMatch(page, /mic-status/);
  assert.match(page, /mic-glyph/);
  assert.match(page, /mic-capsule/);
  assert.match(page, /mic-yoke/);
  assert.doesNotMatch(page, /sound-waves/);
  assert.match(page, /agent-glyph/);
  assert.doesNotMatch(page, /<Bot\s*\/>/);
  assert.match(css, /prefers-reduced-motion/);
  assert.match(layout, /og\.png/);
  assert.doesNotMatch(packageJson, /react-loading-skeleton/);
  await access(new URL("../public/og.png", import.meta.url));
  await assert.rejects(access(new URL("../app\/_sites-preview", import.meta.url)));
});
