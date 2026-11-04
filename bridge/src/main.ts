import { loadBridgeConfig } from "./config.js";
import { createRedactingLogger } from "./obs/logger.js";
import { createBridgeRuntime } from "./runtime.js";

const logger = createRedactingLogger();

async function main(): Promise<void> {
  const config = loadBridgeConfig();
  const runtime = createBridgeRuntime(config, { logger });
  let stopping = false;
  const stop = async (signal: string): Promise<void> => {
    if (stopping) return;
    stopping = true;
    logger.info("bridge_stopping", { signal });
    await runtime.stop();
  };

  process.once("SIGTERM", () => { void stop("SIGTERM"); });
  process.once("SIGINT", () => { void stop("SIGINT"); });
  await runtime.start();
}

main().catch((error: unknown) => {
  logger.error("bridge_start_failed", { error });
  process.exitCode = 1;
});
