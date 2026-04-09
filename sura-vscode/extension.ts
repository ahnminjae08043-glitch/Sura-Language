import * as vscode from 'vscode';
import * as cp from 'child_process';
import * as path from 'path';

// ── 키워드 ──────────────────────────────────────────────────────────
const SURA_KEYWORDS = [
    'is', 'if', 'then', 'else', 'elif', 'end',
    'while', 'do', 'repeat', 'for', 'foreach', 'in', 'to', 'step',
    'break', 'continue', 'return',
    'func', 'class', 'extends',
    'try', 'catch', 'throw',
    'use', 'new', 'self', 'super',
    'true', 'false', 'nil',
    'and', 'or', 'not'
];

// ── 내장 함수 (새 문법: result is func(args)) ─────────────────────
interface FuncInfo {
    sig: string;       // 시그니처
    desc: string;      // 설명
    snippet: string;   // 삽입될 스니펫
}

const BUILTIN_FUNCTIONS: Record<string, FuncInfo> = {
    // ── 출력 / 입력
    'print':       { sig: 'print(값, ...)',             desc: '값을 출력합니다',                  snippet: 'print(${1:값})' },
    'print_n':     { sig: 'print_n(값, ...)',           desc: '줄바꿈 없이 출력합니다',           snippet: 'print_n(${1:값})' },
    'input':       { sig: 'input([메시지])',             desc: '입력을 받아 반환합니다',           snippet: 'input(${1:"메시지: "})' },

    // ── 타입
    'type':        { sig: 'type(값)',                   desc: '값의 타입 문자열을 반환합니다',    snippet: 'type(${1:값})' },
    'to_num':      { sig: 'to_num(값)',                 desc: '숫자로 변환합니다',                snippet: 'to_num(${1:값})' },
    'to_str':      { sig: 'to_str(값)',                 desc: '문자열로 변환합니다',              snippet: 'to_str(${1:값})' },

    // ── 수학
    'abs':         { sig: 'abs(값)',                    desc: '절댓값을 반환합니다',              snippet: 'abs(${1:값})' },
    'sqrt':        { sig: 'sqrt(값)',                   desc: '제곱근을 반환합니다',              snippet: 'sqrt(${1:값})' },
    'pow':         { sig: 'pow(밑, 지수)',              desc: '거듭제곱을 반환합니다',            snippet: 'pow(${1:밑}, ${2:지수})' },
    'floor':       { sig: 'floor(값)',                  desc: '내림값을 반환합니다',              snippet: 'floor(${1:값})' },
    'ceil':        { sig: 'ceil(값)',                   desc: '올림값을 반환합니다',              snippet: 'ceil(${1:값})' },
    'round':       { sig: 'round(값)',                  desc: '반올림값을 반환합니다',            snippet: 'round(${1:값})' },
    'min':         { sig: 'min(a, b)',                  desc: '최솟값을 반환합니다',              snippet: 'min(${1:a}, ${2:b})' },
    'max':         { sig: 'max(a, b)',                  desc: '최댓값을 반환합니다',              snippet: 'max(${1:a}, ${2:b})' },
    'sin':         { sig: 'sin(값)',                    desc: '사인값을 반환합니다',              snippet: 'sin(${1:값})' },
    'cos':         { sig: 'cos(값)',                    desc: '코사인값을 반환합니다',            snippet: 'cos(${1:값})' },
    'tan':         { sig: 'tan(값)',                    desc: '탄젠트값을 반환합니다',            snippet: 'tan(${1:값})' },
    'log':         { sig: 'log(값)',                    desc: '자연로그를 반환합니다',            snippet: 'log(${1:값})' },
    'inc':         { sig: 'inc(값)',                    desc: '값 + 1을 반환합니다',              snippet: 'inc(${1:값})' },
    'dec':         { sig: 'dec(값)',                    desc: '값 - 1을 반환합니다',              snippet: 'dec(${1:값})' },

    // ── 난수
    'rand':        { sig: 'rand()',                     desc: '0~1 사이 실수를 반환합니다',       snippet: 'rand()' },
    'rand_int':    { sig: 'rand_int(최소, 최대)',       desc: '정수 난수를 반환합니다',           snippet: 'rand_int(${1:1}, ${2:10})' },
    'rand_float':  { sig: 'rand_float(최소, 최대)',     desc: '실수 난수를 반환합니다',           snippet: 'rand_float(${1:0.0}, ${2:1.0})' },
    'rand_seed':   { sig: 'rand_seed(씨드)',            desc: '난수 씨드를 설정합니다',           snippet: 'rand_seed(${1:42})' },

    // ── 문자열
    'str_len':     { sig: 'str_len(문자열)',            desc: '문자 수를 반환합니다',             snippet: 'str_len(${1:문자열})' },
    'str_sub':     { sig: 'str_sub(문자열, 시작, 길이)',desc: '부분 문자열을 반환합니다',        snippet: 'str_sub(${1:문자열}, ${2:0}, ${3:3})' },
    'str_upper':   { sig: 'str_upper(문자열)',          desc: '대문자로 변환합니다',              snippet: 'str_upper(${1:문자열})' },
    'str_lower':   { sig: 'str_lower(문자열)',          desc: '소문자로 변환합니다',              snippet: 'str_lower(${1:문자열})' },
    'str_find':    { sig: 'str_find(문자열, 검색어)',   desc: '위치 인덱스를 반환합니다 (-1이면 없음)', snippet: 'str_find(${1:문자열}, ${2:검색어})' },
    'str_replace': { sig: 'str_replace(문자열, 찾기, 바꾸기)', desc: '치환된 문자열을 반환합니다', snippet: 'str_replace(${1:문자열}, ${2:찾기}, ${3:바꾸기})' },
    'str_split':   { sig: 'str_split(문자열, 구분자)', desc: '배열로 분리합니다',               snippet: 'str_split(${1:문자열}, ${2:","})'  },
    'str_join':    { sig: 'str_join(배열, 구분자)',     desc: '배열을 하나의 문자열로 합칩니다', snippet: 'str_join(${1:배열}, ${2:","})'  },
    'str_trim':    { sig: 'str_trim(문자열)',           desc: '앞뒤 공백을 제거합니다',          snippet: 'str_trim(${1:문자열})' },
    'str_starts':  { sig: 'str_starts(문자열, 접두사)',desc: '접두사로 시작하면 true',          snippet: 'str_starts(${1:문자열}, ${2:접두사})' },
    'str_ends':    { sig: 'str_ends(문자열, 접미사)',  desc: '접미사로 끝나면 true',            snippet: 'str_ends(${1:문자열}, ${2:접미사})' },
    'str_num':     { sig: 'str_num(문자)',              desc: '문자의 ASCII 코드를 반환합니다',  snippet: 'str_num(${1:문자})' },
    'num_str':     { sig: 'num_str(코드)',              desc: 'ASCII 코드를 문자로 반환합니다',  snippet: 'num_str(${1:65})' },

    // ── 배열
    'arr_new':     { sig: 'arr_new()',                  desc: '빈 배열을 반환합니다',             snippet: 'arr_new()' },
    'arr_push':    { sig: 'arr_push(배열, 값)',         desc: '배열 끝에 값을 추가합니다',       snippet: 'arr_push(${1:배열}, ${2:값})' },
    'arr_pop':     { sig: 'arr_pop(배열)',              desc: '마지막 요소를 제거하고 반환합니다',snippet: 'arr_pop(${1:배열})' },
    'arr_get':     { sig: 'arr_get(배열, 인덱스)',      desc: '인덱스의 값을 반환합니다',        snippet: 'arr_get(${1:배열}, ${2:0})' },
    'arr_set':     { sig: 'arr_set(배열, 인덱스, 값)', desc: '인덱스의 값을 설정합니다',        snippet: 'arr_set(${1:배열}, ${2:0}, ${3:값})' },
    'arr_insert':  { sig: 'arr_insert(배열, 인덱스, 값)', desc: '인덱스에 삽입합니다',          snippet: 'arr_insert(${1:배열}, ${2:0}, ${3:값})' },
    'arr_remove':  { sig: 'arr_remove(배열, 인덱스)',  desc: '인덱스의 요소를 삭제합니다',      snippet: 'arr_remove(${1:배열}, ${2:0})' },
    'arr_sort':    { sig: 'arr_sort(배열)',             desc: '배열을 정렬합니다',               snippet: 'arr_sort(${1:배열})' },
    'arr_reverse': { sig: 'arr_reverse(배열)',          desc: '배열을 뒤집습니다',               snippet: 'arr_reverse(${1:배열})' },
    'arr_copy':    { sig: 'arr_copy(배열)',             desc: '배열의 복사본을 반환합니다',      snippet: 'arr_copy(${1:배열})' },
    'arr_contains':{ sig: 'arr_contains(배열, 값)',    desc: '값이 있으면 true를 반환합니다',   snippet: 'arr_contains(${1:배열}, ${2:값})' },
    'arr_clear':   { sig: 'arr_clear(배열)',            desc: '배열을 비웁니다',                 snippet: 'arr_clear(${1:배열})' },
    'arr_sample':  { sig: 'arr_sample(배열)',           desc: '임의의 원소를 반환합니다',        snippet: 'arr_sample(${1:배열})' },
    'arr_shuffle': { sig: 'arr_shuffle(배열)',          desc: '배열을 섞습니다',                 snippet: 'arr_shuffle(${1:배열})' },
    'len':         { sig: 'len(배열_또는_문자열)',      desc: '길이를 반환합니다',               snippet: 'len(${1:배열})' },

    // ── 딕셔너리
    'dict_new':    { sig: 'dict_new()',                 desc: '빈 딕셔너리를 반환합니다',        snippet: 'dict_new()' },
    'dict_set':    { sig: 'dict_set(딕셔너리, 키, 값)',desc: '키에 값을 설정합니다',            snippet: 'dict_set(${1:딕셔너리}, ${2:"키"}, ${3:값})' },
    'dict_get':    { sig: 'dict_get(딕셔너리, 키)',    desc: '키의 값을 반환합니다',            snippet: 'dict_get(${1:딕셔너리}, ${2:"키"})' },
    'dict_has':    { sig: 'dict_has(딕셔너리, 키)',    desc: '키가 있으면 true를 반환합니다',   snippet: 'dict_has(${1:딕셔너리}, ${2:"키"})' },
    'dict_del':    { sig: 'dict_del(딕셔너리, 키)',    desc: '키를 삭제합니다',                 snippet: 'dict_del(${1:딕셔너리}, ${2:"키"})' },
    'dict_keys':   { sig: 'dict_keys(딕셔너리)',       desc: '키 목록 배열을 반환합니다',       snippet: 'dict_keys(${1:딕셔너리})' },
    'dict_len':    { sig: 'dict_len(딕셔너리)',        desc: '키 개수를 반환합니다',            snippet: 'dict_len(${1:딕셔너리})' },

    // ── 파일
    'file_save':   { sig: 'file_save(경로, 내용)',     desc: '파일에 저장합니다',               snippet: 'file_save(${1:"파일.txt"}, ${2:내용})' },
    'file_load':   { sig: 'file_load(경로)',           desc: '파일 내용을 문자열로 반환합니다', snippet: 'file_load(${1:"파일.txt"})' },
    'file_exists': { sig: 'file_exists(경로)',         desc: '파일이 존재하면 true를 반환합니다',snippet: 'file_exists(${1:"파일.txt"})' },

    // ── 시스템
    'sleep':       { sig: 'sleep(밀리초)',             desc: '지정한 시간만큼 대기합니다',      snippet: 'sleep(${1:1000})' },
    'time':        { sig: 'time()',                    desc: '현재 Unix 타임스탬프를 반환합니다',snippet: 'time()' },
    'exit':        { sig: 'exit([코드])',              desc: '프로그램을 종료합니다',            snippet: 'exit(${1:0})' },

    // ── 그래픽 (SFML)
    'win_init':    { sig: 'win_init(너비, 높이, 제목)',desc: '창을 만듭니다',                   snippet: 'win_init(${1:800}, ${2:600}, ${3:"창 제목"})' },
    'win_clear':   { sig: 'win_clear(r, g, b)',        desc: '화면을 지웁니다',                 snippet: 'win_clear(${1:0}, ${2:0}, ${3:0})' },
    'win_draw':    { sig: 'win_draw()',                desc: '화면을 갱신합니다',               snippet: 'win_draw()' },
    'win_close':   { sig: 'win_close()',               desc: '창을 닫습니다',                   snippet: 'win_close()' },
    'win_open':    { sig: 'win_open()',                desc: '창이 열려있으면 true',            snippet: 'win_open()' },
    'mouse_pos':   { sig: 'mouse_pos()',               desc: '마우스 위치 [x, y]를 반환합니다', snippet: 'mouse_pos()' },
    'mouse_down':  { sig: 'mouse_down(버튼)',          desc: '마우스 버튼이 눌리면 true',       snippet: 'mouse_down(${1:0})' },
    'key_down':    { sig: 'key_down(키)',              desc: '키가 눌리면 true를 반환합니다',   snippet: 'key_down(${1:"Space"})' },
};

