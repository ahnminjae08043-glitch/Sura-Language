import { spawn, ChildProcessWithoutNullStreams } from 'child_process';
import * as fs from 'fs';
import * as path from 'path';

type Json = Record<string, unknown>;
type EvalKind = 'number' | 'string' | 'boolean' | 'nil' | 'undefined';
type EvalValue = { kind: EvalKind; value?: number | string | boolean };
type EvalResult = { value: EvalValue; diagnostic?: string };
type EvalTokenType = 'number' | 'string' | 'identifier' | 'operator' | 'paren' | 'comma' | 'eof';
type EvalToken = { type: EvalTokenType; text: string };
type DebugVariable = { name: string; value: string; variablesReference: number; type?: string };
type RawDebugVariable = { name: string; value: string; children: RawDebugVariable[] };
type ParsedDebugValue = {
  kind: 'array' | 'dict' | 'tensor' | 'scalar';
  text: string;
  children: { name: string; value: ParsedDebugValue }[];
};

interface DapRequest {
  seq: number;
  type: 'request';
  command: string;
  arguments?: Json;
}

interface LaunchArgs extends Json {
  program?: string;
  enginePath?: string;
  cwd?: string;
  args?: string[];
  env?: Record<string, string>;
  jit?: boolean;
  trace?: boolean;
  profile?: boolean;
  debug?: boolean;
  stopOnEntry?: boolean;
  noDebug?: boolean;
}

interface DebugBreakpoint {
  line: number;
  condition?: string;
  hitCondition?: string;
  hits: number;
  verified: boolean;
}

class DapAdapter {
  private nextSeq = 1;
  private buffer = Buffer.alloc(0);
  private launchArgs: LaunchArgs | undefined;
  private configured = false;
  private started = false;
  private child: ChildProcessWithoutNullStreams | undefined;
  private breakpointId = 1;
  private breakpoints = new Map<string, DebugBreakpoint[]>();
  private debugBuffer = '';
  private currentStop: Json | undefined;
  private breakOnRuntimeException = false;
  private nextVariablesReference = 3;
  private expandedVariables = new Map<number, DebugVariable[]>();
  private scopedVariables = new Map<number, DebugVariable[]>();

  constructor() {
    process.stdin.on('data', (chunk: Buffer) => this.accept(chunk));
    process.stdin.resume();
  }

  private accept(chunk: Buffer): void {
    this.buffer = Buffer.concat([this.buffer, chunk]);
    while (true) {
      const headerEnd = this.buffer.indexOf('\r\n\r\n');
      if (headerEnd < 0) return;

      const header = this.buffer.slice(0, headerEnd).toString('utf8');
      const match = /Content-Length:\s*(\d+)/i.exec(header);
      if (!match) {
        this.buffer = this.buffer.slice(headerEnd + 4);
        continue;
      }

      const length = Number(match[1]);
      const bodyStart = headerEnd + 4;
      const bodyEnd = bodyStart + length;
      if (this.buffer.length < bodyEnd) return;

      const body = this.buffer.slice(bodyStart, bodyEnd).toString('utf8');
      this.buffer = this.buffer.slice(bodyEnd);
      try {
        this.handle(JSON.parse(body) as DapRequest);
      } catch (err) {
        this.event('output', {
          category: 'stderr',
          output: `Sura debug adapter parse error: ${String(err)}\n`,
        });
      }
    }
  }

  private send(message: Json): void {
    const payload = JSON.stringify({ seq: this.nextSeq++, ...message });
    process.stdout.write(`Content-Length: ${Buffer.byteLength(payload, 'utf8')}\r\n\r\n${payload}`);
  }

  private response(request: DapRequest, body: Json = {}, success = true, message?: string): void {
    const msg: Json = {
      type: 'response',
      request_seq: request.seq,
      success,
      command: request.command,
    };
    if (message) msg.message = message;
    if (Object.keys(body).length > 0) msg.body = body;
    this.send(msg);
  }

  private event(event: string, body: Json = {}): void {
    const msg: Json = { type: 'event', event };
    if (Object.keys(body).length > 0) msg.body = body;
    this.send(msg);
  }

