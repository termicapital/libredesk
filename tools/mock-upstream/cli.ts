import { createMockUpstream, MOCK_SCENARIOS, type MockScenario } from "./server.js";

const args = process.argv.slice(2);

void main();

async function main(): Promise<void> {
  const scenario = option("--scenario") ?? "normal";
  const port = Number(option("--port") ?? "8787");
  const nowOption = option("--now");
  const now = nowOption ? new Date(nowOption) : new Date();

  if (!MOCK_SCENARIOS.includes(scenario as MockScenario)) {
    console.error(`Unknown scenario: ${scenario}`);
    console.error(`Choose one of: ${MOCK_SCENARIOS.join(", ")}`);
    process.exitCode = 2;
  } else if (!Number.isInteger(port) || port < 0 || port > 65_535) {
    console.error(`Invalid port: ${port}`);
    process.exitCode = 2;
  } else if (!Number.isFinite(now.getTime())) {
    console.error("Invalid --now timestamp");
    process.exitCode = 2;
  } else {
    const mock = createMockUpstream({ scenario: scenario as MockScenario, port, now });
    await mock.ready;
    console.log(JSON.stringify({ url: mock.url, scenario }));

    const shutdown = () => {
      void mock.close().then(() => process.exit(0));
    };
    process.once("SIGINT", shutdown);
    process.once("SIGTERM", shutdown);
  }
}

function option(name: string): string | undefined {
  const index = args.indexOf(name);
  return index >= 0 ? args[index + 1] : undefined;
}