// ── 키워드 hover 설명 ────────────────────────────────────────────────
const KEYWORD_DOCS: Record<string, string> = {
    'is':       '`변수 is 값` — 변수에 값을 대입합니다\n\n```\nx is 10\ns is str_upper("hello")\n```',
    'if':       '`if 조건 then ... end` — 조건문\n\n```\nif x > 5 then\n    print(x)\nend\n```',
    'elif':     '`elif 조건 then` — 추가 조건 (if 안에서 사용)\n\n```\nif x == 1 then\n    print("하나")\nelif x == 2 then\n    print("둘")\nend\n```',
    'else':     '`else` — 조건이 거짓일 때 실행',
    'while':    '`while 조건 do ... end` — 반복문\n\n```\nwhile i < 10 do\n    i is inc(i)\nend\n```',
    'for':      '`for i in 1 to 10 do ... end` — 범위 반복\n\n```\nfor i in 1 to 5 do\n    print(i)\nend\n```',
    'foreach':  '`foreach item in 배열 do ... end` — 배열 순회\n\n```\nforeach v in arr do\n    print(v)\nend\n```',
    'func':     '`func 이름(인자) do ... end` — 함수 정의\n\n```\nfunc add(a, b) do\n    return a + b\nend\n```',
    'class':    '`class 이름 do ... end` — 클래스 정의\n\n```\nclass Dog do\n    name is "Rex"\n    func bark() do\n        print("Woof!")\n    end\nend\n```',
    'return':   '`return 값` — 함수에서 값을 반환합니다',
    'break':    '`break` — 반복문을 즉시 탈출합니다',
    'continue': '`continue` — 다음 반복으로 넘어갑니다',
    'try':      '`try ... catch 변수 ... end` — 예외 처리\n\n```\ntry\n    result is 1 / 0\ncatch err\n    print(err)\nend\n```',
    'new':      '`new 클래스(인자)` — 인스턴스를 생성합니다\n\n```\ndog is new Dog("Rex")\n```',
    'use':      '`use 라이브러리` — 모듈을 불러옵니다\n\n```\nuse math\n```',
    'self':     '`self` — 현재 인스턴스를 가리킵니다',
    'super':    '`super.메서드(인자)` — 부모 클래스의 메서드를 호출합니다',
    'nil':      '`nil` — 값 없음 (null)',
    'true':     '`true` — 참',
    'false':    '`false` — 거짓',
    'and':      '`A and B` — 논리 AND',
    'or':       '`A or B` — 논리 OR',
    'not':      '`not A` — 논리 NOT',
};