  private handle(request: DapRequest): void {
    switch (request.command) {
      case 'initialize':
        this.response(request, {
          supportsConfigurationDoneRequest: true,
          supportsTerminateRequest: true,
          supportsSetVariable: false,
          supportsEvaluateForHovers: true,
          supportsConditionalBreakpoints: true,
          supportsHitConditionalBreakpoints: true,
          supportsStepBack: false,
          supportsStepInTargetsRequest: false,
          exceptionBreakpointFilters: [
            {
              filter: 'runtime',
              label: 'Runtime errors',
              description: 'Break when the Sura VM raises a runtime error.',
              default: false,
            },
          ],
        });
        return;

      case 'launch':
        this.launchArgs = request.arguments as LaunchArgs | undefined;
        this.response(request);
        this.event('initialized');
        return;

      case 'setBreakpoints':
        this.setBreakpoints(request);
        return;

      case 'setExceptionBreakpoints':
        this.setExceptionBreakpoints(request);
        return;

      case 'configurationDone':
        this.configured = true;
        this.response(request);
        this.startIfReady();
        return;

      case 'threads':
        this.response(request, { threads: [{ id: 1, name: 'Sura main' }] });
        return;

      case 'stackTrace':
        this.stackTrace(request);
        return;

      case 'scopes':
        this.scopes(request);
        return;

      case 'variables':
        this.variables(request);
        return;

      case 'evaluate':
        this.evaluate(request);
        return;

      case 'continue':
        this.sendDebugCommand('continue');
        this.response(request, { allThreadsContinued: true });
        return;

      case 'next':
      case 'stepIn':
      case 'stepOut':
        this.sendDebugCommand(request.command);
        this.response(request);
        return;

      case 'pause':
        this.response(request, {}, false, 'Pause is not available until the Sura VM reaches a line boundary.');
        return;

      case 'disconnect':
      case 'terminate':
        this.stop();
        this.response(request);
        this.event('terminated');
        return;

      default:
        this.response(request, {}, false, `Unsupported request: ${request.command}`);
        return;
    }
  }

  private setBreakpoints(request: DapRequest): void {
    const args = request.arguments ?? {};
    const source = args.source as Json | undefined;
    const sourcePath = typeof source?.path === 'string' ? source.path : '';
    const rawBreakpoints = Array.isArray(args.breakpoints) ? args.breakpoints as Json[] : [];
    const validLines = this.loadExecutableLines(sourcePath);
    const stored: DebugBreakpoint[] = [];
    const breakpoints = rawBreakpoints.map((bp) => {
      const line = typeof bp.line === 'number' ? bp.line : 1;
      const verified = validLines.has(line);
      const condition = typeof bp.condition === 'string' ? bp.condition.trim() : undefined;
      const hitCondition = typeof bp.hitCondition === 'string' ? bp.hitCondition.trim() : undefined;
      stored.push({ line, condition, hitCondition, hits: 0, verified });
      return {
        id: this.breakpointId++,
        verified,
        line,
        message: verified
          ? 'Verified Sura line breakpoint.'
          : 'No executable Sura source found on this line.',
      };
    });
    if (sourcePath) this.breakpoints.set(path.resolve(sourcePath), stored);
    this.response(request, { breakpoints });
  }

  private setExceptionBreakpoints(request: DapRequest): void {
    const args = request.arguments ?? {};
    const filters = Array.isArray(args.filters) ? args.filters : [];
    this.breakOnRuntimeException = filters.includes('runtime');
    this.response(request);
  }

  private stackTrace(request: DapRequest): void {
    if (!this.currentStop || typeof this.currentStop.line !== 'number') {
      this.response(request, { stackFrames: [], totalFrames: 0 });
      return;
    }
    const program = this.launchArgs?.program || '';
    const source = program ? { name: path.basename(program), path: program } : undefined;
    const fallbackLine = this.currentStop.line;
    const rawFrames = Array.isArray(this.currentStop.frames) ? this.currentStop.frames as Json[] : [];
    const stackFrames = rawFrames.length > 0
      ? rawFrames.map((raw, index) => ({
          id: index + 1,
          name: typeof raw.name === 'string' ? raw.name : index === 0 ? '<main>' : `<frame ${index + 1}>`,
          source,
          line: typeof raw.line === 'number' && raw.line > 0 ? raw.line : fallbackLine,
          column: 1,
        }))
      : [{
          id: 1,
          name: typeof this.currentStop.function === 'string' ? this.currentStop.function : '<main>',
          source,
          line: this.currentStop.line,
          column: 1,
        }];
    this.response(request, { stackFrames, totalFrames: stackFrames.length });
  }

  private scopes(request: DapRequest): void {
    if (!this.currentStop) {
      this.response(request, { scopes: [] });
      return;
    }
    const args = request.arguments ?? {};
    const frameId = typeof args.frameId === 'number' ? args.frameId : 1;
    const rawFrames = Array.isArray(this.currentStop.frames) ? this.currentStop.frames as Json[] : [];
    const frame = frameId > 0 && frameId <= rawFrames.length ? rawFrames[frameId - 1] : undefined;
    const rawLocals = Array.isArray(frame?.locals) ? frame?.locals : this.currentStop.locals;
    const localsRef = this.createScopedVariablesReference(rawLocals);
    this.response(request, {
      scopes: [
        { name: 'Globals', variablesReference: 1, expensive: false },
        { name: 'Locals', variablesReference: localsRef, expensive: false },
      ],
    });
  }

