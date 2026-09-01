#!/usr/bin/env node
"use strict";

const http = require("http");
const fs = require("fs");
const path = require("path");
const crypto = require("crypto");

function arg(name, fallback) {
  const idx = process.argv.indexOf(name);
  return idx >= 0 && idx + 1 < process.argv.length ? process.argv[idx + 1] : fallback;
}

const root = path.resolve(arg("--root", process.env.SURA_REGISTRY || path.join(process.cwd(), "registry")));
const port = Number(arg("--port", process.env.PORT || "8765"));
const host = arg("--host", process.env.SURA_REGISTRY_HOST || process.env.HOST || "0.0.0.0");
const token = arg("--token", process.env.SURA_REGISTRY_TOKEN || "dev-token");
const adminToken = arg("--admin-token", process.env.SURA_REGISTRY_ADMIN_TOKEN || token);
const statsPath = path.join(root, "stats.json");
const tokensPath = path.join(root, "tokens.json");
const ownersPath = path.join(root, "owners.json");
const yanksPath = path.join(root, "yanks.json");
const reportsPath = path.join(root, "reports.json");
const advisoriesPath = path.join(root, "advisories.json");
const moderationLogPath = path.join(root, "moderation-log.jsonl");
const keysPath = path.join(root, "keys");

function ensureDir(dir) {
  fs.mkdirSync(dir, { recursive: true });
}

function readJson(file, fallback) {
  try {
    return JSON.parse(fs.readFileSync(file, "utf8"));
  } catch {
    return fallback;
  }
}

function writeJson(file, data) {
  ensureDir(path.dirname(file));
  fs.writeFileSync(file, JSON.stringify(data, null, 2) + "\n");
}

function appendJsonLine(file, data) {
  ensureDir(path.dirname(file));
  fs.appendFileSync(file, JSON.stringify(data) + "\n", "utf8");
}

function hashText(text) {
  return crypto.createHash("sha256").update(text).digest("hex");
}

function tokenHash(value) {
  return hashText(`sura-registry-token-v1:${value}`);
}

function cleanName(value) {
  if (typeof value !== "string" || !/^[A-Za-z0-9_][A-Za-z0-9_.-]*$/.test(value)) return null;
  return value.replace(/[- ]/g, "_");
}

function cleanUser(value) {
  if (typeof value !== "string" || !/^[A-Za-z0-9_][A-Za-z0-9_.-]{1,63}$/.test(value)) return null;
  return value;
}

function cleanVersion(value) {
  if (typeof value !== "string" || !/^[A-Za-z0-9_.+-]+$/.test(value)) return null;
  return value;
}

function cleanKeyId(value) {
  if (typeof value !== "string" || !/^[A-Za-z0-9_.-]{1,96}$/.test(value)) return null;
  return value;
}

function safeJoin(base, rel) {
  const normalized = path.normalize(rel).replace(/^(\.\.(\/|\\|$))+/, "");
  const full = path.resolve(base, normalized);
  if (!full.startsWith(path.resolve(base) + path.sep) && full !== path.resolve(base)) return null;
  return full;
}

function loadStats() {
  return readJson(statsPath, { downloads: {}, publishes: {}, downloadDays: {}, publishDays: {}, lastPublish: null });
}

function loadTokens() {
  return readJson(tokensPath, { tokens: {} });
}

function saveTokens(tokens) {
  writeJson(tokensPath, tokens);
}

function loadOwners() {
  return readJson(ownersPath, { packages: {} });
}

function saveOwners(owners) {
  writeJson(ownersPath, owners);
}

function loadYanks() {
  return readJson(yanksPath, { yanked: {} });
}

function saveYanks(yanks) {
  writeJson(yanksPath, yanks);
}

function loadReports() {
  return readJson(reportsPath, { reports: [] });
}

function saveReports(reports) {
  writeJson(reportsPath, reports);
}

function appendModerationEvent(type, payload) {
  appendJsonLine(moderationLogPath, {
    schema: "sura.registry.moderation_event.v1",
    type,
    at: new Date().toISOString(),
    ...payload,
  });
}

function loadAdvisories() {
  return readJson(advisoriesPath, { advisories: [] });
}

function saveAdvisories(advisories) {
  writeJson(advisoriesPath, advisories);
}

function packageVersionKey(name, version) {
  return `${name}@${version}`;
}

function saveStats(stats) {
  writeJson(statsPath, stats);
}

function utcDay() {
  return new Date().toISOString().slice(0, 10);
}

function incrementCounter(map, key) {
  map[key] = (map[key] || 0) + 1;
}

function incrementDailyCounter(rootMap, day, key) {
  if (!rootMap[day]) rootMap[day] = {};
  incrementCounter(rootMap[day], key);
}

function recordDownload(name, version) {
  const stats = loadStats();
  const key = `${name}@${version}`;
  if (!stats.downloads) stats.downloads = {};
  if (!stats.downloadDays) stats.downloadDays = {};
  incrementCounter(stats.downloads, key);
  incrementDailyCounter(stats.downloadDays, utcDay(), key);
  saveStats(stats);
}

function recordPublish(name, version, user) {
  const stats = loadStats();
  const key = `${name}@${version}`;
  if (!stats.publishes) stats.publishes = {};
  if (!stats.publishDays) stats.publishDays = {};
  incrementCounter(stats.publishes, key);
  incrementDailyCounter(stats.publishDays, utcDay(), key);
  stats.lastPublish = { name, version, user, at: new Date().toISOString() };
  saveStats(stats);
}

function topEntries(map, limit = 20) {
  return Object.entries(map || {})
    .sort((a, b) => b[1] - a[1] || a[0].localeCompare(b[0]))
    .slice(0, limit)
    .map(([packageVersion, count]) => ({ packageVersion, count }));
}

function analyticsSummary() {
  const stats = loadStats();
  return {
    downloads: stats.downloads || {},
    publishes: stats.publishes || {},
    downloadDays: stats.downloadDays || {},
    publishDays: stats.publishDays || {},
    topDownloads: topEntries(stats.downloads),
    topPublishes: topEntries(stats.publishes),
    lastPublish: stats.lastPublish || null,
  };
}

