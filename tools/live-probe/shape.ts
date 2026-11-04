import { readFile } from "node:fs/promises";
import { LibreLinkUpClient } from "../../bridge/src/upstream/client.js";

interface StoredCredentials { libre_email: string; libre_password: string }
function record(value: unknown): Record<string, unknown> | undefined {
  return typeof value === "object" && value !== null && !Array.isArray(value)
    ? value as Record<string, unknown>
    : undefined;
}

const path = process.env.LIBRE_DESK_CREDENTIAL_FILE;
if (!path) throw new Error("set LIBRE_DESK_CREDENTIAL_FILE to your external follower credential file");
const credentials = JSON.parse(await readFile(path, "utf8")) as StoredCredentials;
const client = new LibreLinkUpClient({
  baseUrl: "https://api.libreview.io",
  email: credentials.libre_email,
  password: credentials.libre_password,
  product: "llu.android",
  version: process.env.LLU_APP_VERSION ?? "5.1.1",
  timeoutMs: 15_000,
});
const connections = await client.getConnections();
if (connections.length !== 1 || typeof connections[0]?.patientId !== "string") {
  throw new Error("expected exactly one follower connection");
}
const graph = await client.getGraph(connections[0].patientId);
const data = record(graph.data);
const connection = record(data?.connection);
const measurement = record(connection?.glucoseMeasurement);
if (!connection || !measurement) throw new Error("live graph omitted the connection measurement");
console.log(JSON.stringify({
  connection_count: connections.length,
  measurement_fields: Object.keys(measurement).sort(),
  glucose_units: measurement.GlucoseUnits ?? null,
  glucose_units_type: typeof measurement.GlucoseUnits,
  value_mg_dl_type: typeof measurement.ValueInMgPerDl,
  factory_timestamp_type: typeof measurement.FactoryTimestamp,
  trend_arrow_type: typeof measurement.TrendArrow,
  target_low_type: typeof connection.targetLow,
  target_high_type: typeof connection.targetHigh,
}));