  private variables(request: DapRequest): void {
    const args = request.arguments ?? {};
    const ref = typeof args.variablesReference === 'number' ? args.variablesReference : 0;
    const expanded = this.expandedVariables.get(ref);
    if (expanded) {
      this.response(request, { variables: expanded });
      return;
    }
    const scoped = this.scopedVariables.get(ref);
    if (scoped) {
      this.response(request, { variables: scoped });
      return;
    }
    const raw = ref === 1 ? this.currentStop?.globals : ref === 2 ? this.currentStop?.locals : [];
    this.response(request, { variables: this.variablesFromRaw(raw) });
  }

  private evaluate(request: DapRequest): void {
    const args = request.arguments ?? {};
    const expression = typeof args.expression === 'string' ? args.expression.trim() : '';
    if (!expression) {
      this.response(request, { result: '', variablesReference: 0 });
      return;
    }
    const frameId = typeof args.frameId === 'number' ? args.frameId : undefined;
    const direct = this.directSnapshotEntry(expression, frameId);
    const result = direct ? direct.value : this.evaluateExpression(expression, frameId);
    const rawRef = direct ? this.createRawChildrenReference(direct.children) : 0;
    this.response(request, {
      result,
      variablesReference: rawRef || this.createVariablesReference(result),
      type: this.debugValueType(result),
    });
  }

  private resetExpandedVariables(): void {
    this.nextVariablesReference = 3;
    this.expandedVariables.clear();
    this.scopedVariables.clear();
  }

  private variablesFromRaw(raw: unknown): DebugVariable[] {
    return this.rawDebugVariables(raw).map((entry) => {
      const value = entry.value;
      const childRef = this.createRawChildrenReference(entry.children);
      return {
        name: entry.name,
        value,
        variablesReference: childRef || this.createVariablesReference(value),
        type: this.debugValueType(value),
      };
    });
  }

  private createScopedVariablesReference(raw: unknown): number {
    const variables = this.variablesFromRaw(raw);
    if (variables.length === 0) return 0;
    const ref = this.nextVariablesReference++;
    this.scopedVariables.set(ref, variables);
    return ref;
  }

  private createRawChildrenReference(raw: unknown): number {
    const variables = this.variablesFromRaw(raw);
    if (variables.length === 0) return 0;
    const ref = this.nextVariablesReference++;
    this.expandedVariables.set(ref, variables);
    return ref;
  }

  private rawDebugVariables(raw: unknown): RawDebugVariable[] {
    if (!Array.isArray(raw)) return [];
    const variables: RawDebugVariable[] = [];
    for (const item of raw) {
      const entry = item as Json;
      if (typeof entry.name !== 'string' || typeof entry.value !== 'string') continue;
      variables.push({
        name: entry.name,
        value: entry.value,
        children: this.rawDebugVariables(entry.children),
      });
    }
    return variables;
  }

  private directSnapshotEntry(expression: string, frameId?: number): RawDebugVariable | undefined {
    const expr = expression.trim();
    if (!expr) return undefined;
    return this.snapshotEntries(frameId).get(expr);
  }

  private debugValueType(value: string): string | undefined {
    const trimmed = value.trim();
    if (trimmed.startsWith('[') && trimmed.endsWith(']')) return 'array';
    if (trimmed.startsWith('{') && trimmed.endsWith('}')) return 'dict';
    if (/^-?\d+(?:\.\d+)?$/.test(trimmed)) return 'number';
    if (trimmed === 'true' || trimmed === 'false') return 'boolean';
    if (trimmed === 'nil') return 'nil';
    if (trimmed.startsWith('<Instance ')) return 'instance';
    if (trimmed.startsWith('<Func ')) return 'function';
    if (trimmed.startsWith('<Tensor ') && trimmed.endsWith('>')) return 'tensor';
    return undefined;
  }

  private createVariablesReference(value: string): number {
    const parsed = this.parseDebugValue(value);
    if (!parsed || (parsed.kind !== 'array' && parsed.kind !== 'dict') || parsed.children.length === 0) return 0;
    const ref = this.nextVariablesReference++;
    this.expandedVariables.set(ref, parsed.children.map((child) => this.debugVariableFromParsed(child.name, child.value)));
    return ref;
  }

  private debugVariableFromParsed(name: string, value: ParsedDebugValue): DebugVariable {
    return {
      name,
      value: value.text,
      variablesReference: this.createVariablesReference(value.text),
      type: this.debugValueType(value.text),
    };
  }

