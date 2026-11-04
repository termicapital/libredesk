import { mkdtemp, readdir, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import path from "node:path";
import { afterEach, describe, expect, it } from "vitest";
import { createRedactingLogger } from "../bridge/src/obs/logger.js";
import { persistCoredump, readOtaManifest } from "../bridge/src/runtime.js";

let directory: string | undefined;
afterEach(async () => {
  if (directory) await rm(directory, { recursive: true, force: true });
  directory = undefined;
});

async function writeManifest(url: string): Promise<void> {
  directory = directory ?? await mkdtemp(path.join(tmpdir(), "libre-desk-ota-"));
  await writeFile(path.join(directory, "manifest.json"), JSON.stringify({
    version: "0.2.0",
    url,
    sha256: "a".repeat(64),
    min_prev_version: "0.1.0",
  }));
}

describe("runtime artifact safety", () => {
  it.each([
    "http://downloads.example.test/libre.bin",
    "file:///tmp/libre.bin",
    "not a URL",
    "https://user:password@downloads.example.test/libre.bin",
  ])("rejects unsafe OTA URL %s", async (url) => {
    await writeManifest(url);
    expect(readOtaManifest(directory as string)).toBeUndefined();
  });

  it("accepts only an absolute credential-free HTTPS OTA URL", async () => {
    await writeManifest("https://downloads.example.test/releases/libre-0.2.0.bin");
    expect(readOtaManifest(directory as string)).toMatchObject({ version: "0.2.0" });
  });

  it("retains only the three newest coredumps", async () => {
    directory = await mkdtemp(path.join(tmpdir(), "libre-desk-coredump-retention-"));
    const logger = createRedactingLogger(() => undefined);
    for (let index = 0; index < 5; index += 1) {
      await persistCoredump(directory, {
        firmware_version: "0.1.0",
        reset_reason: "panic",
        data_base64: Buffer.from(`dump-${index}`).toString("base64"),
      }, new Date(1_800_000_000_000 + index), logger);
    }
    expect(await readdir(directory)).toHaveLength(3);
  });
});
