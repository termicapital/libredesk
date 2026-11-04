import { createVercelHealthHandler } from "../bridge/src/vercel/adapter.js";
import { getDefaultStoredBridge } from "../bridge/src/vercel/default.js";

export default createVercelHealthHandler(getDefaultStoredBridge);
