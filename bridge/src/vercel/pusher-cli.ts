import {
  PusherConfigError,
  PusherError,
  loadSecurePusherConfig,
  pushLatestReading,
  type PusherConfig,
} from "./pusher.js";

export const DEFAULT_PUSHER_CONFIG_PATH = "/etc/libre-desk/vercel-push.json";

export interface PusherCliDependencies {
  env?: Readonly<Record<string, string | undefined>>;
  loadConfig?: (filePath: string) => Promise<PusherConfig>;
  push?: (config: PusherConfig) => Promise<{ result: "stored" | "duplicate" }>;
  writeError?: (message: string) => void;
}

export async function runPusherCli(dependencies: PusherCliDependencies = {}): Promise<number> {
  const env = dependencies.env ?? process.env;
  const loadConfig = dependencies.loadConfig ?? loadSecurePusherConfig;
  const push = dependencies.push ?? ((config) => pushLatestReading(config));
  const writeError = dependencies.writeError ?? ((message) => process.stderr.write(message));
  const filePath = env.LIBRE_DESK_VERCEL_PUSH_CONFIG ?? DEFAULT_PUSHER_CONFIG_PATH;

  try {
    const config = await loadConfig(filePath);
    await push(config);
    return 0;
  } catch (error) {
    const category = error instanceof PusherConfigError
      ? "configuration invalid"
      : error instanceof PusherError
        ? "push unavailable"
        : "internal error";
    writeError(`Libre Desk Vercel push failed: ${category}\n`);
    return 1;
  }
}