// ── 1. 자동완성 ──────────────────────────────────────────────────────
class SuraCompletionProvider implements vscode.CompletionItemProvider {
    provideCompletionItems(
        document: vscode.TextDocument,
        position: vscode.Position
    ): vscode.CompletionItem[] {
        const lineText = document.lineAt(position.line).text;
        const wordRange = document.getWordRangeAtPosition(position);
        const word = wordRange ? document.getText(wordRange) : '';
        const completions: vscode.CompletionItem[] = [];

        // use 라이브러리 자동완성
        if (/^\s*use\s+\w*$/.test(lineText)) {
            for (const lib of ['math', 'game', 'string', 'system', 'time']) {
                if (lib.startsWith(word)) {
                    const item = new vscode.CompletionItem(lib, vscode.CompletionItemKind.Module);
                    item.detail = 'Sura 표준 라이브러리';
                    completions.push(item);
                }
            }
            return completions;
        }

        // 키워드
        for (const kw of SURA_KEYWORDS) {
            if (kw.startsWith(word)) {
                const item = new vscode.CompletionItem(kw, vscode.CompletionItemKind.Keyword);
                item.detail = 'Sura 키워드';
                if (kw in KEYWORD_DOCS) {
                    item.documentation = new vscode.MarkdownString(KEYWORD_DOCS[kw]);
                }
                completions.push(item);
            }
        }

        // 내장 함수 — snippet 형태로 삽입
        for (const [name, info] of Object.entries(BUILTIN_FUNCTIONS)) {
            if (name.startsWith(word)) {
                const item = new vscode.CompletionItem(name, vscode.CompletionItemKind.Function);
                item.detail = info.sig;
                item.documentation = new vscode.MarkdownString(
                    `\`\`\`sura\n${info.sig}\n\`\`\`\n\n${info.desc}`
                );
                item.insertText = new vscode.SnippetString(info.snippet);
                item.sortText = '0_' + name; // 함수를 키워드보다 위에
                completions.push(item);
            }
        }

        return completions;
    }
}

