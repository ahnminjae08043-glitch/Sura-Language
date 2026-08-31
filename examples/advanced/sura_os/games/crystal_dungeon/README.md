# Crystal Dungeon

SuraOS의 SVM32 가상 CPU에서 실행되는 파일 선택형 전투 게임이다. 전용 디스크 이미지에는 SVM32 언어 설치, 게임 소스, 선택 파일, 최근 점수와 최고 점수가 함께 저장된다.

## 시작

`sura_os` 폴더에서 실행한다.

```powershell
..\..\..\SuraLanguage.exe main.sura -- --interactive --disk games\crystal_dungeon\crystal-dungeon.disk.json
```

가상 셸에서 문 하나를 선택하고 게임을 실행한다.

```text
write /home/user/games/crystal-dungeon/choice.txt 1
runfile /home/user/games/crystal-dungeon/game.sasm
```

선택지는 다음과 같다.

- `1`: Ember Cave, 쉬움, 기본 보상 100
- `2`: Ancient Forest, 보통, 기본 보상 160
- `3`: Void Citadel, 어려움, 기본 보상 260

난수로 플레이어와 몬스터의 공격력이 정해진다. 최근 점수와 최고 점수는 다음 명령으로 확인한다.

```text
cat /home/user/games/crystal-dungeon/last-score.txt
cat /home/user/games/crystal-dungeon/high-score.txt
```

다른 문으로 반복 플레이한 뒤 `exit`하면 결과가 자동 저장된다.
