"use client";

import React, { useEffect, useRef, useState } from "react";
import {
  ArrowRight,
  BookOpen,
  Check,
  Clipboard,
  Download,
  ExternalLink,
  Menu,
  Terminal,
  X,
} from "lucide-react";
import { VERSION } from "./version.js";
import release from "./release.json";

if (release.schema !== "sura.site.release.v1" || release.version !== VERSION || release.store?.availability !== "public") {
  throw new Error("website release metadata does not match version.json");
}
const store = release.store;
const artifactLabels = ["Windows 설치기", "Windows 압축 파일", "VS Code 확장"];
const artifacts = release.artifacts.map((artifact, index) => ({
  ...artifact,
  label: artifactLabels[index] || "배포 파일",
  detail: `${artifact.bytes.toLocaleString("en-US")} bytes`,
}));

const examples = [
  {
    id: "starter",
    label: "프로젝트 시작",
    filename: "PowerShell",
    description: "생성, 실행, 테스트",
    code: [
      "surapkg new hello_sura",
      "cd hello_sura",
      "surapkg run -- Sura",
      "surapkg test",
    ].join("\n"),
  },
  {
    id: "basics",
    label: "기본 문법",
    filename: "hello.sura",
    description: "변수, 함수, 문자열 보간",
    code: [
      "func greet(name: string) -> string do",
      "  return \"안녕하세요, {name}\"",
      "end",
      "",
      "people is [\"민재\", \"Sura\"]",
      "for person in people do",
      "  print(greet(person))",
      "end",
    ].join("\n"),
  },
  {
    id: "objects",
    label: "객체",
    filename: "counter.sura",
    description: "class, 생성자, 메서드",
    code: [
      "class Counter do",
      "  func init() do",
      "    self.value is 0",
      "  end",
      "",
      "  func next() do",
      "    self.value += 1",
      "    return self.value",
      "  end",
      "end",
      "",
      "counter is new Counter()",
      "print(counter.next())",
    ].join("\n"),
  },
  {
    id: "media",
    label: "영상 문자화",
    filename: "video_text.sura",
    description: "FFmpeg 영상 → UTF-8 문자 프레임",
    code: [
      "use media",
      "",
      "clip is media.ascii_frames(\"clip.mp4\", {width: 80, fps: 8, max_frames: 300, dither: true})",
      "",
      "print(clip.frames[0])",
    ].join("\n"),
  },
  {
    id: "async",
    label: "동시성",
    filename: "async.sura",
    description: "작업 범위, timeout, cancellation",
    code: [
      "use async",
      "",
      "scope is async.scope()",
      "timer is async.sleep(50, scope)",
      "command is async.cmd(\"echo sura\", scope)",
      "",
      "result is async.scope_join(scope, 1000)",
      "print(result.closed)",
    ].join("\n"),
  },
];

const starterLessons = [
  ["01", "Hello", "출력과 문자열 보간", "01_hello.sura"],
  ["02", "값과 변수", "기본 값과 타입 힌트", "02_values.sura"],
  ["03", "제어 흐름", "조건문과 반복문", "03_control_flow.sura"],
  ["04", "함수", "매개변수와 반환값", "04_functions.sura"],
  ["05", "컬렉션", "배열과 딕셔너리", "05_collections.sura"],
  ["06", "클래스", "생성자와 메서드", "06_classes.sura"],
  ["07", "파일", "쓰기, 읽기, 삭제", "07_files.sura"],
  ["08", "JSON", "직렬화와 파싱", "08_json.sura"],
  ["09", "HTTP 도구", "URL과 상태 코드", "09_http_helpers.sura"],
  ["10", "비동기", "작업 생성과 대기", "10_async.sura"],
  ["11", "오류 처리", "try, catch, finally", "11_errors.sura"],
  ["12", "테스트", "내장 assertion", "12_testing.sura"],
];