// ── 2. Signature Help (함수명( 입력 시 파라미터 힌트) ────────────────
class SuraSignatureHelpProvider implements vscode.SignatureHelpProvider {
    provideSignatureHelp(
        document: vscode.TextDocument,
        position: vscode.Position
    ): vscode.SignatureHelp | undefined {
        const lineText = document.lineAt(position.line).text.substring(0, position.character);

        // 현재 커서 앞에서 열린 괄호 찾기
        let depth = 0;
        let funcEnd = -1;
        for (let i = lineText.length - 1; i >= 0; i--) {
            if (lineText[i] === ')') depth++;
            else if (lineText[i] === '(') {
                if (depth === 0) { funcEnd = i; break; }
                depth--;
            }
        }
        if (funcEnd < 0) return undefined;

        // 함수 이름 추출
        const before = lineText.substring(0, funcEnd);
        const match = before.match(/([a-zA-Z_][a-zA-Z0-9_]*)$/);
        if (!match) return undefined;
        const funcName = match[1];
        if (!(funcName in BUILTIN_FUNCTIONS)) return undefined;

        const info = BUILTIN_FUNCTIONS[funcName];

        // 현재 인자 인덱스 (쉼표 개수)
        const argsStr = lineText.substring(funcEnd + 1);
        let argIndex = 0;
        let d = 0;
        for (const ch of argsStr) {
            if (ch === '(' || ch === '[') d++;
            else if (ch === ')' || ch === ']') d--;
            else if (ch === ',' && d === 0) argIndex++;
        }

        const sigInfo = new vscode.SignatureInformation(info.sig, new vscode.MarkdownString(info.desc));

        // 시그니처에서 파라미터 파싱
        const paramMatch = info.sig.match(/\(([^)]*)\)/);
        if (paramMatch && paramMatch[1]) {
            const params = paramMatch[1].split(',').map(p => p.trim());
            for (const p of params) {
                sigInfo.parameters.push(new vscode.ParameterInformation(p));
            }
        }

