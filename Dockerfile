FROM --platform=$BUILDPLATFORM node:22-bookworm-slim AS build

WORKDIR /app
COPY package.json package-lock.json ./
RUN npm ci

COPY . .
RUN npm run build

FROM node:22-bookworm-slim AS runtime

WORKDIR /app
COPY package.json package-lock.json ./
RUN npm ci && npm cache clean --force

COPY --from=build /app/dist ./dist

ENV NODE_ENV=production \
    WRANGLER_SEND_METRICS=false \
    WRANGLER_WRITE_LOGS=false \
    WRANGLER_LOG_PATH=/tmp/inkloop-wrangler.log

EXPOSE 3000
VOLUME ["/data"]

HEALTHCHECK --interval=30s --timeout=8s --start-period=30s --retries=3 \
  CMD ["node", "-e", "fetch('http://127.0.0.1:3000/api/health').then(r=>{if(!r.ok)process.exit(1)}).catch(()=>process.exit(1))"]

CMD ["./node_modules/.bin/wrangler", "dev", "--config", "dist/server/wrangler.json", "--env-file", "/app/.dev.vars", "--ip", "0.0.0.0", "--port", "3000", "--persist-to", "/data", "--log-level", "info", "--show-interactive-dev-session=false"]
