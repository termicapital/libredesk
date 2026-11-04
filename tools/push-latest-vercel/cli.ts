import { runPusherCli } from "../../bridge/src/vercel/pusher-cli.js";

process.exitCode = await runPusherCli();