        const help = new vscode.SignatureHelp();
        help.signatures = [sigInfo];
        help.activeSignature = 0;
        help.activeParameter = Math.min(argIndex, sigInfo.parameters.length - 1);
        return help;
    }
}

// ── 3. 호버 ──────────────────────────────────────────────────────────
class SuraHoverProvider implements vscode.HoverProvider {
    provideHover(
        document: vscode.TextDocument,
        position: vscode.Position
    ): vscode.Hover | undefined {
        const wordRange = document.getWordRangeAtPosition(position);
        if (!wordRange) return undefined;
        const word = document.getText(wordRange);

        if (word in BUILTIN_FUNCTIONS) {
            const info = BUILTIN_FUNCTIONS[word];
            const md = new vscode.MarkdownString(
                `\`\`\`sura\n${info.sig}\n\`\`\`\n\n${info.desc}`
            );
            return new vscode.Hover(md, wordRange);
        }

        if (word in KEYWORD_DOCS) {
            return new vscode.Hover(new vscode.MarkdownString(KEYWORD_DOCS[word]), wordRange);
        }

        // 파일 내 함수 정의 찾기
        for (let i = 0; i < document.lineCount; i++) {
            const line = document.lineAt(i).text;
            const m = line.match(new RegExp(`func\\s+${word}\\s*\\(([^)]*)\\)`));
            if (m) {
                const params = m[1] || '';
                const md = new vscode.MarkdownString(
                    `\`\`\`sura\nfunc ${word}(${params})\n\`\`\`\n\n사용자 정의 함수 (${i + 1}번 줄)`
                );
                return new vscode.Hover(md, wordRange);
            }
        }

        return undefined;
    }
}