  private parseDebugValue(value: string): ParsedDebugValue | undefined {
    const text = value.trim();
    if (!text) return undefined;
    let pos = 0;
    const skip = (): void => {
      while (pos < text.length && /\s/.test(text[pos])) pos += 1;
    };
    const parseString = (): ParsedDebugValue => {
      const start = pos;
      pos += 1;
      let escaped = false;
      while (pos < text.length) {
        const ch = text[pos++];
        if (escaped) {
          escaped = false;
        } else if (ch === '\\') {
          escaped = true;
        } else if (ch === '"') {
          break;
        }
      }
      return { kind: 'scalar', text: text.slice(start, pos), children: [] };
    };
    const parseScalar = (): ParsedDebugValue => {
      const start = pos;
      while (pos < text.length && text[pos] !== ',' && text[pos] !== ']' && text[pos] !== '}') pos += 1;
      return { kind: 'scalar', text: text.slice(start, pos).trim(), children: [] };
    };
    const parseTensor = (): ParsedDebugValue => {
      const start = pos;
      const end = text.indexOf('>', pos);
      if (end < 0) return parseScalar();
      pos = end + 1;
      return { kind: 'tensor', text: text.slice(start, pos), children: [] };
    };
    const parseArray = (): ParsedDebugValue => {
      const start = pos;
      pos += 1;
      const children: ParsedDebugValue['children'] = [];
      let index = 0;
      while (pos < text.length) {
        skip();
        if (text[pos] === ']') {
          pos += 1;
          break;
        }
        const child = parseValue();
        children.push({ name: `[${index++}]`, value: child });
        skip();
        if (text[pos] === ',') {
          pos += 1;
          continue;
        }
        if (text[pos] === ']') {
          pos += 1;
          break;
        }
        break;
      }
      return { kind: 'array', text: text.slice(start, pos), children };
    };
    const parseDictKey = (): string => {
      skip();
      if (text[pos] !== '"') return parseScalar().text;
      const parsed = parseString().text;
      return parsed.length >= 2 ? parsed.slice(1, -1) : parsed;
    };
    const parseDict = (): ParsedDebugValue => {
      const start = pos;
      pos += 1;
      const children: ParsedDebugValue['children'] = [];
      while (pos < text.length) {
        skip();
        if (text[pos] === '}') {
          pos += 1;
          break;
        }
        const key = parseDictKey();
        skip();
        if (text[pos] === ':') pos += 1;
        skip();
        const child = parseValue();
        children.push({ name: key, value: child });
        skip();
        if (text[pos] === ',') {
          pos += 1;
          continue;
        }
        if (text[pos] === '}') {
          pos += 1;
          break;
        }
        break;
      }
      return { kind: 'dict', text: text.slice(start, pos), children };
    };
    const parseValue = (): ParsedDebugValue => {
      skip();
      if (text[pos] === '[') return parseArray();
      if (text[pos] === '{') return parseDict();
      if (text[pos] === '"') return parseString();
      if (text.startsWith('<Tensor ', pos)) return parseTensor();
      return parseScalar();
    };
    const parsed = parseValue();
    skip();
    return pos === text.length ? parsed : undefined;
  }

  private frameLocals(frameId?: number): unknown {
    const rawFrames = Array.isArray(this.currentStop?.frames) ? this.currentStop?.frames as Json[] : [];
    const frame = frameId && frameId > 0 && frameId <= rawFrames.length ? rawFrames[frameId - 1] : undefined;
    return Array.isArray(frame?.locals) ? frame?.locals : this.currentStop?.locals;
  }

  private snapshotValues(frameId?: number): Map<string, string> {
    const values = new Map<string, string>();
    for (const [name, entry] of this.snapshotEntries(frameId)) values.set(name, entry.value);
    return values;
  }

  private snapshotEntries(frameId?: number): Map<string, RawDebugVariable> {
    const values = new Map<string, RawDebugVariable>();
    const addAll = (raw: unknown): void => {
      for (const entry of this.rawDebugVariables(raw)) this.addSnapshotEntry(values, entry, [entry.name]);
    };
    addAll(this.currentStop?.globals);
    addAll(this.frameLocals(frameId));
    return values;
  }

  private addSnapshotEntry(
    values: Map<string, RawDebugVariable>,
    entry: RawDebugVariable,
    aliases: string[],
  ): void {
    for (const alias of aliases) values.set(alias, entry);
    for (const child of entry.children) {
      const childAliases = this.childSnapshotAliases(aliases, child.name);
      this.addSnapshotEntry(values, child, childAliases);
    }
  }

  private childSnapshotAliases(parents: string[], childName: string): string[] {
    const aliases: string[] = [];
    for (const parent of parents) {
      if (/^\[[0-9]+\]$/.test(childName)) {
        aliases.push(`${parent}${childName}`);
        continue;
      }
      if (this.canUseDotProperty(childName)) aliases.push(`${parent}.${childName}`);
      aliases.push(`${parent}["${this.escapeQuotedKey(childName, '"')}"]`);
      aliases.push(`${parent}['${this.escapeQuotedKey(childName, "'")}']`);
    }
    return aliases;
  }

