# Showcase 예제

언어의 핵심 기능을 짧게 보여주는 자기 검증 예제들입니다. 모든 파일은
`assert_eq`로 스스로 결과를 검증하며, VM과 `--jit` 양쪽에서 동작을
확인했습니다.

```powershell
.\SuraLanguage.exe examples\showcase\primes.sura
.\SuraLanguage.exe --jit examples\showcase\particle_jit.sura
```

| 파일 | 보여주는 것 |
| --- | --- |
| `particle_jit.sura` | 클래스 + 숫자 루프. JIT가 `step` 메서드를 네이티브 컴파일 |
| `primes.sura` | 에라토스테네스의 체 — 배열과 while 루프 |
| `shapes.sura` | 클래스 상속, 메서드 오버라이드, `use math` |
| `word_frequency.sura` | 딕셔너리 집계와 `keys()` 순회 |
| `grades.sura` | if/elif, `when ... in a to b`, try/catch, 배열 집계 |

입문 순서대로 배우려면 [../starter](../starter)를 먼저 보세요.