// ── 4. 정의 이동 ─────────────────────────────────────────────────────
class SuraDefinitionProvider implements vscode.DefinitionProvider {
    provideDefinition(
        document: vscode.TextDocument,
        position: vscode.Position
    ): vscode.Definition | undefined {
        const wordRange = document.getWordRangeAtPosition(position);
        if (!wordRange) return undefined;
        const word = document.getText(wordRange);

        for (let i = 0; i < document.lineCount; i++) {
            const line = document.lineAt(i).text;
            if (line.match(new RegExp(`^\\s*func\\s+${word}\\s*(\\(|do)`)) ||
                line.match(new RegExp(`^\\s*class\\s+${word}\\s*(extends|do)`))) {
                return new vscode.Location(document.uri, new vscode.Position(i, 0));
            }
        }
        return undefined;
    }
}

// ── 5. 포맷팅 ────────────────────────────────────────────────────────
class SuraFormattingProvider implements vscode.DocumentFormattingEditProvider {
    provideDocumentFormattingEdits(document: vscode.TextDocument): vscode.TextEdit[] {
        const edits: vscode.TextEdit[] = [];
        let indent = 0;
        const TAB = '    ';

        const DECREASE = /^\s*(end|else|elif|catch)\b/;
        const INCREASE = /\b(then|do)\s*$/;
        const NEUTRAL  = /^\s*(else|elif|catch)\b/;

        for (let i = 0; i < document.lineCount; i++) {
            const line = document.lineAt(i);
            const text = line.text.trim();
            if (text === '' || text.startsWith('//') || text.startsWith('#')) continue;

            if (DECREASE.test(text)) indent = Math.max(0, indent - 1);

            const expected = TAB.repeat(indent);
            const actual   = line.text.substring(0, line.text.length - line.text.trimStart().length);
            if (actual !== expected) {
                edits.push(vscode.TextEdit.replace(
                    new vscode.Range(i, 0, i, actual.length),
                    expected
                ));
            }

            if (INCREASE.test(text) && !NEUTRAL.test(text)) indent++;
        }
        return edits;
    }
}

// ── 6. 진단 (린팅) ───────────────────────────────────────────────────
class SuraDiagnosticProvider {
    private collection: vscode.DiagnosticCollection;

    constructor(context: vscode.ExtensionContext) {
        this.collection = vscode.languages.createDiagnosticCollection('sura');
        context.subscriptions.push(this.collection);
    }