  private canUseDotProperty(name: string): boolean {
    return name.length > 0 && !/[\s.[\]()+\-*/%!=<>"']/.test(name);
  }

  private escapeQuotedKey(key: string, quote: '"' | "'"): string {
    return key.replace(/\\/g, '\\\\').replace(new RegExp(`\\${quote}`, 'g'), `\\${quote}`);
  }

  private tokenizeExpression(expression: string): EvalToken[] {
    const tokens: EvalToken[] = [];
    let i = 0;
    const isBoundary = (ch: string): boolean => /\s/.test(ch) || '(),+-*/%!=<>'.includes(ch);

    while (i < expression.length) {
      const ch = expression[i];
      if (/\s/.test(ch)) {
        i += 1;
        continue;
      }

      const two = expression.slice(i, i + 2);
      if (two === '==' || two === '!=' || two === '>=' || two === '<=') {
        tokens.push({ type: 'operator', text: two });
        i += 2;
        continue;
      }

      if ('+-*/%!<>'.includes(ch)) {
        tokens.push({ type: 'operator', text: ch });
        i += 1;
        continue;
      }

      if (ch === '(' || ch === ')') {
        tokens.push({ type: 'paren', text: ch });
        i += 1;
        continue;
      }

      if (ch === ',') {
        tokens.push({ type: 'comma', text: ch });
        i += 1;
        continue;
      }

      if (ch === '"' || ch === "'") {
        const quote = ch;
        i += 1;
        let text = '';
        while (i < expression.length) {
          const cur = expression[i];
          if (cur === '\\' && i + 1 < expression.length) {
            const escaped = expression[i + 1];
            if (escaped === 'n') text += '\n';
            else if (escaped === 'r') text += '\r';
            else if (escaped === 't') text += '\t';
            else text += escaped;
            i += 2;
            continue;
          }
          if (cur === quote) {
            i += 1;
            break;
          }
          text += cur;
          i += 1;
        }
        tokens.push({ type: 'string', text });
        continue;
      }

      if (/\d/.test(ch) || (ch === '.' && i + 1 < expression.length && /\d/.test(expression[i + 1]))) {
        const start = i;
        i += 1;
        while (i < expression.length && /[\d.]/.test(expression[i])) i += 1;
        tokens.push({ type: 'number', text: expression.slice(start, i) });
        continue;
      }

      const start = i;
      while (i < expression.length && !isBoundary(expression[i])) i += 1;
      if (i === start) {
        tokens.push({ type: 'operator', text: ch });
        i += 1;
      } else {
        tokens.push({ type: 'identifier', text: expression.slice(start, i) });
      }
    }

    tokens.push({ type: 'eof', text: '' });
    return tokens;
  }

  private undefinedValue(): EvalValue {
    return { kind: 'undefined' };
  }

  private snapshotToEvalValue(raw: string | undefined): EvalValue {
    if (raw === undefined) return this.undefinedValue();
    if (/^-?\d+(?:\.\d+)?$/.test(raw)) return { kind: 'number', value: Number(raw) };
    if (raw === 'true' || raw === 'false') return { kind: 'boolean', value: raw === 'true' };
    if (raw === 'nil') return { kind: 'nil' };
    return { kind: 'string', value: raw };
  }

  private evalValueToNumber(value: EvalValue): number | undefined {
    if (value.kind === 'number') return value.value as number;
    if (value.kind === 'string') {
      const n = Number(value.value);
      return Number.isFinite(n) ? n : undefined;
    }
    return undefined;
  }

  private displayEvalValue(value: EvalValue): string {
    if (value.kind === 'undefined') return '<undefined>';
    if (value.kind === 'nil') return 'nil';
    if (value.kind === 'number') {
      const n = value.value as number;
      return Number.isInteger(n) ? String(n) : String(n);
    }
    if (value.kind === 'boolean') return (value.value as boolean) ? 'true' : 'false';
    return String(value.value ?? '');
  }

  private truthyValue(value: EvalValue): boolean {
    if (value.kind === 'undefined' || value.kind === 'nil') return false;
    if (value.kind === 'boolean') return value.value === true;
    if (value.kind === 'number') return value.value !== 0;
    return String(value.value ?? '') !== '';
  }

  private compareEvalValues(left: EvalValue, op: string, right: EvalValue): EvalValue {
    if (left.kind === 'undefined' || right.kind === 'undefined') {
      return { kind: 'boolean', value: op === '!=' };
    }

    const ln = this.evalValueToNumber(left);
    const rn = this.evalValueToNumber(right);
    const numeric = ln !== undefined && rn !== undefined;
    let result = false;

    if (op === '==') result = numeric ? ln === rn : this.displayEvalValue(left) === this.displayEvalValue(right);
    else if (op === '!=') result = numeric ? ln !== rn : this.displayEvalValue(left) !== this.displayEvalValue(right);
    else if (numeric && op === '>') result = ln > rn;
    else if (numeric && op === '<') result = ln < rn;
    else if (numeric && op === '>=') result = ln >= rn;
    else if (numeric && op === '<=') result = ln <= rn;

    return { kind: 'boolean', value: result };
  }

  private parseEvaluateExpressionDetailed(expression: string, values: Map<string, string>): EvalResult {
    const tokens = this.tokenizeExpression(expression);
    let pos = 0;
    let diagnostic: string | undefined;
    const peek = (): EvalToken => tokens[pos] || { type: 'eof', text: '' };
    const take = (): EvalToken => tokens[pos++] || { type: 'eof', text: '' };
    const isOp = (...ops: string[]): boolean => peek().type === 'operator' && ops.includes(peek().text);
    const fail = (message: string): EvalValue => {
      if (!diagnostic) diagnostic = message;
      return this.undefinedValue();
    };
    const displayToken = (token: EvalToken): string => token.type === 'eof' ? 'end of expression' : `"${token.text}"`;
    const quoted = (text: string): string => `"${text.replace(/\\/g, '\\\\').replace(/"/g, '\\"')}"`;
    const debugKindName = (value: EvalValue): string => {
      if (value.kind !== 'string') return value.kind;
      const text = this.displayEvalValue(value);
      const parsed = this.parseDebugValue(text);
      if (parsed?.kind === 'array' || parsed?.kind === 'dict') return parsed.kind;
      return this.debugValueType(text) || 'string';
    };
    const expectArgCount = (name: string, args: EvalValue[], min: number, max = min): boolean => {
      if (args.length >= min && args.length <= max) return true;
      const expected = min === max ? String(min) : `${min}-${max}`;
      fail(`${name}() expects ${expected} argument(s), got ${args.length}`);
      return false;
    };
    const callHelper = (name: string, args: EvalValue[]): EvalValue => {
      if (name === 'len') {
        if (!expectArgCount(name, args, 1)) return this.undefinedValue();
        const text = this.displayEvalValue(args[0]);
        const parsed = this.parseDebugValue(text);
        if (parsed?.kind === 'array' || parsed?.kind === 'dict') {
          return { kind: 'number', value: parsed.children.length };
        }
        if (args[0].kind === 'undefined') return fail('len() cannot inspect an undefined value');
        if (args[0].kind === 'nil') return { kind: 'number', value: 0 };
        return { kind: 'number', value: text.length };
      }
      if (name === 'type') {
        if (!expectArgCount(name, args, 1)) return this.undefinedValue();
        return { kind: 'string', value: debugKindName(args[0]) };
      }
      if (name === 'str') {
        if (!expectArgCount(name, args, 1)) return this.undefinedValue();
        return { kind: 'string', value: this.displayEvalValue(args[0]) };
      }
      if (name === 'num') {
        if (!expectArgCount(name, args, 1)) return this.undefinedValue();
        const n = this.evalValueToNumber(args[0]);
        return n === undefined ? fail('num() expects a numeric value') : { kind: 'number', value: n };
      }
      if (name === 'bool') {
        if (!expectArgCount(name, args, 1)) return this.undefinedValue();
        return { kind: 'boolean', value: this.truthyValue(args[0]) };
      }
      if (name === 'keys') {
        if (!expectArgCount(name, args, 1)) return this.undefinedValue();
        const parsed = this.parseDebugValue(this.displayEvalValue(args[0]));
        if (parsed?.kind !== 'dict') return fail('keys() expects a dict value');
        return { kind: 'string', value: `[${parsed.children.map((child) => quoted(child.name)).join(', ')}]` };
      }
      if (name === 'contains') {
        if (!expectArgCount(name, args, 2)) return this.undefinedValue();
        const containerText = this.displayEvalValue(args[0]);
        const needle = this.displayEvalValue(args[1]);
        const parsed = this.parseDebugValue(containerText);
        if (parsed?.kind === 'array') {
          return { kind: 'boolean', value: parsed.children.some((child) => child.value.text === needle) };
        }
        if (parsed?.kind === 'dict') {
          return { kind: 'boolean', value: parsed.children.some((child) => child.name === needle) };
        }
        if (args[0].kind === 'undefined') return fail('contains() cannot inspect an undefined value');
        return { kind: 'boolean', value: containerText.includes(needle) };
      }
      return fail(`unsupported safe helper "${name}"`);
    };
    const parseCallArguments = (name: string): EvalValue[] => {
      const args: EvalValue[] = [];
      take();
      if (peek().type === 'paren' && peek().text === ')') {
        take();
        return args;
      }
      while (peek().type !== 'eof') {
        args.push(parseComparison());
        if (peek().type === 'comma') {
          take();
          continue;
        }
        if (peek().type === 'paren' && peek().text === ')') {
          take();
          return args;
        }
        fail(`${name}() expected "," or ")" but found ${displayToken(peek())}`);
        return args;
      }
      fail(`${name}() missing closing ")"`);
      return args;
    };

    const parseComparison = (): EvalValue => {
      let left = parseAdditive();
      if (isOp('==', '!=', '>=', '<=', '>', '<')) {
        const op = take().text;
        const right = parseAdditive();
        left = this.compareEvalValues(left, op, right);
      }
      return left;
    };

    const parseAdditive = (): EvalValue => {
      let left = parseMultiplicative();
      while (isOp('+', '-')) {
        const op = take().text;
        const right = parseMultiplicative();
        if (op === '+' && (left.kind === 'string' || right.kind === 'string') &&
            left.kind !== 'undefined' && right.kind !== 'undefined') {
          left = { kind: 'string', value: this.displayEvalValue(left) + this.displayEvalValue(right) };
          continue;
        }

        const ln = this.evalValueToNumber(left);
        const rn = this.evalValueToNumber(right);
        if (ln === undefined || rn === undefined) return fail(`operator "${op}" expects numeric operands`);
        left = { kind: 'number', value: op === '+' ? ln + rn : ln - rn };
      }
      return left;
    };

    const parseMultiplicative = (): EvalValue => {
      let left = parseUnary();
      while (isOp('*', '/', '%')) {
        const op = take().text;
        const right = parseUnary();
        const ln = this.evalValueToNumber(left);
        const rn = this.evalValueToNumber(right);
        if (ln === undefined || rn === undefined || ((op === '/' || op === '%') && rn === 0)) {
          return fail(rn === 0 ? `operator "${op}" cannot use zero as the right operand` :
            `operator "${op}" expects numeric operands`);
        }
        if (op === '*') left = { kind: 'number', value: ln * rn };
        else if (op === '/') left = { kind: 'number', value: ln / rn };
        else left = { kind: 'number', value: ln % rn };
      }
      return left;
    };

    const parseUnary = (): EvalValue => {
      if (isOp('!')) {
        take();
        return { kind: 'boolean', value: !this.truthyValue(parseUnary()) };
      }
      if (isOp('-')) {
        take();
        const value = parseUnary();
        const n = this.evalValueToNumber(value);
        return n === undefined ? fail('unary "-" expects a numeric operand') : { kind: 'number', value: -n };
      }
      return parsePrimary();
    };

    const parsePrimary = (): EvalValue => {
      const token = take();
      if (token.type === 'number') {
        const n = Number(token.text);
        return Number.isFinite(n) ? { kind: 'number', value: n } : this.undefinedValue();
      }
      if (token.type === 'string') return { kind: 'string', value: token.text };
      if (token.type === 'identifier') {
        if (token.text === 'true' || token.text === 'false') {
          return { kind: 'boolean', value: token.text === 'true' };
        }
        if (token.text === 'nil') return { kind: 'nil' };
        if (peek().type === 'paren' && peek().text === '(') {
          return callHelper(token.text, parseCallArguments(token.text));
        }
        if (!values.has(token.text)) return fail(`unknown name "${token.text}"`);
        return this.snapshotToEvalValue(values.get(token.text));
      }
      if (token.type === 'paren' && token.text === '(') {
        const value = parseComparison();
        if (peek().type === 'paren' && peek().text === ')') take();
        else return fail(`expected ")" but found ${displayToken(peek())}`);
        return value;
      }
      return fail(`unexpected token ${displayToken(token)}`);
    };

    const result = parseComparison();
    if (peek().type !== 'eof' && !diagnostic) diagnostic = `unexpected token ${displayToken(peek())}`;
    return { value: peek().type === 'eof' ? result : this.undefinedValue(), diagnostic };
  }

  private parseEvaluateExpression(expression: string, values: Map<string, string>): EvalValue {
    return this.parseEvaluateExpressionDetailed(expression, values).value;
  }

  private evaluateCondition(expression: string): boolean {
    const expr = expression.trim();
    if (!expr) return true;
    return this.truthyValue(this.parseEvaluateExpression(expr, this.snapshotValues()));
  }

  private evaluateExpression(expression: string, frameId?: number): string {
    const expr = expression.trim();
    if (!expr) return '';
    const result = this.parseEvaluateExpressionDetailed(expr, this.snapshotValues(frameId));
    if (result.diagnostic) return `<debug-eval error: ${result.diagnostic}>`;
    return this.displayEvalValue(result.value);
  }

  private hitConditionMatches(bp: DebugBreakpoint): boolean {
    if (!bp.hitCondition) return true;
    const text = bp.hitCondition.trim();
    if (/^\d+$/.test(text)) return bp.hits >= Number(text);
    const match = /^(==|>=|<=|>|<)\s*(\d+)$/.exec(text);
    if (!match) return true;
    const n = Number(match[2]);
    if (match[1] === '==') return bp.hits === n;
    if (match[1] === '>=') return bp.hits >= n;
    if (match[1] === '<=') return bp.hits <= n;
    if (match[1] === '>') return bp.hits > n;
    if (match[1] === '<') return bp.hits < n;
    return true;
  }

  private shouldStopAtCurrentBreakpoint(): boolean {
    if (!this.currentStop || typeof this.currentStop.line !== 'number') return true;
    const program = this.launchArgs?.program;
    if (!program) return true;
    const bps = this.breakpoints.get(path.resolve(program)) || [];
    const matching = bps.filter((bp) => bp.verified && bp.line === this.currentStop?.line);
    if (matching.length === 0) return true;
    for (const bp of matching) {
      bp.hits += 1;
      if (bp.condition && !this.evaluateCondition(bp.condition)) continue;
      if (!this.hitConditionMatches(bp)) continue;
      return true;
    }
    return false;
  }

  private sendDebugCommand(command: string): void {
    if (!this.child || this.child.killed) return;
    this.currentStop = undefined;
    this.resetExpandedVariables();
    this.child.stdin.write(`${JSON.stringify({ command })}\n`);
  }

  private loadExecutableLines(sourcePath: string): Set<number> {
    const lines = new Set<number>();
    if (!sourcePath || !fs.existsSync(sourcePath)) return lines;
    const text = fs.readFileSync(sourcePath, 'utf8');
    text.split(/\r?\n/).forEach((line, index) => {
      const trimmed = line.trim();
      if (trimmed && !trimmed.startsWith('#') && !trimmed.startsWith('//')) {
        lines.add(index + 1);
      }
    });
    return lines;
  }

  private startIfReady(): void {
    if (!this.launchArgs || !this.configured || this.started) return;
    this.started = true;

    const program = this.launchArgs.program;
    if (!program) {
      this.event('output', { category: 'stderr', output: 'Missing Sura launch program.\n' });
      this.event('terminated');
      return;
    }

    const engine = this.launchArgs.enginePath || 'SuraLanguage.exe';
    const debugMode = !this.launchArgs.noDebug;
    const args: string[] = [];
    if (debugMode) {
      args.push('--debug-protocol');
    } else {
      if (this.launchArgs.trace) args.push('--trace');
      if (this.launchArgs.profile) args.push('--profile');
      if (this.launchArgs.jit && !this.launchArgs.trace && !this.launchArgs.debug) args.push('--jit');
    }
    if (Array.isArray(this.launchArgs.args)) args.push(...this.launchArgs.args);
    args.push(program);

    const cwd = this.launchArgs.cwd || path.dirname(program);
    const env = { ...process.env, ...(this.launchArgs.env || {}) };
    if (debugMode) {
      const bpLines = (this.breakpoints.get(path.resolve(program)) || [])
        .filter((bp) => bp.verified)
        .map((bp) => bp.line);
      env.SURA_DEBUG_BREAKPOINTS = bpLines.join(',');
      if (this.launchArgs.stopOnEntry) env.SURA_DEBUG_STOP_ON_ENTRY = '1';
      if (this.breakOnRuntimeException) env.SURA_DEBUG_BREAK_ON_EXCEPTION = '1';
    }
    this.event('output', {
      category: 'console',
      output: `Launching ${engine} ${args.join(' ')}\n`,
    });

    this.child = spawn(engine, args, { cwd, env });
    this.child.stdout.on('data', (data: Buffer) => {
      this.event('output', { category: 'stdout', output: data.toString('utf8') });
    });
    this.child.stderr.on('data', (data: Buffer) => {
      this.acceptDebugOutput(data.toString('utf8'));
    });
    this.child.on('error', (err) => {
      this.event('output', { category: 'stderr', output: `${err.message}\n` });
      this.event('terminated');
    });
    this.child.on('exit', (code, signal) => {
      const suffix = signal ? `signal ${signal}` : `exit code ${code ?? 0}`;
      this.event('output', { category: 'console', output: `Sura process finished with ${suffix}.\n` });
      this.event('terminated');
    });

  }

  private acceptDebugOutput(text: string): void {
    this.debugBuffer += text;
    const prefix = '@@SURA_DEBUG@@';
    while (true) {
      const newline = this.debugBuffer.indexOf('\n');
      if (newline < 0) return;
      const line = this.debugBuffer.slice(0, newline).trim();
      this.debugBuffer = this.debugBuffer.slice(newline + 1);
      if (!line.startsWith(prefix)) {
        if (line) this.event('output', { category: 'stderr', output: `${line}\n` });
        continue;
      }
      try {
        const message = JSON.parse(line.slice(prefix.length)) as Json;
        if (message.event === 'stopped') {
          this.currentStop = message;
          this.resetExpandedVariables();
          if (message.reason === 'breakpoint' && !this.shouldStopAtCurrentBreakpoint()) {
            this.sendDebugCommand('continue');
            continue;
          }
          this.event('stopped', {
            reason: typeof message.reason === 'string' ? message.reason : 'breakpoint',
            description: typeof message.description === 'string' ? message.description : undefined,
            threadId: 1,
            allThreadsStopped: true,
          });
        } else if (message.event === 'terminated') {
          this.event('terminated');
        }
      } catch (err) {
        this.event('output', { category: 'stderr', output: `Bad Sura debug event: ${String(err)}\n` });
      }
    }
  }

  private stop(): void {
    if (this.child && !this.child.killed) this.child.kill();
  }
}

new DapAdapter();