function rebuildIndex() {
  ensureDir(root);
  const packages = [];
  const owners = loadOwners();
  const yanks = loadYanks();
  for (const name of fs.readdirSync(root, { withFileTypes: true })) {
    if (!name.isDirectory()) continue;
    if (name.name === "latest") continue;
    const pkgRoot = path.join(root, name.name);
    for (const version of fs.readdirSync(pkgRoot, { withFileTypes: true })) {
      if (!version.isDirectory() || version.name === "latest") continue;
      const bundlePath = path.join(pkgRoot, version.name, "package.surabundle.json");
      if (!fs.existsSync(bundlePath)) continue;
      const bundle = fs.readFileSync(bundlePath, "utf8");
      const yank = yanks.yanked?.[packageVersionKey(name.name, version.name)] || null;
      packages.push({
        name: name.name,
        version: version.name,
        bundle: `${name.name}/${version.name}/package.surabundle.json`,
        hash: hashText(bundle),
        owner: owners.packages?.[name.name]?.owner || null,
        yanked: !!yank,
        yankReason: yank?.reason || null,
        yankedAt: yank?.at || null,
      });
    }
  }
  const index = { packages, stats: loadStats(), owners: owners.packages || {}, yanks: yanks.yanked || {} };
  writeJson(path.join(root, "index.json"), index);
  return index;
}

function bundleFiles(bundle) {
  const files = {};
  if (!bundle || !Array.isArray(bundle.files)) return files;
  for (const file of bundle.files) {
    if (file && typeof file.path === "string" && typeof file.content === "string") {
      files[file.path] = file.content;
    }
  }
  return files;
}

function parseJsonText(text) {
  if (typeof text !== "string" || !text.trim()) return null;
  try {
    return JSON.parse(text);
  } catch {
    return null;
  }
}

function packageDetail(name, version) {
  const bundlePath = path.join(root, name, version || "latest", "package.surabundle.json");
  if (!fs.existsSync(bundlePath)) return null;
  const bundleText = fs.readFileSync(bundlePath, "utf8");
  const bundle = parseJsonText(bundleText);
  if (!bundle || !Array.isArray(bundle.files)) return null;

  const actualVersion = version === "latest" ? (cleanVersion(bundle.version) || "latest") : version;
  const yank = loadYanks().yanked?.[packageVersionKey(name, actualVersion)] || null;
  const files = bundleFiles(bundle);
  const manifest = parseJsonText(files["sura.pkg.json"]);
  const api = parseJsonText(files["docs/api.json"]);
  const searchIndex = parseJsonText(files["docs/search-index.json"]);
  const benchReportPath = typeof manifest?.bench_report === "string" ? manifest.bench_report : "";
  const benchmark = benchReportPath ? parseJsonText(files[benchReportPath]) : null;
  const auditReportPath = typeof manifest?.audit_report === "string" ? manifest.audit_report : "";
  const audit = auditReportPath ? parseJsonText(files[auditReportPath]) : null;
  const index = rebuildIndex();
  const indexEntry = index.packages.find((pkg) => pkg.name === name && pkg.version === actualVersion) || null;
  const owners = loadOwners();

  return {
    ok: true,
    name,
    version: actualVersion,
    owner: owners.packages?.[name]?.owner || null,
    yanked: !!yank,
    yankReason: yank?.reason || null,
    bundle: indexEntry?.bundle || `${name}/${actualVersion}/package.surabundle.json`,
    hash: indexEntry?.hash || hashText(bundleText),
    manifest,
    readme: files["README.md"] || files["readme.md"] || null,
    api,
    searchIndex,
    benchmark,
    audit,
    advisories: packageAdvisories(name, actualVersion),
  };
}

function advisoryAppliesTo(advisory, name, version = "") {
  if (!advisory || advisory.name !== name) return false;
  return !advisory.version || !version || advisory.version === version;
}

function packageAdvisories(name, version = "") {
  const store = loadAdvisories();
  const items = Array.isArray(store.advisories) ? store.advisories : [];
  return items.filter((item) => advisoryAppliesTo(item, name, version));
}

function filterAdvisories(url) {
  const queryName = cleanName(url.searchParams.get("name") || "") || "";
  const queryVersion = cleanVersion(url.searchParams.get("version") || "") || "";
  const querySeverity = String(url.searchParams.get("severity") || "").toLowerCase();
  const queryStatus = String(url.searchParams.get("status") || "").toLowerCase();
  const store = loadAdvisories();
  const items = Array.isArray(store.advisories) ? store.advisories : [];
  return items.filter((item) => {
    if (queryName && item.name !== queryName) return false;
    if (queryVersion && item.version && item.version !== queryVersion) return false;
    if (queryVersion && !item.version) return true;
    if (querySeverity && String(item.severity || "").toLowerCase() !== querySeverity) return false;
    if (queryStatus && String(item.status || "").toLowerCase() !== queryStatus) return false;
    return true;
  });
}

