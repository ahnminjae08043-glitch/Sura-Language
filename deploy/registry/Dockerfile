FROM node:20-alpine

WORKDIR /app
COPY tools/sura_registry_api.js /app/tools/sura_registry_api.js

ENV NODE_ENV=production
ENV SURA_REGISTRY=/var/lib/sura-registry
ENV SURA_REGISTRY_HOST=0.0.0.0
ENV PORT=8765

RUN mkdir -p /var/lib/sura-registry && addgroup -S sura && adduser -S sura -G sura \
  && chown -R sura:sura /app /var/lib/sura-registry

USER sura
VOLUME ["/var/lib/sura-registry"]
EXPOSE 8765

HEALTHCHECK --interval=30s --timeout=5s --start-period=10s --retries=3 \
  CMD node -e "fetch('http://127.0.0.1:' + (process.env.PORT || 8765) + '/health').then(r => r.json()).then(j => process.exit(j.ok ? 0 : 1)).catch(() => process.exit(1))"

CMD ["node", "/app/tools/sura_registry_api.js"]
