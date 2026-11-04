import { readFile } from "node:fs/promises";
import { LatestCache } from "../../bridge/src/cache.js";
import { Poller } from "../../bridge/src/poller.js";
import { LibreLinkUpClient } from "../../bridge/src/upstream/client.js";

interface StoredCredentials {
  libre_email: string;
  libre_password: string;
}

const credentialPath = process.env.LIBRE_DESK_CREDENTIAL_FILE;
if (!credentialPath) throw new Error("set LIBRE_DESK_CREDENTIAL_FILE to your external follower credential file");
const credentials = JSON.parse(await readFile(credentialPath, "utf8")) as StoredCredentials;
if (!credentials.libre_email || !credentials.libre_password) {
  throw new Error("credential file is missing follower fields");
}

const client = new LibreLinkUpClient({
  baseUrl: process.env.UPSTREAM_BASE_URL ?? "https://api.libreview.io",
  email: credentials.libre_email,
  password: credentials.libre_password,
  product: "llu.android",
  version: process.env.LLU_APP_VERSION ?? "4.16.0",
  timeoutMs: 15_000,
});
const cache = new LatestCache();
const poller = new Poller(client, cache, {
  maxAgeS: 3_600,
  urgentLowMgDl: 70,
  pollAfterS: 60,
  source: "librelinkup",
});

const result = await poller.pollOnce(new Date());
if (result.status === "success") {
  console.log(JSON.stringify({
    status: "success",
    source: result.reading.source,
    unit: result.reading.unit,
    reading_fresh: result.reading.age_s <= 600,
    age_under_one_hour: result.reading.age_s <= 3_600,
    trend_available: result.reading.trend !== "unknown",
    range_available: result.reading.range !== "unknown",
  }));
} else {
  console.log(JSON.stringify(result));
  process.exitCode = 2;
}
