# Sura Hosted Registry Service

This folder contains deployable service artifacts for the Sura HTTP registry API:

- `Dockerfile` runs `tools/sura_registry_api.js` as an unprivileged Node service.
- `docker-compose.yml` adds restart policy, persistent storage, token configuration, and health checks.
- `systemd/sura-registry.service` runs the same API as an always-on Linux service.
- `sura-registry.env.example` lists the required production environment variables.

Required environment:

```text
SURA_REGISTRY=/var/lib/sura-registry
SURA_REGISTRY_HOST=0.0.0.0
PORT=8765
SURA_REGISTRY_TOKEN=<publisher token>
SURA_REGISTRY_ADMIN_TOKEN=<admin token>
```

The service exposes `/health` with schema `sura.registry.health_endpoint.v1`, package counts, advisory counts, report counts, per-status abuse queue counts, open report counts, and uptime. The same process also serves publish/install metadata, browser package pages, analytics, security advisories, abuse reports, admin report review queues, moderation audit events in `moderation-log.jsonl`, and actioned-report yanks.

Abuse review queue:

```text
POST /api/report
GET /api/reports?status=open&limit=100        # admin token
POST /api/reports/review                     # admin token; status=open|reviewing|dismissed|actioned
```

When a reviewed report is marked `actioned` with `{"yank":true}`, the registry updates `yanks.json`, rebuilds `index.json`, and records a `sura.registry.moderation_event.v1` line for the action.

Local smoke:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\sura_registry_service_smoke.ps1 -Surapkg .\surapkg.exe
```

Operational gate:

```powershell
$env:SURA_REGISTRY_URL="https://registry.example.com"
$env:SURA_REGISTRY_TOKEN="<admin token>"
.\surapkg.exe registry-health --fail-on-warning --json registry-health.json
```
