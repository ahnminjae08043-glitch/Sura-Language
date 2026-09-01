const cp = require("child_process");
const fs = require("fs");
const os = require("os");
const path = require("path");

const root = path.resolve(__dirname, "..");
const engine = process.argv[2] || path.join(root, "SuraLanguage.exe");
const adapterPath = path.join(root, "sura-vscode", "out", "debugAdapter.js");

const tempDir = fs.mkdtempSync(path.join(os.tmpdir(), "sura-debug-locals-"));
const program = path.join(tempDir, "locals.sura");
fs.writeFileSync(
  program,
    [
      "func calc(a) do",
      "  inner is a + 2",
      '  items is [a, inner, "ok"]',
      '  config is {"a": a, "inner": inner, "tensor": autograd_parameter([a, inner])}',
      "  return inner",
      "end",
      "",
      "class Counter do",
      "  base is 0",
      '  label is ""',
      "  func init(base) do",
      "    self.base is base",
      '    self.label is "counter"',
      "  end",
      "  func add(x) do",
      "    total is self.base + x",
      "    return total",
      "  end",
      "end",
      "",
      "result is calc(5)",
      "c is new Counter(3)",
      "method_result is c.add(4)",
      "print result",
      "print method_result",
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

function forgetEvents(name) {
  for (let i = events.length - 1; i >= 0; i--) {
    if (!events[i].__waiter && events[i].event === name) events.splice(i, 1);
  }
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
  if (!init.body || init.body.supportsEvaluateForHovers !== true) throw new Error("evaluate not advertised");

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
      { line: 5, condition: "(a + inner) == 12" },
      { line: 17, condition: "(total / 7) == 1" },
    ],
  });
  if (!bp.body.breakpoints.every((breakpoint) => breakpoint.verified)) {
    throw new Error(`breakpoint not verified: ${JSON.stringify(bp.body)}`);
  }

  await request("configurationDone");
  await waitEvent("stopped", (event) => event.body && event.body.reason === "breakpoint");

  const stack = await request("stackTrace", { threadId: 1 });
  const frame = stack.body.stackFrames[0];
  if (!frame || frame.line !== 5 || frame.name !== "calc") {
    throw new Error(`bad function frame: ${JSON.stringify(stack.body)}`);
  }

  const scopes = await request("scopes", { frameId: frame.id });
  const localsScope = scopes.body.scopes.find((scope) => scope.name === "Locals");
  if (!localsScope) throw new Error(`missing locals scope: ${JSON.stringify(scopes.body)}`);

  const localsAtBreakpoint = await request("variables", { variablesReference: localsScope.variablesReference });
  const localValues = new Map(localsAtBreakpoint.body.variables.map((v) => [v.name, v.value]));
  if (localValues.get("a") !== "5" || localValues.get("inner") !== "7") {
    throw new Error(`bad locals at breakpoint: ${JSON.stringify(localsAtBreakpoint.body)}`);
  }
  const itemsVar = localsAtBreakpoint.body.variables.find((v) => v.name === "items");
  if (!itemsVar || itemsVar.value !== '[5, 7, "ok"]' || !itemsVar.variablesReference) {
    throw new Error(`items not expandable: ${JSON.stringify(localsAtBreakpoint.body)}`);
  }
  const itemChildren = await request("variables", { variablesReference: itemsVar.variablesReference });
  const itemValues = new Map(itemChildren.body.variables.map((v) => [v.name, v.value]));
  if (itemValues.get("[0]") !== "5" || itemValues.get("[1]") !== "7" || itemValues.get("[2]") !== "ok") {
    throw new Error(`bad item children: ${JSON.stringify(itemChildren.body)}`);
  }

  const configVar = localsAtBreakpoint.body.variables.find((v) => v.name === "config");
  if (!configVar || !configVar.variablesReference) {
    throw new Error(`config not expandable: ${JSON.stringify(localsAtBreakpoint.body)}`);
  }
  const configChildren = await request("variables", { variablesReference: configVar.variablesReference });
  const configValues = new Map(configChildren.body.variables.map((v) => [v.name, v.value]));
  if (configValues.get("a") !== "5" || configValues.get("inner") !== "7") {
    throw new Error(`bad config children: ${JSON.stringify(configChildren.body)}`);
  }
  const tensorVar = configChildren.body.variables.find((v) => v.name === "tensor");
  if (!tensorVar || tensorVar.value !== "<Tensor shape=[2] requires_grad>" || !tensorVar.variablesReference) {
    throw new Error(`tensor not expandable: ${JSON.stringify(configChildren.body)}`);
  }
  const tensorChildren = await request("variables", { variablesReference: tensorVar.variablesReference });
  const tensorFields = new Map(tensorChildren.body.variables.map((v) => [v.name, v]));
  if (tensorFields.get("shape")?.value !== "[2]" || tensorFields.get("requires_grad")?.value !== "true") {
    throw new Error(`bad tensor metadata: ${JSON.stringify(tensorChildren.body)}`);
  }
  const tensorData = tensorFields.get("data");
  if (!tensorData || !tensorData.variablesReference) {
    throw new Error(`tensor data not expandable: ${JSON.stringify(tensorChildren.body)}`);
  }
  const tensorDataChildren = await request("variables", { variablesReference: tensorData.variablesReference });
  const tensorDataValues = new Map(tensorDataChildren.body.variables.map((v) => [v.name, v.value]));
  if (tensorDataValues.get("[0]") !== "5" || tensorDataValues.get("[1]") !== "7") {
    throw new Error(`bad tensor data: ${JSON.stringify(tensorDataChildren.body)}`);
  }

  const evalInner = await request("evaluate", { expression: "inner", frameId: frame.id, context: "watch" });
  if (!evalInner.body || evalInner.body.result !== "7") {
    throw new Error(`bad local evaluate: ${JSON.stringify(evalInner.body)}`);
  }
  const evalItems = await request("evaluate", { expression: "items", frameId: frame.id, context: "watch" });
  if (!evalItems.body || evalItems.body.result !== '[5, 7, "ok"]' || !evalItems.body.variablesReference) {
    throw new Error(`bad expandable watch evaluate: ${JSON.stringify(evalItems.body)}`);
  }
  const evalItemIndex = await request("evaluate", { expression: "items[1]", frameId: frame.id, context: "watch" });
  if (!evalItemIndex.body || evalItemIndex.body.result !== "7") {
    throw new Error(`bad indexed watch evaluate: ${JSON.stringify(evalItemIndex.body)}`);
  }

  const evalConfigKey = await request("evaluate", { expression: 'config["a"]', frameId: frame.id, context: "watch" });
  if (!evalConfigKey.body || evalConfigKey.body.result !== "5") {
    throw new Error(`bad dict key watch evaluate: ${JSON.stringify(evalConfigKey.body)}`);
  }

  const evalCondition = await request("evaluate", { expression: "a == 5", frameId: frame.id, context: "watch" });
  if (!evalCondition.body || evalCondition.body.result !== "true") {
    throw new Error(`bad local condition evaluate: ${JSON.stringify(evalCondition.body)}`);
  }

  const evalArithmetic = await request("evaluate", { expression: "inner + a * 2", frameId: frame.id, context: "watch" });
  if (!evalArithmetic.body || evalArithmetic.body.result !== "17") {
    throw new Error(`bad arithmetic evaluate: ${JSON.stringify(evalArithmetic.body)}`);
  }

  const evalGrouped = await request("evaluate", { expression: "(inner - a) * 3", frameId: frame.id, context: "watch" });
  if (!evalGrouped.body || evalGrouped.body.result !== "6") {
    throw new Error(`bad grouped evaluate: ${JSON.stringify(evalGrouped.body)}`);
  }

  const evalNestedArithmetic = await request("evaluate", { expression: 'items[1] + config["a"]', frameId: frame.id, context: "watch" });
  if (!evalNestedArithmetic.body || evalNestedArithmetic.body.result !== "12") {
    throw new Error(`bad nested arithmetic evaluate: ${JSON.stringify(evalNestedArithmetic.body)}`);
  }

  const evalLenItems = await request("evaluate", { expression: "len(items)", frameId: frame.id, context: "watch" });
  if (!evalLenItems.body || evalLenItems.body.result !== "3") {
    throw new Error(`bad len() watch evaluate: ${JSON.stringify(evalLenItems.body)}`);
  }

  const evalTypeItems = await request("evaluate", { expression: "type(items)", frameId: frame.id, context: "watch" });
  if (!evalTypeItems.body || evalTypeItems.body.result !== "array") {
    throw new Error(`bad type() watch evaluate: ${JSON.stringify(evalTypeItems.body)}`);
  }

  const evalKeysConfig = await request("evaluate", { expression: "keys(config)", frameId: frame.id, context: "watch" });
  if (!evalKeysConfig.body || !evalKeysConfig.body.variablesReference) {
    throw new Error(`bad keys() watch evaluate: ${JSON.stringify(evalKeysConfig.body)}`);
  }
  const evalKeysChildren = await request("variables", { variablesReference: evalKeysConfig.body.variablesReference });
  const evalKeysValues = new Set(evalKeysChildren.body.variables.map((v) => String(v.value).replace(/^"|"$/g, "")));
  if (!evalKeysValues.has("a") || !evalKeysValues.has("inner") || !evalKeysValues.has("tensor") || evalKeysValues.size !== 3) {
    throw new Error(`bad keys() children: ${JSON.stringify(evalKeysChildren.body)}`);
  }

  const evalContainsArray = await request("evaluate", { expression: "contains(items, 7)", frameId: frame.id, context: "watch" });
  if (!evalContainsArray.body || evalContainsArray.body.result !== "true") {
    throw new Error(`bad contains() array watch evaluate: ${JSON.stringify(evalContainsArray.body)}`);
  }

  const evalContainsDict = await request("evaluate", { expression: 'contains(config, "inner")', frameId: frame.id, context: "watch" });
  if (!evalContainsDict.body || evalContainsDict.body.result !== "true") {
    throw new Error(`bad contains() dict watch evaluate: ${JSON.stringify(evalContainsDict.body)}`);
  }

  const evalNumHelper = await request("evaluate", { expression: 'num("5") + 1', frameId: frame.id, context: "watch" });
  if (!evalNumHelper.body || evalNumHelper.body.result !== "6") {
    throw new Error(`bad num() watch evaluate: ${JSON.stringify(evalNumHelper.body)}`);
  }

  const evalUnknownName = await request("evaluate", { expression: "missing + 1", frameId: frame.id, context: "watch" });
  if (!evalUnknownName.body || !String(evalUnknownName.body.result).includes('unknown name "missing"')) {
    throw new Error(`missing-name diagnostic not returned: ${JSON.stringify(evalUnknownName.body)}`);
  }

  const evalDivideByZero = await request("evaluate", { expression: "inner / 0", frameId: frame.id, context: "watch" });
  if (!evalDivideByZero.body || !String(evalDivideByZero.body.result).includes('operator "/" cannot use zero')) {
    throw new Error(`divide-by-zero diagnostic not returned: ${JSON.stringify(evalDivideByZero.body)}`);
  }

  forgetEvents("stopped");
  await request("continue", { threadId: 1 });
  await waitEvent("stopped", (event) => event.body && event.body.reason === "breakpoint");

  const methodStack = await request("stackTrace", { threadId: 1 });
  const methodFrame = methodStack.body.stackFrames[0];
  if (!methodFrame || methodFrame.line !== 17 || methodFrame.name !== "add") {
    throw new Error(`bad method frame: ${JSON.stringify(methodStack.body)}`);
  }

  const methodScopes = await request("scopes", { frameId: methodFrame.id });
  const methodLocalsScope = methodScopes.body.scopes.find((scope) => scope.name === "Locals");
  if (!methodLocalsScope) throw new Error(`missing method locals scope: ${JSON.stringify(methodScopes.body)}`);

  const methodLocals = await request("variables", { variablesReference: methodLocalsScope.variablesReference });
  const methodValues = new Map(methodLocals.body.variables.map((v) => [v.name, v.value]));
  if (!methodValues.has("self") || methodValues.get("x") !== "4" || methodValues.get("total") !== "7") {
    throw new Error(`bad method locals at breakpoint: ${JSON.stringify(methodLocals.body)}`);
  }
  const selfVar = methodLocals.body.variables.find((v) => v.name === "self");
  if (!selfVar || selfVar.value !== "<Instance Counter>" || !selfVar.variablesReference) {
    throw new Error(`self not expandable: ${JSON.stringify(methodLocals.body)}`);
  }
  const selfChildren = await request("variables", { variablesReference: selfVar.variablesReference });
  const selfFields = new Map(selfChildren.body.variables.map((v) => [v.name, v.value]));
  if (selfFields.get("base") !== "3" || selfFields.get("label") !== "counter") {
    throw new Error(`bad self fields: ${JSON.stringify(selfChildren.body)}`);
  }

  const evalSelf = await request("evaluate", { expression: "self", frameId: methodFrame.id, context: "watch" });
  if (!evalSelf.body || evalSelf.body.result !== "<Instance Counter>" || !evalSelf.body.variablesReference) {
    throw new Error(`bad expandable self watch evaluate: ${JSON.stringify(evalSelf.body)}`);
  }
  const evalSelfChildren = await request("variables", { variablesReference: evalSelf.body.variablesReference });
  const evalSelfFields = new Map(evalSelfChildren.body.variables.map((v) => [v.name, v.value]));
  if (evalSelfFields.get("base") !== "3" || evalSelfFields.get("label") !== "counter") {
    throw new Error(`bad expandable self watch fields: ${JSON.stringify(evalSelfChildren.body)}`);
  }

  const evalSelfBase = await request("evaluate", { expression: "self.base", frameId: methodFrame.id, context: "watch" });
  if (!evalSelfBase.body || evalSelfBase.body.result !== "3") {
    throw new Error(`bad field watch evaluate: ${JSON.stringify(evalSelfBase.body)}`);
  }

  const evalSelfLabel = await request("evaluate", { expression: "self.label", frameId: methodFrame.id, context: "watch" });
  if (!evalSelfLabel.body || evalSelfLabel.body.result !== "counter") {
    throw new Error(`bad string field watch evaluate: ${JSON.stringify(evalSelfLabel.body)}`);
  }

  const evalTotal = await request("evaluate", { expression: "total", frameId: methodFrame.id, context: "watch" });
  if (!evalTotal.body || evalTotal.body.result !== "7") {
    throw new Error(`bad method local evaluate: ${JSON.stringify(evalTotal.body)}`);
  }

  const evalMethodArithmetic = await request("evaluate", { expression: "total % x", frameId: methodFrame.id, context: "watch" });
  if (!evalMethodArithmetic.body || evalMethodArithmetic.body.result !== "3") {
    throw new Error(`bad method arithmetic evaluate: ${JSON.stringify(evalMethodArithmetic.body)}`);
  }

  const evalMethodFieldArithmetic = await request("evaluate", { expression: "self.base + x", frameId: methodFrame.id, context: "watch" });
  if (!evalMethodFieldArithmetic.body || evalMethodFieldArithmetic.body.result !== "7") {
    throw new Error(`bad method field arithmetic evaluate: ${JSON.stringify(evalMethodFieldArithmetic.body)}`);
  }

  await request("continue", { threadId: 1 });
  await waitEvent("terminated");

  console.log(
    JSON.stringify(
      {
        ok: true,
        functionFrame: frame.name,
        methodFrame: methodFrame.name,
        functionLocals: Object.fromEntries(localValues),
        methodLocals: Object.fromEntries(methodValues),
        expandedItems: Object.fromEntries(itemValues),
        expandedConfig: Object.fromEntries(configValues),
        expandedSelf: Object.fromEntries(selfFields),
        arithmetic: {
          functionExpression: evalArithmetic.body.result,
          groupedExpression: evalGrouped.body.result,
          nestedExpression: evalNestedArithmetic.body.result,
          methodExpression: evalMethodArithmetic.body.result,
          methodFieldExpression: evalMethodFieldArithmetic.body.result,
        },
        safeHelpers: {
          lenItems: evalLenItems.body.result,
          typeItems: evalTypeItems.body.result,
          keysConfig: Array.from(evalKeysValues).sort(),
          containsArray: evalContainsArray.body.result,
          containsDict: evalContainsDict.body.result,
          numHelper: evalNumHelper.body.result,
        },
        diagnostics: {
          unknownName: evalUnknownName.body.result,
          divideByZero: evalDivideByZero.body.result,
        },
        watchAccess: {
          itemIndex: evalItemIndex.body.result,
          configKey: evalConfigKey.body.result,
          selfBase: evalSelfBase.body.result,
          selfLabel: evalSelfLabel.body.result,
          expandableSelf: Object.fromEntries(evalSelfFields),
        },
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