const implementationRows = [
  ["실행", "Register bytecode와 register VM", "기본 실행 경로"],
  ["JIT", "자체 Win64 x86-64 emitter", "--jit, 지원 함수·메서드만 부분 컴파일"],
  ["타입", "Strict-by-default 점진적 타입 검사", "--legacy-types는 소스 실행에서만 선택 가능"],
  ["메모리", "NaN-boxed Value와 mark-sweep GC", "프로세스 전역 heap"],
  ["도구", "formatter, lint, test, profile, LSP, DAP", "Starter 프로젝트 생성과 VS Code 설정 포함"],
  ["연동", "FFI 1.2, plugin ABI 1.1, Python bridge", "기능별 외부 도구가 필요할 수 있음"],
];

const targetRows = [
  ["Native VM", "주 실행 경로", "전체 dynamic Value runtime"],
  ["Win64 JIT", "부분 지원", "Windows x64, warm-up된 지원 body"],
  ["JavaScript", "41 full · 2 ignored", "43개 AST node 분류 완료 · 일부 native module 미지원"],
  ["WebAssembly", "27 full · 14 partial · 2 ignored", "전체 dynamic Value parity는 아직 INCOMPLETE"],
];

const verificationRows = [
  ["Starter examples", "외부 의존성 없이 12/12 실행 PASS"],
  ["Stable runtime suite", "VM 107/107 · JIT 107/107 PASS"],
  ["Release evidence", "157/157 PASS · 2026-07-17 갱신"],
  ["Goal audit", "126/127 · 99.2% · INCOMPLETE"],
  ["JavaScript target", "41 full · 0 partial · 0 missing · 2 ignored"],
  ["WebAssembly target", "27 full · 14 partial · 0 missing · 2 ignored"],
  ["Bytecode validation", "Stable VM/JIT suites PASS · standalone suite not rerun"],
  ["Async / FFI", "Stable VM/JIT suites PASS · standalone FFI suite not rerun"],
];

function CopyButton({ text, label = "복사" }) {
  const [copied, setCopied] = useState(false);

  const copy = async () => {
    try {
      await navigator.clipboard.writeText(text);
      setCopied(true);
      window.setTimeout(() => setCopied(false), 1400);
    } catch {
      setCopied(false);
    }
  };

  return (
    <button className="copy-button" type="button" onClick={copy} aria-label={`${label}: ${text}`}>
      {copied ? <Check size={14} aria-hidden="true" /> : <Clipboard size={14} aria-hidden="true" />}
      {copied ? "복사됨" : label}
    </button>
  );
}

function CodeBlock({ filename, code, output, className = "" }) {
  return (
    <div className={`code-block ${className}`.trim()}>
      <div className="code-head">
        <span><Terminal size={14} aria-hidden="true" /> {filename}</span>
        <CopyButton text={code} />
      </div>
      <pre><code>{code}</code></pre>
      {output && <div className="code-output"><span>실행 결과</span><code>{output}</code></div>}
    </div>
  );
}

function SectionHeading({ number, eyebrow, title, description }) {
  return (
    <div className="section-heading" data-reveal>
      <div className="section-number">{number}</div>
      <div>
        <p className="section-eyebrow">{eyebrow}</p>
        <h2>{title}</h2>
        {description && <p className="section-description">{description}</p>}
      </div>
    </div>
  );
}