    check(document: vscode.TextDocument) {
        if (document.languageId !== 'sura') return;
        const diags: vscode.Diagnostic[] = [];
        const blockStack: { keyword: string; line: number }[] = [];

        for (let i = 0; i < document.lineCount; i++) {
            const raw = document.lineAt(i).text;
            // 주석 이전 부분만
            const commentIdx = this.commentIndex(raw);
            const text = commentIdx >= 0 ? raw.substring(0, commentIdx).trim() : raw.trim();
            if (text === '') continue;

            // 괄호 불일치 (문자열 안 제외)
            const counts = this.countBrackets(text);
            if (counts.open !== counts.close) {
                diags.push(new vscode.Diagnostic(
                    new vscode.Range(i, 0, i, raw.length),
                    `괄호가 맞지 않습니다 (열림 ${counts.open}개, 닫힘 ${counts.close}개)`,
                    vscode.DiagnosticSeverity.Warning
                ));
            }

            // 블록 열기 추적
            if (/\b(if|while|for|foreach|func|class|try)\b/.test(text) &&
                /\b(then|do)\b/.test(text)) {
                blockStack.push({ keyword: text.match(/\b(if|while|for|foreach|func|class|try)\b/)![0], line: i });
            }
            if (/^\s*(else|elif|catch)\b/.test(raw)) {
                // 짝이 맞는 if/try가 없으면 경고
                if (blockStack.length === 0) {
                    diags.push(new vscode.Diagnostic(
                        new vscode.Range(i, 0, i, raw.length),
                        `대응하는 if/try가 없습니다`,
                        vscode.DiagnosticSeverity.Warning
                    ));
                }
            }
            if (/^\s*end\b/.test(text)) {
                if (blockStack.length > 0) blockStack.pop();
                else {
                    diags.push(new vscode.Diagnostic(
                        new vscode.Range(i, 0, i, raw.length),
                        '대응하는 블록 시작이 없는 end입니다',
                        vscode.DiagnosticSeverity.Warning
                    ));
                }
            }
        }

        // 닫히지 않은 블록
        for (const b of blockStack) {
            diags.push(new vscode.Diagnostic(
                new vscode.Range(b.line, 0, b.line, document.lineAt(b.line).text.length),
                `'${b.keyword}' 블록이 end로 닫히지 않았습니다`,
                vscode.DiagnosticSeverity.Warning
            ));
        }

        this.collection.set(document.uri, diags);
    }

    // 문자열 안을 무시하고 첫 번째 // 또는 # 위치 반환
    private commentIndex(text: string): number {
        let inStr = false;
        for (let i = 0; i < text.length; i++) {
            if (text[i] === '"' && (i === 0 || text[i-1] !== '\\')) inStr = !inStr;
            if (!inStr) {
                if (text[i] === '#') return i;
                if (text[i] === '/' && text[i+1] === '/') return i;
            }
        }
        return -1;
    }

    private countBrackets(text: string): { open: number; close: number } {
        let open = 0, close = 0, inStr = false;
        for (let i = 0; i < text.length; i++) {
            if (text[i] === '"' && (i === 0 || text[i-1] !== '\\')) { inStr = !inStr; continue; }
            if (inStr) continue;
            if (text[i] === '(') open++;
            else if (text[i] === ')') close++;
        }
        return { open, close };
    }
}

