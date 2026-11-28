import { cloudflareTest } from "@cloudflare/vitest-plugin";
import { defineConfig } from "vitest/config";

export default defineConfig({
	plugins: [
		cloudflareTest({
			wrangler: { configPath: "./wrangler.jsonc" },
			miniflare: {
				bindings: {
					PUBLISH_TOKEN: "test-publish-token",
					TELEGRAM_BOT_TOKEN: "test-bot-token",
					TELEGRAM_CHAT_ID: "42",
					TELEGRAM_WEBHOOK_SECRET: "test-hook-secret",
				},
			},
		}),
	],
});
