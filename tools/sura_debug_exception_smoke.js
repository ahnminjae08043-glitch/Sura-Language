const cp = require("child_process");
const fs = require("fs");
const os = require("os");
const path = require("path");

const root = path.resolve(__dirname, "..");
const engine = process.argv[2] || path.join(root, "SuraLanguage.exe");
const adapterPath = path.join(root, "sura-vscode", "out", "debugAdapter.js");

const tempDir = fs.mkdtempSync(path.join(os.tmpdir(), "sura-debug-exception-"));
const program = path.join(tempDir, "exception.sura");
fs.writeFileSync(
  program,
  [
    "func divide(x, y) do",
    "  return x / y",
    "end",
    "func wrapper() do",
    "  denom is 0",
    "  return divide(10, denom)",
    "end",
    "z is wrapper()",
    "print z",
    "",
  ].join("\n"),
  "utf8",
);

const adapter = cp.spawn(process.execPath, [adapterPath], {
  cwd: root,
  stdio: ["pipe", "pipe", "pipe"],
});

let buffer = Buffer.alloc(0);
let nextSeq = 1;
const pending = new Map();
const events = [];

function send(message) {
  const body = Buffer.from(JSON.stringify({ seq: nextSeq++, type: "request", ...message }), "utf8");
  adapter.stdin.write(`Content-Length: ${body.length}\r\n\r\n`);
  adapter.stdin.write(body);
}

function request(command, args = {}) {
  const seq = nextSeq;
  send({ command, arguments: args });
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => {
      pending.delete(seq);
      reject(new Error(`timeout waiting for ${command}`));
    }, 10000);
    pending.set(seq, { command, resolve, reject, timer });
  });
}

function waitEvent(name, predicate = () => true) {
  const existing = events.find((event) => event.event === name && predicate(event));
  if (existing) return Promise.resolve(existing);
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error(`timeout waiting for event ${name}`)), 10000);
    events.push({ __waiter: true, name, predicate, resolve, timer });
  });
}

function handleMessage(message) {
  if (message.type === "response") {
    const item = pending.get(message.request_seq);
    if (!item) return;
    pending.delete(message.request_seq);
    clearTimeout(item.timer);
    if (message.success === false) item.reject(new Error(message.message || `${item.command} failed`));
    else item.resolve(message);
    return;
  }
  if (message.type === "event") {
    for (let i = events.length - 1; i >= 0; i--) {
      const waiter = events[i];
      if (waiter.__waiter && waiter.name === message.event && waiter.predicate(message)) {
        events.splice(i, 1);
        clearTimeout(waiter.timer);
        waiter.resolve(message);
      }
    }
    events.push(message);
  }
}

adapter.stdout.on("data", (chunk) => {
  buffer = Buffer.concat([buffer, chunk]);
  while (true) {
    const marker = buffer.indexOf(Buffer.from("\r\n\r\n"));
    if (marker < 0) return;
    const header = buffer.slice(0, marker).toString("utf8");
    const match = header.match(/Content-Length:\s*(\d+)/i);
    if (!match) throw new Error(`bad DAP header: ${header}`);
    const length = Number(match[1]);
    const start = marker + 4;
    if (buffer.length < start + length) return;
    const body = buffer.slice(start, start + length).toString("utf8");
    buffer = buffer.slice(start + length);
    handleMessage(JSON.parse(body));
  }
});

adapter.stderr.on("data", (chunk) => process.stderr.write(chunk));

async function main() {
  const init = await request("initialize", { adapterID: "sura", linesStartAt1: true, columnsStartAt1: true });
  const filters = (init.body && init.body.exceptionBreakpointFilters) || [];
  if (!filters.some((filter) => filter.filter === "runtime")) {
    throw new Error(`runtime exception filter missing: ${JSON.stringify(init.body)}`);
  }

  await request("launch", {
    program,
    enginePath: engine,
    cwd: root,
    stopOnEntry: false,
    jit: false,
  });
  await waitEvent("initialized");
  await request("setExceptionBreakpoints", { filters: ["runtime"] });
  await request("configurationDone");

  const stopped = await waitEvent("stopped", (event) => event.body && event.body.reason === "exception");
  if (!String(stopped.body.description || "").includes("division by zero")) {
    throw new Error(`bad exception description: ${JSON.stringify(stopped.body)}`);
  }

  const stack = await request("stackTrace", { threadId: 1 });
  const frame = stack.body.stackFrames[0];
  if (!frame || frame.line !== 2 || frame.name !== "divide") {
    throw new Error(`bad exception frame: ${JSON.stringify(stack.body)}`);
  }
  const frameNames = stack.body.stackFrames.map((item) => item.name);
  if (!frameNames.includes("wrapper") || !frameNames.includes("<main>") || stack.body.stackFrames.length < 3) {
    throw new Error(`missing call stack frames: ${JSON.stringify(stack.body)}`);
  }
  const wrapperFrame = stack.body.stackFrames.find((item) => item.name === "wrapper");
  if (!wrapperFrame || wrapperFrame.line !== 6) {
    throw new Error(`bad wrapper frame: ${JSON.stringify(stack.body)}`);
  }

  const scopes = await request("scopes", { frameId: frame.id });
  const localsScope = scopes.body.scopes.find((scope) => scope.name === "Locals");
  const locals = await request("variables", { variablesReference: localsScope.variablesReference });
  if (!locals.body.variables.some((v) => v.name === "y" && v.value === "0")) {
    throw new Error(`missing y at exception: ${JSON.stringify(locals.body)}`);
  }
  const wrapperScopes = await request("scopes", { frameId: wrapperFrame.id });
  const wrapperLocalsScope = wrapperScopes.body.scopes.find((scope) => scope.name === "Locals");
  const wrapperLocals = await request("variables", { variablesReference: wrapperLocalsScope.variablesReference });
  if (!wrapperLocals.body.variables.some((v) => v.name === "denom" && v.value === "0")) {
    throw new Error(`missing wrapper denom local: ${JSON.stringify(wrapperLocals.body)}`);
  }

  const evalY = await request("evaluate", { expression: "y == 0", frameId: frame.id, context: "watch" });
  if (!evalY.body || evalY.body.result !== "true") {
    throw new Error(`bad exception evaluate: ${JSON.stringify(evalY.body)}`);
  }

  await request("continue", { threadId: 1 });
  await waitEvent("terminated");

  console.log(
    JSON.stringify(
      {
        ok: true,
        exceptionLine: frame.line,
        stackFrames: frameNames,
        wrapperLine: wrapperFrame.line,
        description: stopped.body.description,
        evaluate: evalY.body.result,
      },
      null,
      2,
    ),
  );
}

main()
  .catch((error) => {
    console.error(error.stack || String(error));
    process.exitCode = 1;
  })
  .finally(() => {
    setTimeout(() => {
      if (!adapter.killed) adapter.kill();
    }, 100);
  });
