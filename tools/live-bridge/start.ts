import { readFile } from "node:fs/promises";
import { loadBridgeConfig } from "../../bridge/src/config.js";
import { createRedactingLogger } from "../../bridge/src/obs/logger.js";
import { createBridgeRuntime } from "../../bridge/src/runtime.js";

interface StoredCredentials {
  libre_email: string;
  libre_password: string;
}
interface DeviceConfig {
  device_token_sha256: string;
  bridge_port: number;
}

const secureDirectory = process.env.LIBRE_DESK_SECURE_DIR;
if (!secureDirectory) throw new Error("set LIBRE_DESK_SECURE_DIR to your external private configuration directory");
const credentials = JSON.parse(await readFile(`${secureDirectory}/credentials.json`, "utf8")) as StoredCredentials;
const device = JSON.parse(await readFile(`${secureDirectory}/device.json`, "utf8")) as DeviceConfig;
const config = loadBridgeConfig({
  HOST: "127.0.0.1",
  PORT: String(device.bridge_port),
  BRIDGE_VERSION: "0.1.0-mvp",
  LIBRE_EMAIL: credentials.libre_email,
  LIBRE_PASSWORD: credentials.libre_password,
  LLU_PRODUCT: "llu.android",
  LLU_APP_VERSION: "5.1.1",
  UPSTREAM_SOURCE: "librelinkup",
  UPSTREAM_BASE_URL: "https://api.libreview.io",
  UPSTREAM_TIMEOUT_MS: "15000",
  DEVICE_TOKEN_HASHES: device.device_token_sha256,
  POLL_INTERVAL_S: "20",
  POLL_JITTER_S: "0",
  MAX_BACKOFF_S: "900",
  MAX_AGE_S: "3600",
  RANGE_URGENT_LOW: "70",
  FIRMWARE_DIR: "/tmp/libre-desk-mvp-no-ota",
});
const logger = createRedactingLogger();
const runtime = createBridgeRuntime(config, { logger });
let stopping = false;
async function stop(signal: string): Promise<void> {
  if (stopping) return;
  stopping = true;
  logger.info("bridge_stopping", { signal });
  await runtime.stop();
}
process.once("SIGTERM", () => { void stop("SIGTERM"); });
process.once("SIGINT", () => { void stop("SIGINT"); });
await runtime.start();
