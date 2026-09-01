const cp = require("child_process");
const fs = require("fs");
const os = require("os");
const path = require("path");

const root = path.resolve(__dirname, "..");
const engine = process.argv[2] || path.join(root, "SuraLanguage.exe");
const adapterPath = path.join(root, "sura-vscode", "out", "debugAdapter.js");

const tempDir = fs.mkdtempSync(path.join(os.tmpdir(), "sura-debug-smoke-"));
const program = path.join(tempDir, "debug.sura");
fs.writeFileSync(program, ["x is 1", "y is x + 2", "z is y + 4", "print z", ""].join("\n"), "utf8");

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
  if (!init.body || init.body.supportsTerminateRequest !== true) throw new Error("bad initialize capabilities");
  if (init.body.supportsConditionalBreakpoints !== true) throw new Error("conditional breakpoints not advertised");
  if (init.body.supportsEvaluateForHovers !== true) throw new Error("evaluate not advertised");

  await request("launch", {
    program,
    enginePath: engine,
    cwd: root,
    stopOnEntry: false,
    jit: false,
  });
  await waitEvent("initialized");

  const bp = await request("setBreakpoints", {
    source: { path: program },
    breakpoints: [
      { line: 2, condition: "x == 999" },
      { line: 3, condition: "y == 3" },
    ],
  });
  if (!bp.body.breakpoints.every((breakpoint) => breakpoint.verified)) {
    throw new Error(`breakpoint not verified: ${JSON.stringify(bp.body)}`);
  }

  await request("configurationDone");
  await waitEvent("stopped", (event) => event.body && event.body.reason === "breakpoint");

  const stack = await request("stackTrace", { threadId: 1 });
  const frame = stack.body.stackFrames[0];
  if (!frame || frame.line !== 3) throw new Error(`bad conditional breakpoint frame: ${JSON.stringify(stack.body)}`);

  const scopes = await request("scopes", { frameId: frame.id });
  const globalsScope = scopes.body.scopes.find((scope) => scope.name === "Globals");
  if (!globalsScope) throw new Error(`missing globals scope: ${JSON.stringify(scopes.body)}`);

  const globalsAtBreakpoint = await request("variables", { variablesReference: globalsScope.variablesReference });
  if (!globalsAtBreakpoint.body.variables.some((v) => v.name === "y" && v.value === "3")) {
    throw new Error(`missing y at conditional breakpoint: ${JSON.stringify(globalsAtBreakpoint.body)}`);
  }

  const evalY = await request("evaluate", { expression: "y", frameId: frame.id, context: "watch" });
  if (!evalY.body || evalY.body.result !== "3") {
    throw new Error(`bad variable evaluate: ${JSON.stringify(evalY.body)}`);
  }

  const evalCondition = await request("evaluate", { expression: "y == 3", frameId: frame.id, context: "watch" });
  if (!evalCondition.body || evalCondition.body.result !== "true") {
    throw new Error(`bad condition evaluate: ${JSON.stringify(evalCondition.body)}`);
  }

  await request("next", { threadId: 1 });
  await waitEvent("stopped", (event) => event.body && event.body.reason === "step");

  const stackAfterStep = await request("stackTrace", { threadId: 1 });
  const steppedFrame = stackAfterStep.body.stackFrames[0];
  if (!steppedFrame || steppedFrame.line !== 4) {
    throw new Error(`bad stepped frame: ${JSON.stringify(stackAfterStep.body)}`);
  }

  const globalsAfterStep = await request("variables", { variablesReference: globalsScope.variablesReference });
  if (!globalsAfterStep.body.variables.some((v) => v.name === "z" && v.value === "7")) {
    throw new Error(`missing z after step: ${JSON.stringify(globalsAfterStep.body)}`);
  }

  await request("continue", { threadId: 1 });
  await waitEvent("terminated");

  console.log(
    JSON.stringify(
      { ok: true, conditionalBreakpointLine: frame.line, steppedLine: steppedFrame.line, evaluateY: evalY.body.result },
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