// ── 7. 스니펫 ────────────────────────────────────────────────────────
class SuraSnippetProvider implements vscode.CompletionItemProvider {
    provideCompletionItems(): vscode.CompletionItem[] {
        const snippets: vscode.CompletionItem[] = [];

        const add = (label: string, body: string, doc: string) => {
            const item = new vscode.CompletionItem(label, vscode.CompletionItemKind.Snippet);
            item.insertText = new vscode.SnippetString(body);
            item.documentation = doc;
            item.sortText = '9_' + label;
            snippets.push(item);
        };

        add('if',      'if ${1:조건} then\n\t$2\nend', '조건문');
        add('ife',     'if ${1:조건} then\n\t$2\nelse\n\t$3\nend', '조건문 (else 포함)');
        add('elif',    'elif ${1:조건} then\n\t$2', 'elif 분기');
        add('while',   'while ${1:조건} do\n\t$2\nend', '반복문');
        add('for',     'for ${1:i} in ${2:1} to ${3:10} do\n\t$4\nend', '범위 반복');
        add('foreach', 'foreach ${1:item} in ${2:arr} do\n\t$3\nend', '배열 순회');
        add('func',    'func ${1:함수명}(${2:인자}) do\n\t${3:return nil}\nend', '함수 정의');
        add('class',   'class ${1:클래스명} do\n\tfunc init(${2:인자}) do\n\t\t$3\n\tend\nend', '클래스 정의');
        add('try',     'try\n\t$1\ncatch ${2:err}\n\tprint(${2:err})\nend', '예외 처리');
        add('main',    '// main\n${1:x} is ${2:0}\n$3', '메인 코드');

        return snippets;
    }
}

// ── 활성화 ───────────────────────────────────────────────────────────
export function activate(context: vscode.ExtensionContext) {

    const SEL = { language: 'sura' };

    context.subscriptions.push(
        vscode.languages.registerCompletionItemProvider(SEL, new SuraCompletionProvider(), '.', '_', '('),
        vscode.languages.registerCompletionItemProvider(SEL, new SuraSnippetProvider()),
        vscode.languages.registerSignatureHelpProvider(SEL, new SuraSignatureHelpProvider(), '(', ','),
        vscode.languages.registerHoverProvider(SEL, new SuraHoverProvider()),
        vscode.languages.registerDefinitionProvider(SEL, new SuraDefinitionProvider()),
        vscode.languages.registerDocumentFormattingEditProvider(SEL, new SuraFormattingProvider()),
    );

    // 진단
    const diag = new SuraDiagnosticProvider(context);
    context.subscriptions.push(
        vscode.workspace.onDidOpenTextDocument(d => diag.check(d)),
        vscode.workspace.onDidChangeTextDocument(e => diag.check(e.document)),
        vscode.workspace.onDidSaveTextDocument(d => diag.check(d)),
    );
    vscode.workspace.textDocuments.forEach(d => diag.check(d));

    // 명령어: 파일 실행
    context.subscriptions.push(
        vscode.commands.registerCommand('sura.runFile', () => {
            const editor = vscode.window.activeTextEditor;
            if (!editor || editor.document.languageId !== 'sura') return;

            const filePath = editor.document.fileName;
            editor.document.save();

            const config = vscode.workspace.getConfiguration('sura');
            const enginePath = config.get<string>('enginePath') || 'SuraEngine2.exe';

            // 엔진 경로가 상대적이면 파일 디렉토리 기준
            const cwd = path.dirname(filePath);
            const engine = path.isAbsolute(enginePath)
                ? enginePath
                : path.join(cwd, enginePath);

            let term = vscode.window.terminals.find(t => t.name === 'Sura');
            if (!term) term = vscode.window.createTerminal('Sura');
            term.show();
            term.sendText(`"${engine}" "${filePath}"`);
        })
    );

    // 명령어: JIT 실행
    context.subscriptions.push(
        vscode.commands.registerCommand('sura.runJIT', () => {
            const editor = vscode.window.activeTextEditor;
            if (!editor || editor.document.languageId !== 'sura') return;

            const filePath = editor.document.fileName;
            editor.document.save();

            const cwd = path.dirname(filePath);
            const engine = path.join(cwd, 'SuraJIT.exe');

            let term = vscode.window.terminals.find(t => t.name === 'Sura JIT');
            if (!term) term = vscode.window.createTerminal('Sura JIT');
            term.show();
            term.sendText(`"${engine}" "${filePath}"`);
        })
    );

    context.subscriptions.push(
        vscode.commands.registerCommand('sura.format', () => {
            vscode.commands.executeCommand('editor.action.formatDocument');
        })
    );
}

export function deactivate() {}