function htmlEscape(value) {
  return String(value ?? "")
    .replace(/&/g, "&amp;")
    .replace(/</g, "&lt;")
    .replace(/>/g, "&gt;")
    .replace(/"/g, "&quot;")
    .replace(/'/g, "&#39;");
}

function detailLink(name, version) {
  return `/api/package/${encodeURIComponent(name)}/${encodeURIComponent(version)}`;
}

function bundleLink(bundle) {
  return `/${String(bundle || "").split("/").map(encodeURIComponent).join("/")}`;
}

function packagePageLink(name, version) {
  return `/package/${encodeURIComponent(name)}/${encodeURIComponent(version)}`;
}

function jsonBlock(value) {
  if (value == null) return "";
  return htmlEscape(JSON.stringify(value, null, 2));
}

function benchmarkSummary(benchmark) {
  if (!benchmark || typeof benchmark !== "object") return "";
  const passed = benchmark.passed == null ? "" : `<span class="pill">${htmlEscape(benchmark.passed ? "passed" : "failed")}</span>`;
  const speedup = benchmark.speedup == null ? "" : `<span class="pill">speedup ${htmlEscape(benchmark.speedup)}x</span>`;
  const interpMs = benchmark.interpreter_ms == null ? "" : `<span class="pill">interp ${htmlEscape(benchmark.interpreter_ms)} ms</span>`;
  const jitMs = benchmark.jit_ms == null ? "" : `<span class="pill">jit ${htmlEscape(benchmark.jit_ms)} ms</span>`;
  return [passed, speedup, interpMs, jitMs].filter(Boolean).join("");
}

function auditSummary(audit) {
  if (!audit || typeof audit !== "object") return "";
  const passed = audit.passed == null ? "" : `<span class="pill ${audit.passed ? "good" : "bad"}">${htmlEscape(audit.passed ? "audit passed" : "audit failed")}</span>`;
  const findingCount = audit.finding_count ?? audit.findingCount;
  const findings = findingCount == null ? "" : `<span class="pill">${htmlEscape(findingCount)} finding${Number(findingCount) === 1 ? "" : "s"}</span>`;
  return [passed, findings].filter(Boolean).join("");
}

function packageDetailHtml(detail) {
  const title = `Sura package ${detail.name}@${detail.version}`;
  const symbols = Array.isArray(detail.api?.symbols) ? detail.api.symbols : [];
  const advisories = Array.isArray(detail.advisories) ? detail.advisories : [];
  const symbolRows = symbols.slice(0, 100).map((symbol) => {
    const kind = symbol?.kind || symbol?.type || "";
    const name = symbol?.name || "";
    const signature = symbol?.signature || symbol?.usage || "";
    const description = symbol?.description || symbol?.doc || "";
    return `<tr><td>${htmlEscape(kind)}</td><td><code>${htmlEscape(name)}</code></td><td><code>${htmlEscape(signature)}</code></td><td>${htmlEscape(description)}</td></tr>`;
  }).join("");
  const symbolSection = symbolRows
    ? `<table><thead><tr><th>Kind</th><th>Name</th><th>Signature</th><th>Description</th></tr></thead><tbody>${symbolRows}</tbody></table>`
    : `<p class="muted">No API symbol metadata was published for this package.</p>`;
  const readmeSection = detail.readme
    ? `<pre>${htmlEscape(detail.readme)}</pre>`
    : `<p class="muted">No README was published for this package.</p>`;
  const benchmarkPills = benchmarkSummary(detail.benchmark);
  const benchmarkSection = detail.benchmark
    ? `<p class="pills">${benchmarkPills}</p><pre>${jsonBlock(detail.benchmark)}</pre>`
    : `<p class="muted">No benchmark report was published for this package.</p>`;
  const auditPills = auditSummary(detail.audit);
  const auditFindings = Array.isArray(detail.audit?.findings) ? detail.audit.findings : [];
  const auditRows = auditFindings.slice(0, 50).map((finding) => {
    const source = finding.line ? `${finding.file || finding.source || ""}:${finding.line}` : (finding.file || finding.source || "");
    return `<tr><td><code>${htmlEscape(finding.kind || "")}</code></td><td>${htmlEscape(finding.message || "")}</td><td><small>${htmlEscape(source)}</small></td></tr>`;
  }).join("");
  const auditSection = detail.audit
    ? `<p class="pills">${auditPills}</p>${auditRows ? `<table><thead><tr><th>Kind</th><th>Message</th><th>Source</th></tr></thead><tbody>${auditRows}</tbody></table>` : `<pre>${jsonBlock(detail.audit)}</pre>`}`
    : `<p class="muted">No security audit report was published for this package.</p>`;
  const advisoryRows = advisories.map((advisory) => {
    const link = advisory.url ? `<a href="${htmlEscape(advisory.url)}">link</a>` : "";
    return `<tr><td><code>${htmlEscape(advisory.id)}</code></td><td>${htmlEscape(advisory.severity || "")}</td><td>${htmlEscape(advisory.status || "")}</td><td>${htmlEscape(advisory.title || "")}</td><td>${link}</td></tr>`;
  }).join("");
  const advisorySection = advisoryRows
    ? `<table><thead><tr><th>ID</th><th>Severity</th><th>Status</th><th>Title</th><th>Reference</th></tr></thead><tbody>${advisoryRows}</tbody></table>`
    : `<p class="muted">No security advisories were published for this package.</p>`;
  const status = detail.yanked
    ? `<span class="status bad">yanked${detail.yankReason ? `: ${htmlEscape(detail.yankReason)}` : ""}</span>`
    : `<span class="status good">active</span>`;
  return `<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>${htmlEscape(title)}</title>
  <style>
    :root { color-scheme: light dark; font-family: system-ui, -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif; }
    body { margin: 0; background: Canvas; color: CanvasText; }
    main { max-width: 1040px; margin: 0 auto; padding: 32px 20px 48px; }
    h1 { margin: 0 0 8px; font-size: 32px; line-height: 1.15; }
    h2 { margin-top: 32px; font-size: 20px; }
    a { color: LinkText; }
    .meta { display: grid; grid-template-columns: max-content 1fr; gap: 8px 16px; margin: 20px 0; }
    .meta dt { color: GrayText; }
    .meta dd { margin: 0; word-break: break-word; }
    .actions { display: flex; flex-wrap: wrap; gap: 10px; margin: 18px 0 26px; }
    .actions a { border: 1px solid color-mix(in srgb, CanvasText 22%, transparent); border-radius: 6px; padding: 8px 11px; text-decoration: none; }
    .status, .pill { display: inline-block; border-radius: 999px; padding: 2px 8px; font-size: 12px; border: 1px solid color-mix(in srgb, CanvasText 22%, transparent); }
    .good { color: #0a7a3d; }
    .bad { color: #b42318; }
    .muted { color: GrayText; }
    .pills { display: flex; flex-wrap: wrap; gap: 8px; }
    table { width: 100%; border-collapse: collapse; }
    th, td { border-bottom: 1px solid color-mix(in srgb, CanvasText 16%, transparent); padding: 8px; text-align: left; vertical-align: top; }
    pre { overflow: auto; padding: 14px; border-radius: 6px; background: color-mix(in srgb, CanvasText 8%, Canvas); }
    code { font-family: ui-monospace, SFMono-Regular, Consolas, "Liberation Mono", monospace; }
  </style>
</head>
<body>
  <main>
    <h1>${htmlEscape(detail.name)} <span class="muted">@${htmlEscape(detail.version)}</span></h1>
    ${status}
    <div class="actions">
      <a href="${htmlEscape(detailLink(detail.name, detail.version))}">JSON detail</a>
      <a href="${htmlEscape(bundleLink(detail.bundle))}">Download bundle</a>
    </div>
    <dl class="meta">
      <dt>Owner</dt><dd>${htmlEscape(detail.owner || "unclaimed")}</dd>
      <dt>Bundle</dt><dd><code>${htmlEscape(detail.bundle)}</code></dd>
      <dt>SHA-256</dt><dd><code>${htmlEscape(detail.hash)}</code></dd>
    </dl>
    <h2>API Symbols</h2>
    ${symbolSection}
    <h2>Benchmark</h2>
    ${benchmarkSection}
    <h2>Security Audit</h2>
    ${auditSection}
    <h2>Security Advisories</h2>
    ${advisorySection}
    <h2>README</h2>
    ${readmeSection}
    <h2>Manifest</h2>
    <pre>${jsonBlock(detail.manifest)}</pre>
  </main>
</body>
</html>`;
}

function packageMatchesQuery(pkg, query) {
  if (!query) return true;
  const haystack = `${pkg.name} ${pkg.version} ${pkg.owner || ""} ${pkg.bundle || ""}`.toLowerCase();
  return query.toLowerCase().split(/\s+/).filter(Boolean).every((part) => haystack.includes(part));
}

function registryIndexHtml(queryValue = "") {
  const query = String(queryValue || "").trim().slice(0, 120);
  const index = rebuildIndex();
  const allPackages = [...(index.packages || [])]
    .sort((a, b) => a.name.localeCompare(b.name) || a.version.localeCompare(b.version));
  const visiblePackages = allPackages.filter((pkg) => packageMatchesQuery(pkg, query));
  const activeCount = allPackages.filter((pkg) => !pkg.yanked).length;
  const yankedCount = allPackages.length - activeCount;
  const advisoryCount = filterAdvisories(new URL("http://localhost/api/advisories?status=active")).length;
  const rows = visiblePackages.map((pkg) => {
    const activeAdvisories = packageAdvisories(pkg.name, pkg.version).filter((item) => item.status === "active").length;
    const status = pkg.yanked
      ? `<span class="status bad">yanked${pkg.yankReason ? `: ${htmlEscape(pkg.yankReason)}` : ""}</span>`
      : `<span class="status good">active</span>`;
    return `<tr>
      <td><a href="${htmlEscape(packagePageLink(pkg.name, pkg.version))}"><code>${htmlEscape(pkg.name)}</code></a></td>
      <td><code>${htmlEscape(pkg.version)}</code></td>
      <td>${htmlEscape(pkg.owner || "unclaimed")}</td>
      <td>${status}</td>
      <td>${activeAdvisories ? `<span class="status bad">${htmlEscape(activeAdvisories)} active</span>` : `<span class="muted">none</span>`}</td>
      <td><a href="${htmlEscape(bundleLink(pkg.bundle))}">bundle</a></td>
      <td><a href="${htmlEscape(detailLink(pkg.name, pkg.version))}">json</a></td>
    </tr>`;
  }).join("");
  const packageTable = rows
    ? `<table><thead><tr><th>Package</th><th>Version</th><th>Owner</th><th>Status</th><th>Advisories</th><th>Bundle</th><th>API</th></tr></thead><tbody>${rows}</tbody></table>`
    : `<p class="muted">No packages match this search.</p>`;
  const heading = query ? `Packages matching "${query}"` : "Packages";
  return `<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Sura Registry</title>
  <style>
    :root { color-scheme: light dark; font-family: system-ui, -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif; }
    body { margin: 0; background: Canvas; color: CanvasText; }
    main { max-width: 1120px; margin: 0 auto; padding: 32px 20px 48px; }
    h1 { margin: 0 0 8px; font-size: 34px; line-height: 1.15; }
    h2 { margin-top: 28px; font-size: 20px; }
    a { color: LinkText; }
    form { display: flex; flex-wrap: wrap; gap: 10px; margin: 22px 0; }
    input { min-width: min(100%, 320px); flex: 1 1 320px; padding: 9px 11px; border-radius: 6px; border: 1px solid color-mix(in srgb, CanvasText 24%, transparent); background: Canvas; color: CanvasText; }
    button, .button { border: 1px solid color-mix(in srgb, CanvasText 24%, transparent); border-radius: 6px; padding: 9px 12px; background: Canvas; color: CanvasText; text-decoration: none; cursor: pointer; }
    .summary { display: flex; flex-wrap: wrap; gap: 10px; margin: 18px 0; }
    .pill, .status { display: inline-block; border-radius: 999px; padding: 2px 8px; font-size: 12px; border: 1px solid color-mix(in srgb, CanvasText 22%, transparent); }
    .good { color: #0a7a3d; }
    .bad { color: #b42318; }
    .muted { color: GrayText; }
    table { width: 100%; border-collapse: collapse; }
    th, td { border-bottom: 1px solid color-mix(in srgb, CanvasText 16%, transparent); padding: 8px; text-align: left; vertical-align: top; }
    code { font-family: ui-monospace, SFMono-Regular, Consolas, "Liberation Mono", monospace; }
  </style>
</head>
<body>
  <main>
    <h1>Sura Registry</h1>
    <p class="muted">Browse published Sura packages, package pages, bundle links, and registry JSON metadata.</p>
    <form action="/packages" method="get">
      <input name="q" value="${htmlEscape(query)}" placeholder="Search packages, versions, owners, bundles" aria-label="Search packages">
      <button type="submit">Search</button>
      <a class="button" href="/index.json">index.json</a>
    </form>
    <div class="summary">
      <span class="pill">${htmlEscape(allPackages.length)} total</span>
      <span class="pill">${htmlEscape(activeCount)} active</span>
      <span class="pill">${htmlEscape(yankedCount)} yanked</span>
      <span class="pill">${htmlEscape(advisoryCount)} active advisories</span>
      <span class="pill">${htmlEscape(visiblePackages.length)} shown</span>
    </div>
    <h2>${htmlEscape(heading)}</h2>
    ${packageTable}
  </main>
</body>
</html>`;
}

function send(res, status, body, type = "application/json") {
  const text = typeof body === "string" ? body : JSON.stringify(body, null, 2) + "\n";
  res.writeHead(status, {
    "content-type": `${type}; charset=utf-8`,
    "content-length": Buffer.byteLength(text),
  });
  res.end(text);
}

function authenticate(req) {
  const got = req.headers.authorization || "";
  if (!got.startsWith("Bearer ")) return null;
  const raw = got.slice("Bearer ".length);
  if (raw === adminToken) return { user: "admin", admin: true, source: "admin-token" };
  if (raw === token) return { user: "dev", admin: token === adminToken, source: "bootstrap-token" };
  const tokens = loadTokens();
  const record = tokens.tokens?.[tokenHash(raw)];
  if (!record || record.revokedAt) return null;
  return {
    user: record.user,
    admin: !!record.admin,
    source: "stored-token",
  };
}

function validSecret(value, min = 8, max = 256) {
  return typeof value === "string" &&
    value.length >= min &&
    value.length <= max &&
    !/[\r\n"]/g.test(value);
}

function ensureTokenStoreShape(store) {
  if (!store.tokens) store.tokens = {};
  if (!store.recovery) store.recovery = {};
}

function randomSecret(bytes = 24) {
  return crypto.randomBytes(bytes).toString("hex");
}

function issueToken(store, user, admin, rawToken, createdBy, extra = {}) {
  ensureTokenStoreShape(store);
  const now = new Date().toISOString();
  store.tokens[tokenHash(rawToken)] = {
    user,
    admin: !!admin,
    createdAt: now,
    createdBy,
    ...extra,
  };
  return now;
}

function issueRecoveryCode(store, user, admin, createdBy) {
  ensureTokenStoreShape(store);
  const recoveryCode = randomSecret(18);
  store.recovery[tokenHash(recoveryCode)] = {
    user,
    admin: !!admin,
    createdAt: new Date().toISOString(),
    createdBy,
  };
  return recoveryCode;
}

function revokeUserTokens(store, user, by) {
  ensureTokenStoreShape(store);
  const now = new Date().toISOString();
  let revoked = 0;
  for (const record of Object.values(store.tokens)) {
    if (record.user === user && !record.revokedAt) {
      record.revokedAt = now;
      record.revokedBy = by;
      ++revoked;
    }
  }
  return revoked;
}

function requireAuth(req, res) {
  const account = authenticate(req);
  if (!account) {
    send(res, 401, { error: "unauthorized" });
    return null;
  }
  return account;
}

function requireAdmin(req, res) {
  const account = requireAuth(req, res);
  if (!account) return null;
  if (!account.admin) {
    send(res, 403, { error: "admin token required" });
    return null;
  }
  return account;
}

function optionalAuth(req, res) {
  if (!req.headers.authorization) return null;
  return requireAuth(req, res);
}

function claimPackageOwner(name, account, res) {
  const owners = loadOwners();
  const existing = owners.packages[name];
  if (existing && existing.owner !== account.user && !account.admin) {
    send(res, 403, {
      error: `package '${name}' is owned by ${existing.owner}`,
      owner: existing.owner,
    });
    return null;
  }
  const now = new Date().toISOString();
  if (!existing) {
    owners.packages[name] = { owner: account.user, createdAt: now, updatedAt: now };
  } else {
    existing.updatedAt = now;
    existing.lastPublisher = account.user;
  }
  saveOwners(owners);
  return owners.packages[name].owner;
}

function readBody(req, limitBytes = 10 * 1024 * 1024) {
  return new Promise((resolve, reject) => {
    let size = 0;
    const chunks = [];
    req.on("data", (chunk) => {
      size += chunk.length;
      if (size > limitBytes) {
        reject(new Error("request body too large"));
        req.destroy();
        return;
      }
      chunks.push(chunk);
    });
    req.on("end", () => resolve(Buffer.concat(chunks).toString("utf8")));
    req.on("error", reject);
  });
}

async function handlePublish(req, res) {
  const account = requireAuth(req, res);
  if (!account) return;
  let bundleText;
  let bundle;
  try {
    bundleText = await readBody(req);
    bundle = JSON.parse(bundleText);
  } catch (err) {
    send(res, 400, { error: `invalid bundle: ${err.message}` });
    return;
  }

  const name = cleanName(bundle.name);
  const version = cleanVersion(bundle.version);
  if (!name || !version || !Array.isArray(bundle.files)) {
    send(res, 400, { error: "bundle requires valid name, version, and files array" });
    return;
  }

  const plannedFiles = [];
  for (const file of bundle.files) {
    if (!file || typeof file.path !== "string" || typeof file.content !== "string") {
      send(res, 400, { error: "invalid file entry" });
      return;
    }
    const full = safeJoin(path.join(root, name, version), file.path);
    if (!full) {
      send(res, 400, { error: `unsafe file path: ${file.path}` });
      return;
    }
    plannedFiles.push({ full, content: file.content });
  }

  const owner = claimPackageOwner(name, account, res);
  if (!owner) return;

  const versionDir = path.join(root, name, version);
  fs.rmSync(versionDir, { recursive: true, force: true });
  ensureDir(versionDir);
  for (const file of plannedFiles) {
    ensureDir(path.dirname(file.full));
    fs.writeFileSync(file.full, file.content);
  }
  if (!bundleText.endsWith("\n")) bundleText += "\n";
  fs.writeFileSync(path.join(versionDir, "package.surabundle.json"), bundleText);

  const latestDir = path.join(root, name, "latest");
  fs.rmSync(latestDir, { recursive: true, force: true });
  ensureDir(latestDir);
  fs.copyFileSync(path.join(versionDir, "package.surabundle.json"), path.join(latestDir, "package.surabundle.json"));

  recordPublish(name, version, account.user);
  rebuildIndex();
  send(res, 201, { ok: true, name, version, owner, publishedBy: account.user, hash: hashText(bundleText) });
}

async function handleCreateToken(req, res) {
  const account = requireAdmin(req, res);
  if (!account) return;
  let body;
  try {
    body = JSON.parse(await readBody(req, 64 * 1024));
  } catch (err) {
    send(res, 400, { error: `invalid token request: ${err.message}` });
    return;
  }

  const user = cleanUser(body.user);
  if (!user) {
    send(res, 400, { error: "token request requires valid user" });
    return;
  }
  let rawToken = typeof body.token === "string" && body.token ? body.token : randomSecret();
  if (!validSecret(rawToken)) {
    send(res, 400, { error: "token must be 8-256 chars and must not contain quotes or newlines" });
    return;
  }
  const store = loadTokens();
  issueToken(store, user, !!body.admin, rawToken, account.user);
  const recoveryCode = issueRecoveryCode(store, user, !!body.admin, account.user);
  saveTokens(store);
  send(res, 201, { ok: true, user, admin: !!body.admin, token: rawToken, recoveryCode });
}

async function handleRecoverToken(req, res) {
  let body;
  try {
    body = JSON.parse(await readBody(req, 64 * 1024));
  } catch (err) {
    send(res, 400, { error: `invalid recovery request: ${err.message}` });
    return;
  }
  const user = cleanUser(body.user);
  const recoveryCode = typeof body.recoveryCode === "string" ? body.recoveryCode : "";
  if (!user || !validSecret(recoveryCode, 12, 256)) {
    send(res, 400, { error: "recovery requires valid user and recoveryCode" });
    return;
  }
  let rawToken = typeof body.token === "string" && body.token ? body.token : randomSecret();
  if (!validSecret(rawToken)) {
    send(res, 400, { error: "token must be 8-256 chars and must not contain quotes or newlines" });
    return;
  }
  const store = loadTokens();
  ensureTokenStoreShape(store);
  const recoveryHash = tokenHash(recoveryCode);
  const recovery = store.recovery[recoveryHash];
  if (!recovery || recovery.user !== user || recovery.usedAt) {
    send(res, 403, { error: "invalid or already used recovery code" });
    return;
  }
  const revoked = revokeUserTokens(store, user, "recovery");
  recovery.usedAt = new Date().toISOString();
  recovery.usedBy = user;
  issueToken(store, user, !!recovery.admin, rawToken, "recovery", { recoveredAt: new Date().toISOString() });
  const nextRecoveryCode = issueRecoveryCode(store, user, !!recovery.admin, "recovery");
  saveTokens(store);
  send(res, 201, {
    ok: true,
    user,
    admin: !!recovery.admin,
    token: rawToken,
    recoveryCode: nextRecoveryCode,
    revokedTokens: revoked,
  });
}

async function handleYank(req, res) {
  const account = requireAdmin(req, res);
  if (!account) return;
  let body;
  try {
    body = JSON.parse(await readBody(req, 64 * 1024));
  } catch (err) {
    send(res, 400, { error: `invalid yank request: ${err.message}` });
    return;
  }

  const name = cleanName(body.name);
  const version = cleanVersion(body.version);
  if (!name || !version || version === "latest") {
    send(res, 400, { error: "yank request requires concrete name and version" });
    return;
  }
  const bundle = path.join(root, name, version, "package.surabundle.json");
  if (!fs.existsSync(bundle)) {
    send(res, 404, { error: "package version not found" });
    return;
  }

  const yanks = loadYanks();
  const key = packageVersionKey(name, version);
  const shouldYank = body.yanked !== false;
  if (shouldYank) {
    yanks.yanked[key] = {
      name,
      version,
      reason: typeof body.reason === "string" ? body.reason.slice(0, 500) : "",
      by: account.user,
      at: new Date().toISOString(),
    };
  } else {
    delete yanks.yanked[key];
  }
  saveYanks(yanks);
  rebuildIndex();
  send(res, 200, { ok: true, name, version, yanked: shouldYank, entry: yanks.yanked[key] || null });
}

function packageExists(name, version) {
  if (version) return fs.existsSync(path.join(root, name, version, "package.surabundle.json"));
  return fs.existsSync(path.join(root, name));
}

function reportStatusCounts(reportItems) {
  const counts = { open: 0, reviewing: 0, dismissed: 0, actioned: 0 };
  for (const item of reportItems) {
    const status = String(item.status || "open").toLowerCase();
    counts[status] = (counts[status] || 0) + 1;
  }
  return counts;
}

function reportQueue(url) {
  const reports = loadReports();
  const reportItems = Array.isArray(reports.reports) ? reports.reports : [];
  const status = String(url.searchParams.get("status") || "").toLowerCase();
  const name = cleanName(url.searchParams.get("name") || "");
  const version = cleanVersion(url.searchParams.get("version") || "");
  const rawLimit = Number(url.searchParams.get("limit") || "100");
  const limit = Number.isFinite(rawLimit) ? Math.min(Math.max(Math.floor(rawLimit), 1), 200) : 100;
  const filtered = reportItems.filter((item) => {
    if (status && String(item.status || "open").toLowerCase() !== status) return false;
    if (name && item.name !== name) return false;
    if (version && item.version !== version) return false;
    return true;
  }).slice(0, limit);
  return {
    schema: "sura.registry.reports.queue.v1",
    ok: true,
    status: status || "all",
    name: name || null,
    version: version || null,
    limit,
    total: reportItems.length,
    count: filtered.length,
    counts: reportStatusCounts(reportItems),
    reports: filtered,
  };
}

async function handleReport(req, res) {
  const account = optionalAuth(req, res);
  if (req.headers.authorization && !account) return;
  let body;
  try {
    body = JSON.parse(await readBody(req, 96 * 1024));
  } catch (err) {
    send(res, 400, { error: `invalid report request: ${err.message}` });
    return;
  }

  const name = cleanName(body.name);
  const version = body.version ? cleanVersion(body.version) : "";
  const reason = typeof body.reason === "string" ? body.reason.trim().slice(0, 2000) : "";
  const contact = typeof body.contact === "string" ? body.contact.trim().slice(0, 200) : "";
  const priority = String(body.priority || "normal").toLowerCase();
  const cleanPriority = ["low", "normal", "high", "urgent"].includes(priority) ? priority : "normal";
  if (!name || (body.version && !version) || reason.length < 8) {
    send(res, 400, { error: "report requires valid name, optional version, and reason with at least 8 chars" });
    return;
  }
  if (!packageExists(name, version)) {
    send(res, 404, { error: "reported package not found", name, version });
    return;
  }

  const reports = loadReports();
  if (!Array.isArray(reports.reports)) reports.reports = [];
  const now = new Date().toISOString();
  const id = hashText(`sura-report:${name}:${version}:${reason}:${now}:${crypto.randomBytes(8).toString("hex")}`).slice(0, 16);
  const entry = {
    id,
    name,
    version,
    reason,
    contact,
    source: typeof body.source === "string" ? body.source.slice(0, 80) : "api",
    reporter: account ? account.user : "anonymous",
    queue: "abuse",
    priority: cleanPriority,
    status: "open",
    createdAt: now,
    updatedAt: now,
  };
  reports.reports.push(entry);
  saveReports(reports);
  appendModerationEvent("report.created", {
    id,
    name,
    version,
    status: "open",
    reporter: entry.reporter,
    priority: cleanPriority,
    source: entry.source,
  });
  send(res, 201, { ok: true, id, report: entry });
}

function cleanSeverity(value) {
  const severity = String(value || "").toLowerCase();
  return ["low", "moderate", "high", "critical"].includes(severity) ? severity : null;
}

function cleanAdvisoryStatus(value) {
  const status = String(value || "active").toLowerCase();
  return ["active", "resolved", "dismissed"].includes(status) ? status : null;
}

async function handleAdvisory(req, res) {
  const account = requireAdmin(req, res);
  if (!account) return;
  let body;
  try {
    body = JSON.parse(await readBody(req, 96 * 1024));
  } catch (err) {
    send(res, 400, { error: `invalid advisory request: ${err.message}` });
    return;
  }

  const name = cleanName(body.name);
  const version = body.version ? cleanVersion(body.version) : "";
  const severity = cleanSeverity(body.severity);
  const status = cleanAdvisoryStatus(body.status);
  const title = typeof body.title === "string" ? body.title.trim().slice(0, 180) : "";
  const description = typeof body.description === "string" ? body.description.trim().slice(0, 2000) : "";
  const reference = typeof body.url === "string" ? body.url.trim().slice(0, 500) : "";
  if (!name || (body.version && !version) || !severity || !status || title.length < 4 || description.length < 8) {
    send(res, 400, { error: "advisory requires valid name, optional version, severity, status, title, and description" });
    return;
  }
  if (!packageExists(name, version)) {
    send(res, 404, { error: "advisory package not found", name, version });
    return;
  }

  const advisories = loadAdvisories();
  if (!Array.isArray(advisories.advisories)) advisories.advisories = [];
  const now = new Date().toISOString();
  const id = hashText(`sura-advisory:${name}:${version}:${severity}:${title}:${now}:${crypto.randomBytes(8).toString("hex")}`).slice(0, 16);
  const entry = {
    id,
    name,
    version,
    severity,
    status,
    title,
    description,
    url: reference,
    createdBy: account.user,
    createdAt: now,
    updatedAt: now,
  };
  advisories.advisories.push(entry);
  saveAdvisories(advisories);
  send(res, 201, { ok: true, id, advisory: entry });
}

async function handleReviewReport(req, res) {
  const account = requireAdmin(req, res);
  if (!account) return;
  let body;
  try {
    body = JSON.parse(await readBody(req, 64 * 1024));
  } catch (err) {
    send(res, 400, { error: `invalid report review request: ${err.message}` });
    return;
  }

  const id = typeof body.id === "string" ? body.id : "";
  const status = typeof body.status === "string" ? body.status : "";
  const allowed = new Set(["open", "reviewing", "dismissed", "actioned"]);
  if (!id || !allowed.has(status)) {
    send(res, 400, { error: "review requires id and status open|reviewing|dismissed|actioned" });
    return;
  }
  const reports = loadReports();
  const entry = Array.isArray(reports.reports) ? reports.reports.find((item) => item.id === id) : null;
  if (!entry) {
    send(res, 404, { error: "report not found", id });
    return;
  }
  entry.status = status;
  entry.reviewNote = typeof body.note === "string" ? body.note.slice(0, 1000) : "";
  entry.reviewedBy = account.user;
  entry.reviewedAt = new Date().toISOString();
  entry.updatedAt = entry.reviewedAt;
  let yanked = false;
  if (status === "actioned" && body.yank === true) {
    if (!entry.version) {
      send(res, 400, { error: "actioned report yank requires a package version", id });
      return;
    }
    const yanks = loadYanks();
    if (!yanks.yanked) yanks.yanked = {};
    yanks.yanked[packageVersionKey(entry.name, entry.version)] = {
      name: entry.name,
      version: entry.version,
      reason: entry.reviewNote || `abuse report ${id} actioned`,
      by: account.user,
      at: entry.reviewedAt,
      reportId: id,
    };
    saveYanks(yanks);
    rebuildIndex();
    yanked = true;
    entry.action = "yanked";
  }
  saveReports(reports);
  appendModerationEvent("report.reviewed", {
    id,
    name: entry.name,
    version: entry.version,
    status,
    reviewedBy: account.user,
    yanked,
  });
  send(res, 200, { ok: true, report: entry, yanked });
}

function serveBundle(req, res, urlPath) {
  const parts = urlPath.split("/").filter(Boolean);
  if (parts.length !== 3 || parts[2] !== "package.surabundle.json") {
    send(res, 404, { error: "not found" });
    return;
  }
  const name = cleanName(parts[0]);
  const version = cleanVersion(parts[1]);
  if (!name || !version) {
    send(res, 404, { error: "not found" });
    return;
  }
  const bundle = path.join(root, name, version, "package.surabundle.json");
  if (!fs.existsSync(bundle)) {
    send(res, 404, { error: "not found" });
    return;
  }
  const text = fs.readFileSync(bundle, "utf8");
  let parsed = {};
  try { parsed = JSON.parse(text); } catch {}
  const resolvedVersion = version === "latest" ? (parsed.version || "latest") : version;
  const yank = loadYanks().yanked?.[packageVersionKey(name, resolvedVersion)];
  if (yank) {
    send(res, 410, {
      error: "package version yanked",
      name,
      version: resolvedVersion,
      reason: yank.reason || "",
      yankedAt: yank.at,
    });
    return;
  }
  recordDownload(name, resolvedVersion);
  send(res, 200, text);
}

function servePackageDetail(req, res, urlPath) {
  const parts = urlPath.split("/").filter(Boolean);
  if ((parts.length !== 3 && parts.length !== 4) || parts[0] !== "api" || parts[1] !== "package") {
    send(res, 404, { error: "not found" });
    return;
  }
  const name = cleanName(parts[2] || "");
  const version = cleanVersion(parts[3] || "latest");
  if (!name || !version) {
    send(res, 404, { error: "not found" });
    return;
  }
  const detail = packageDetail(name, version);
  if (!detail) {
    send(res, 404, { error: "package not found", name, version });
    return;
  }
  if (detail.yanked) {
    send(res, 410, {
      error: "package version yanked",
      name: detail.name,
      version: detail.version,
      reason: detail.yankReason || "",
    });
    return;
  }
  send(res, 200, detail);
}

function servePackagePage(req, res, urlPath) {
  const parts = urlPath.split("/").filter(Boolean);
  if ((parts.length !== 2 && parts.length !== 3) || parts[0] !== "package") {
    send(res, 404, { error: "not found" });
    return;
  }
  const name = cleanName(parts[1] || "");
  const version = cleanVersion(parts[2] || "latest");
  if (!name || !version) {
    send(res, 404, { error: "not found" });
    return;
  }
  const detail = packageDetail(name, version);
  if (!detail) {
    send(res, 404, { error: "package not found", name, version });
    return;
  }
  send(res, detail.yanked ? 410 : 200, packageDetailHtml(detail), "text/html");
}

function serveRegistryIndexPage(req, res, url) {
  const query = url.searchParams.get("q") || "";
  send(res, 200, registryIndexHtml(query), "text/html");
}

function serveAdvisories(req, res, url) {
  send(res, 200, { advisories: filterAdvisories(url) });
}

function serveKey(req, res, urlPath) {
  const parts = urlPath.split("/").filter(Boolean);
  if (parts.length !== 2 || parts[0] !== "keys" || !parts[1].endsWith(".pem")) {
    send(res, 404, { error: "not found" });
    return;
  }
  const keyId = cleanKeyId(parts[1].slice(0, -4));
  if (!keyId) {
    send(res, 404, { error: "not found" });
    return;
  }
  const keyPath = path.join(keysPath, `${keyId}.pem`);
  if (!fs.existsSync(keyPath)) {
    send(res, 404, { error: "not found" });
    return;
  }
  const text = fs.readFileSync(keyPath, "utf8");
  if (!text.includes("PUBLIC KEY")) {
    send(res, 404, { error: "not found" });
    return;
  }
  send(res, 200, text, "application/x-pem-file");
}

ensureDir(root);
rebuildIndex();
const startedAt = new Date();

function healthStatus() {
  const index = rebuildIndex();
  const reports = loadReports();
  const reportItems = Array.isArray(reports.reports) ? reports.reports : [];
  const openReports = reportItems.filter((item) => String(item.status || "open").toLowerCase() === "open");
  const reportCounts = reportStatusCounts(reportItems);
  const advisories = loadAdvisories();
  const advisoryItems = Array.isArray(advisories.advisories) ? advisories.advisories : [];
  return {
    schema: "sura.registry.health_endpoint.v1",
    ok: true,
    service: "sura-registry",
    root,
    startedAt: startedAt.toISOString(),
    uptimeSeconds: Math.max(0, Math.floor((Date.now() - startedAt.getTime()) / 1000)),
    packageCount: Array.isArray(index.packages) ? index.packages.length : 0,
    reportCount: reportItems.length,
    openReportCount: openReports.length,
    reportCounts,
    advisoryCount: advisoryItems.length,
    activeAdvisoryCount: advisoryItems.filter((item) => String(item.status || "active").toLowerCase() === "active").length,
  };
}

const server = http.createServer(async (req, res) => {
  const url = new URL(req.url, `http://${req.headers.host || "localhost"}`);
  try {
    if (req.method === "GET" && url.pathname === "/health") {
      send(res, 200, healthStatus());
    } else if (req.method === "GET" && (url.pathname === "/" || url.pathname === "/packages")) {
      serveRegistryIndexPage(req, res, url);
    } else if (req.method === "GET" && url.pathname === "/index.json") {
      send(res, 200, rebuildIndex());
    } else if (req.method === "GET" && url.pathname === "/api/stats") {
      send(res, 200, loadStats());
    } else if (req.method === "GET" && url.pathname === "/api/analytics") {
      send(res, 200, analyticsSummary());
    } else if (req.method === "GET" && url.pathname === "/api/owners") {
      send(res, 200, loadOwners());
    } else if (req.method === "GET" && url.pathname === "/api/yanks") {
      send(res, 200, loadYanks());
    } else if (req.method === "GET" && url.pathname === "/api/advisories") {
      serveAdvisories(req, res, url);
    } else if (req.method === "GET" && url.pathname === "/api/reports") {
      const account = requireAdmin(req, res);
      if (account) send(res, 200, reportQueue(url));
    } else if (req.method === "GET" && url.pathname.startsWith("/api/package/")) {
      servePackageDetail(req, res, url.pathname);
    } else if (req.method === "GET" && url.pathname.startsWith("/package/")) {
      servePackagePage(req, res, url.pathname);
    } else if (req.method === "GET" && url.pathname.startsWith("/keys/")) {
      serveKey(req, res, url.pathname);
    } else if (req.method === "POST" && url.pathname === "/api/tokens") {
      await handleCreateToken(req, res);
    } else if (req.method === "POST" && url.pathname === "/api/tokens/recover") {
      await handleRecoverToken(req, res);
    } else if (req.method === "POST" && url.pathname === "/api/yank") {
      await handleYank(req, res);
    } else if (req.method === "POST" && url.pathname === "/api/report") {
      await handleReport(req, res);
    } else if (req.method === "POST" && url.pathname === "/api/advisories") {
      await handleAdvisory(req, res);
    } else if (req.method === "POST" && url.pathname === "/api/reports/review") {
      await handleReviewReport(req, res);
    } else if (req.method === "POST" && url.pathname === "/api/publish") {
      await handlePublish(req, res);
    } else if (req.method === "GET") {
      serveBundle(req, res, url.pathname);
    } else {
      send(res, 405, { error: "method not allowed" });
    }
  } catch (err) {
    send(res, 500, { error: err.message });
  }
});

server.listen(port, host, () => {
  const displayHost = host === "0.0.0.0" || host === "::" ? "localhost" : host;
  console.log(`[OK] Sura registry API serving ${root}`);
  console.log(`[OK] SURA_REGISTRY_URL=http://${displayHost}:${port}`);
  console.log(`[OK] SURA_REGISTRY_TOKEN=${token}`);
  console.log(`[OK] SURA_REGISTRY_ADMIN_TOKEN=${adminToken}`);
});

function shutdown(signal) {
  console.log(`[OK] Sura registry API received ${signal}, shutting down`);
  server.close(() => process.exit(0));
  setTimeout(() => process.exit(1), 5000).unref();
}

process.on("SIGTERM", () => shutdown("SIGTERM"));
process.on("SIGINT", () => shutdown("SIGINT"));