export default function App() {
  const [menuOpen, setMenuOpen] = useState(false);
  const [activeExample, setActiveExample] = useState(examples[0]);
  const progressRef = useRef(null);
  const closeMenu = () => setMenuOpen(false);

  useEffect(() => {
    const root = document.documentElement;
    const targets = Array.from(document.querySelectorAll("[data-reveal]"));
    const reduceMotion = window.matchMedia("(prefers-reduced-motion: reduce)").matches;
    root.classList.add("motion-ready");

    let observer;
    if (reduceMotion || !("IntersectionObserver" in window)) {
      targets.forEach((target) => target.classList.add("is-visible"));
    } else {
      observer = new IntersectionObserver((entries) => {
        entries.forEach((entry) => {
          if (!entry.isIntersecting) return;
          entry.target.classList.add("is-visible");
          observer.unobserve(entry.target);
        });
      }, { threshold: 0.12, rootMargin: "0px 0px -8% 0px" });
      targets.forEach((target) => observer.observe(target));
    }

    let frame = 0;
    const updateProgress = () => {
      frame = 0;
      const distance = document.documentElement.scrollHeight - window.innerHeight;
      const progress = distance > 0 ? Math.min(1, Math.max(0, window.scrollY / distance)) : 0;
      progressRef.current?.style.setProperty("--scroll-progress", String(progress));
    };
    const onScroll = () => {
      if (!frame) frame = window.requestAnimationFrame(updateProgress);
    };
    updateProgress();
    window.addEventListener("scroll", onScroll, { passive: true });
    window.addEventListener("resize", onScroll);

    return () => {
      observer?.disconnect();
      if (frame) window.cancelAnimationFrame(frame);
      window.removeEventListener("scroll", onScroll);
      window.removeEventListener("resize", onScroll);
      root.classList.remove("motion-ready");
    };
  }, []);

  return (
    <div className="site-shell">
      <a className="skip-link" href="#main-content">본문 바로가기</a>
      <div ref={progressRef} className="scroll-progress" aria-hidden="true" />

      <header className="site-header">
        <a className="brand" href="#top" aria-label="Sura 홈">
          <img src="/sura-logo.png" alt="" />
          <span><strong>Sura</strong><small>Language {VERSION}</small></span>
        </a>

        <button
          className="menu-button"
          type="button"
          onClick={() => setMenuOpen((open) => !open)}
          aria-expanded={menuOpen}
          aria-controls="main-navigation"
          aria-label={menuOpen ? "메뉴 닫기" : "메뉴 열기"}
        >
          {menuOpen ? <X aria-hidden="true" /> : <Menu aria-hidden="true" />}
        </button>

        <nav id="main-navigation" className={menuOpen ? "main-nav open" : "main-nav"} aria-label="주요 메뉴">
          <a href="#quickstart" onClick={closeMenu}>시작하기</a>
          <a href="#examples" onClick={closeMenu}>예제</a>
          <a href="#implementation" onClick={closeMenu}>구현 범위</a>
          <a href="#downloads" onClick={closeMenu}>다운로드</a>
          <a className="nav-reference" href="/reference.html" onClick={closeMenu}>레퍼런스 <ExternalLink size={13} aria-hidden="true" /></a>
        </nav>
      </header>

      <main id="main-content">
        <section className="hero" id="top">
          <div className="hero-copy">
            <p className="hero-version">SURA LANGUAGE / {VERSION} / MICROSOFT STORE RELEASE</p>
            <h1><span>Sura Language</span></h1>
            <p className="hero-lead">만들고, 실행하고, 끝까지 검증하는 언어.</p>
            <p className="hero-description">
              <code>.sura</code> 소스를 register bytecode로 컴파일해 register VM에서 실행합니다.
              기본 타입 검사는 strict이며, Windows x64에서는 자체 JIT를 선택해서 사용할 수 있습니다.
              Microsoft Store 설치와 Starter 프로젝트로 바로 시작할 수 있습니다.
            </p>
            <div className="hero-actions">
              <a className="button primary" href={store.url} target="_blank" rel="noreferrer">Microsoft Store에서 설치 <ExternalLink size={17} aria-hidden="true" /></a>
              <a className="button secondary" href="#quickstart">10분 시작하기 <ArrowRight size={17} aria-hidden="true" /></a>
              <a className="button secondary" href="/reference.html"><BookOpen size={17} aria-hidden="true" /> 레퍼런스 보기</a>
            </div>
            <dl className="hero-meta">
              <div><dt>기본 실행기</dt><dd>Register VM</dd></div>
              <div><dt>선택형 JIT</dt><dd>Win64 x86-64</dd></div>
              <div><dt>진단 언어</dt><dd>English / 한국어</dd></div>
            </dl>
          </div>

          <div className="hero-store" aria-label="Sura Language Microsoft Store 공개">
            <div className="hero-store-art">
              <img src="/store-super-hero.png" alt="Sura Language 로고, 언어 소개와 실제 Sura 코드 예제가 담긴 16대9 공식 미리보기" />
            </div>
            <div className="hero-store-summary">
              <div>
                <span><Check size={14} aria-hidden="true" /> MICROSOFT STORE · PUBLIC</span>
                <strong>Sura Language {VERSION}</strong>
                <small>SuraTeam · 무료 · Windows x64</small>
              </div>
              <a href={store.url} target="_blank" rel="noreferrer" aria-label="Microsoft Store에서 Sura Language 열기">
                Store 열기 <ExternalLink size={15} aria-hidden="true" />
              </a>
            </div>
          </div>
        </section>

        <section className="section quickstart" id="quickstart">
          <SectionHeading
            number="01"
            eyebrow="QUICK START"
            title="프로젝트를 만들고 테스트까지"
            description="일반적인 Sura 프로그램의 생성, 실행, 테스트에는 Python, Node.js, CMake, FFmpeg가 필요하지 않습니다."
          />
          <div className="steps">
            <article data-reveal style={{ "--reveal-delay": "0ms" }}>
              <span>1</span>
              <div>
                <h3>Microsoft Store에서 설치</h3>
                <p>Microsoft Store에 공개된 SuraTeam의 무료 Windows 앱입니다.</p>
                <a className="inline-action" href={store.url} target="_blank" rel="noreferrer">
                  <ExternalLink size={15} aria-hidden="true" /> Store 페이지 열기
                </a>
              </div>
            </article>
            <article data-reveal style={{ "--reveal-delay": "70ms" }}>
              <span>2</span>
              <div>
                <h3>새 PowerShell에서 확인</h3>
                <div className="command-row"><code>sura --version</code><CopyButton text="sura --version" /></div>
                <div className="command-row"><code>sura --repl</code><CopyButton text="sura --repl" /></div>
              </div>
            </article>
            <article data-reveal style={{ "--reveal-delay": "140ms" }}>
              <span>3</span>
              <div>
                <h3>Starter 프로젝트 만들기</h3>
                <div className="command-row"><code>surapkg new my_app</code><CopyButton text="surapkg new my_app" /></div>
                <div className="command-row"><code>cd my_app</code><CopyButton text="cd my_app" /></div>
              </div>
            </article>
            <article data-reveal style={{ "--reveal-delay": "210ms" }}>
              <span>4</span>
              <div>
                <h3>실행하고 테스트하기</h3>
                <div className="command-row"><code>surapkg run</code><CopyButton text="surapkg run" /></div>
                <div className="command-row"><code>surapkg test</code><CopyButton text="surapkg test" /></div>
              </div>
            </article>
          </div>
          <div className="starter-lessons" data-reveal>
            <div className="starter-lessons-head">
              <div>
                <p className="section-eyebrow">12 OFFLINE LESSONS</p>
                <h3>기초부터 테스트까지 순서대로</h3>
              </div>
              <a href="/examples/starter/README.md">전체 안내 보기 <ArrowRight size={14} aria-hidden="true" /></a>
            </div>
            <div className="starter-lesson-grid">
              {starterLessons.map(([number, title, detail, file]) => (
                <a key={file} href={`/examples/starter/${file}`}>
                  <span>{number}</span>
                  <strong>{title}</strong>
                  <small>{detail}</small>
                </a>
              ))}
            </div>
          </div>
          <p className="dependency-note" data-reveal><strong>기능별 선택 의존성:</strong> 영상 처리는 FFmpeg, Python 연동은 Python, JavaScript 타깃과 VS Code 도구 검증은 Node.js가 필요합니다.</p>
        </section>

        <section className="section examples-section" id="examples">
          <SectionHeading
            number="02"
            eyebrow="EXAMPLES"
            title="실제로 쓰는 문법"
            description="Starter 명령과 레퍼런스의 실제 호출 형태를 짧은 예제로 정리했습니다."
          />
          <div className="example-tabs" role="tablist" aria-label="Sura 예제 선택" data-reveal>
            {examples.map((example) => (
              <button
                key={example.id}
                type="button"
                role="tab"
                aria-selected={activeExample.id === example.id}
                className={activeExample.id === example.id ? "active" : ""}
                onClick={() => setActiveExample(example)}
              >
                <strong>{example.label}</strong>
                <small>{example.description}</small>
              </button>
            ))}
          </div>
          <CodeBlock key={activeExample.id} className="example-code-switch" filename={activeExample.filename} code={activeExample.code} />
        </section>

        <section className="section implementation-section" id="implementation">
          <SectionHeading
            number="03"
            eyebrow="IMPLEMENTATION"
            title="구현된 범위와 경계"
            description="지원하는 기능과 아직 부분 구현인 타깃을 같은 화면에서 확인할 수 있습니다."
          />

          <div className="implementation-grid">
            <div className="fact-list" aria-label="Sura 구현 정보">
              {implementationRows.map(([name, value, note], index) => (
                <article key={name} data-reveal style={{ "--reveal-delay": `${index * 45}ms` }}>
                  <h3>{name}</h3>
                  <div><strong>{value}</strong><p>{note}</p></div>
                </article>
              ))}
            </div>

            <div className="support-panel" data-reveal>
              <div className="panel-head"><h3>실행 타깃</h3><a href="/reference.html#targets">자세히 보기 <ArrowRight size={14} aria-hidden="true" /></a></div>
              <div className="support-table">
                {targetRows.map(([target, status, detail]) => (
                  <div key={target}>
                    <strong>{target}</strong>
                    <span>{status}</span>
                    <p>{detail}</p>
                  </div>
                ))}
              </div>
            </div>
          </div>
        </section>

        <section className="section verification-section" id="verification">
          <SectionHeading
            number="04"
            eyebrow="VERIFICATION"
            title="통과 기록과 성능 수치"
            description="좋은 결과만 고르지 않고 현재 측정값과 미달한 목표도 함께 공개합니다."
          />
          <div className="verification-layout">
            <div className="verification-list">
              {verificationRows.map(([name, result], index) => <div key={name} data-reveal style={{ "--reveal-delay": `${index * 55}ms` }}><span>{name}</span><strong>{result}</strong></div>)}
            </div>
            <div className="benchmark-note" data-reveal>
              <p className="mono-label">2026-07-17 · i5-12400F · 동일 100,000-step inner physics loop · 각 5 runs</p>
              <table>
                <thead><tr><th>작업</th><th>Sura JIT</th><th>C++ -O3</th><th>비율</th></tr></thead>
                <tbody>
                  <tr><td>Vec2 물리 루프</td><td>0.090 ms</td><td>0.050 ms</td><td>1.80×</td></tr>
                  <tr><td>Vec3 물리 루프</td><td>0.104 ms</td><td>0.050 ms</td><td>2.08×</td></tr>
                </tbody>
              </table>
              <p>두 결과 모두 같은 고정 물리 루프 범위와 최종 축 값을 확인한 fair-scope 기록입니다. 특정 최적화 경로의 결과이며 언어 전체가 C++보다 1.80~2.08배 느리다는 뜻은 아닙니다. 전체 목표 감사에서 남은 1개 항목은 WASM의 완전한 dynamic AST/bytecode lowering입니다.</p>
              <div className="record-links" aria-label="최신 검증 원본">
                <a href="/records/native-performance.json">성능 원본 JSON</a>
                <a href="/records/release-evidence.json">157개 검증 기록</a>
                <a href="/records/goal-audit.json">목표 감사</a>
                <a href="/records/target-lowering-audit.json">타깃 감사</a>
              </div>
            </div>
          </div>
        </section>

        <section className="section downloads-section" id="downloads">
          <SectionHeading
            number="05"
            eyebrow="DOWNLOADS"
            title={`Windows x64 · ${VERSION}`}
            description="파일명, 크기, SHA-256을 공개 배포 파일과 맞춰 표시합니다."
          />

          <div className="store-approved" data-reveal>
            <div>
              <strong>Microsoft Store 승인·공개 완료</strong>
              <p>게시자 {store.publisher}, 제품 ID {store.product_id}, 무료 앱으로 확인했습니다. 기본 설치는 Microsoft Store를 사용합니다.</p>
            </div>
            <a className="button primary" href={store.url} target="_blank" rel="noreferrer">Store에서 설치 <ExternalLink size={16} aria-hidden="true" /></a>
          </div>

          <div className="download-warning" data-reveal>
            <strong>아래 파일은 직접 다운로드용 보조 경로입니다.</strong>
            <p>직접 다운로드 EXE는 Microsoft Store 패키지와 별개이며 현재 Authenticode 서명이 없습니다. Chrome 또는 Windows가 경고할 수 있으므로 일반 사용자는 위 Store 설치를 권장합니다.</p>
          </div>

          <div className="download-list">
            {artifacts.map((artifact, index) => (
              <article key={artifact.name} data-reveal style={{ "--reveal-delay": `${index * 65}ms` }}>
                <div className="download-title"><span>{artifact.label}</span><h3>{artifact.name}</h3><p>{artifact.detail}</p></div>
                <div className="download-hash"><span>SHA-256</span><code>{artifact.sha256}</code><CopyButton text={artifact.sha256} label="해시 복사" /></div>
                <a className="button download-button" href={`/downloads/${artifact.name}`} download><Download size={16} aria-hidden="true" /> 직접 받기</a>
              </article>
            ))}
          </div>

          <div className="release-links" data-reveal>
            <a href="/downloads/SHA256SUMS.txt">SHA256SUMS.txt <ExternalLink size={13} aria-hidden="true" /></a>
            <a href={`/downloads/release-${VERSION}.json`}>release JSON <ExternalLink size={13} aria-hidden="true" /></a>
            <a href={`/downloads/verification-${VERSION}.json`}>verification JSON <ExternalLink size={13} aria-hidden="true" /></a>
            <a href="/downloads/README-KO.txt">설치 안내 <ExternalLink size={13} aria-hidden="true" /></a>
          </div>
        </section>

        <section className="reference-section" data-reveal>
          <p>LANGUAGE REFERENCE / HTML + JSON</p>
          <div>
            <h2>문법부터 528개 API 서명까지 한 문서에 정리했습니다.</h2>
            <p>검색 가능한 목차, 실행 예제, CLI 전체 목록, 외부 의존성, 구현 제한, 검증 기록과 구조화 JSON을 함께 제공합니다.</p>
          </div>
          <a className="button reference-button" href="/reference.html">레퍼런스 열기 <ArrowRight size={17} aria-hidden="true" /></a>
        </section>
      </main>

      <footer data-reveal>
        <a className="brand footer-brand" href="#top"><img src="/sura-logo.png" alt="" /><span><strong>Sura</strong><small>Language {VERSION}</small></span></a>
        <p>C++ · Register VM · Win64 x86-64 JIT</p>
        <div><a href="/reference.html">레퍼런스</a><a href="#downloads">다운로드</a><a href="#verification">검증</a></div>
      </footer>
    </div>
  );
}
