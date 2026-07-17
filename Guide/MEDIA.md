# 영상 → 문자 프레임

최종 업데이트: 2026-07-12

Sura의 `media` 모듈은 로컬 영상 파일을 밝기 기반 ASCII/UTF-8 문자 프레임으로 변환합니다. 컨테이너와 코덱 디코딩에는 선택 설치 항목인 FFmpeg를 사용하고, 픽셀→문자 변환·디더링·안전 제한은 Sura 런타임이 직접 처리합니다. 셸을 거치지 않고 FFmpeg 인자 배열을 직접 전달하며 네트워크 프로토콜은 허용하지 않습니다.

## 빠른 시작

```sura
use media

clip is media.ascii_frames("clip.mp4", {width: 80, fps: 8, max_frames: 300, charset: " .:-=+*#%@", dither: true})

print(clip.frames[0])
print("frames = {clip.frame_count}, size = {clip.width}x{clip.height}")
```

`media.video_to_text`와 `media.video_text_frames`는 `media.ascii_frames`의 별칭입니다. 결과 형식은 `sura.text-video.v1`이며 다음 필드를 가집니다.

- `frames`: 줄바꿈을 포함한 문자 프레임 배열
- `timestamps`: 각 샘플 프레임의 초 단위 시각
- `width`, `height`, `fps`, `frame_count`, `sampled_duration`
- `truncated`: 원본에 `max_frames`보다 많은 샘플 프레임이 있었는지 여부
- `charset`, `gamma`, `inverted`, `dithered`
- `backend`, `decoder`, `source`

## 옵션

| 옵션 | 기본값 | 범위/의미 |
|---|---:|---|
| `width` | `80` | 1..512 문자 |
| `height` | 자동 | 1..512, 지정하면 비율을 유지한 letterbox 사용 |
| `char_aspect` | `0.5` | 자동 높이에서 문자의 세로/가로 비율 보정, 0.1..2.0 |
| `fps` | `8` | 초당 샘플 프레임, 0.1..60 |
| `max_frames` | `300` | 반환할 최대 프레임, 1..10000 |
| `start` | `0` | 시작 시각(초) |
| `duration` | 없음 | 디코딩 구간(초) |
| `charset` | `" .:-=+*#%@"` | 어두운 픽셀→밝은 픽셀 순 UTF-8 glyph |
| `gamma` | `1` | 0.1..5.0, 1보다 크면 중간 밝기가 어두워짐 |
| `invert` | `false` | 문자 ramp 방향 반전 |
| `dither` | `false` | 결정적인 Floyd–Steinberg 디더링 |
| `ffmpeg` | 자동 | FFmpeg 실행 파일 경로 |
| `timeout_ms` | `120000` | 1000..600000ms |

`height`와 `char_aspect`는 동시에 지정할 수 없습니다. FFmpeg 탐색 순서는 옵션의 `ffmpeg`, 환경 변수 `SURA_FFMPEG`, `PATH`입니다.

```sura
use media

if not media.available() then
  throw "FFmpeg가 필요합니다"
end
```

## 한 프레임만 직접 변환

FFmpeg 없이 grayscale 숫자 또는 RGB/RGBA 픽셀 행렬을 바로 변환할 수 있습니다. 채널 범위는 0..255이고 RGBA는 검은 배경 위에 합성합니다.

```sura
use media

pixels is [[0, 85, 170, 255], [[0, 0, 0], [0, 255, 0], [255, 255, 255, 128], [255, 255, 255]]]

print(media.frame_to_text(pixels, {charset: " .#@"}))
```

## 안전성과 제한

- 입력은 존재하는 로컬 일반 파일이어야 합니다. URL 입력은 받지 않습니다.
- FFmpeg는 Windows `CreateProcessW`, POSIX `fork/execv`로 실행되어 파일명에 `&`, 공백 등이 있어도 셸 명령으로 해석되지 않습니다.
- decoder stderr는 픽셀 스트림과 분리되고 64 KiB까지만 진단에 보존됩니다.
- 디코딩 시간, 프레임 수, 프레임 픽셀 수, 임시 PGM 스트림, 최종 문자열 크기에 하드 상한이 있습니다.
- 변환은 현재 네이티브 Sura 런타임 전용입니다. JS/WASM target에서 FFmpeg 실행 parity를 보장하지 않습니다.

검증은 `tests/52_media_text_frames.sura`와 `tools/sura_media_smoke.ps1`이 담당합니다. 스모크 테스트는 FFmpeg 설치 여부에 의존하지 않고 가짜 decoder 실행 파일로 연속 PGM, NUL 픽셀, stderr 분리, 잘린 데이터, 실패 exit, timeout, 셸 주입 방지를 검사합니다.

## Bad Apple풍 오리지널 애니메이션

원본 영상 프레임이나 음악을 포함하지 않는 절차적 고대비 실루엣 예제가 들어 있습니다. 달의 위상, 사과, 춤추는 인물, 낙하 장면, 눈과 나비를 매 프레임 Sura 코드로 그립니다.

```powershell
sura examples/bad_apple_ascii.sura
sura examples/bad_apple_ascii.sura --smoke
```

두 번째 명령은 4개 장면을 빠르게 한 프레임씩 렌더링하는 검증 모드입니다. VM/JIT 회귀는 `tools/sura_bad_apple_ascii_smoke.ps1`로 실행합니다.

사용 권한이 있는 원본 영상의 프레임 순서를 그대로 문자화하려면 영상 자체를 저장소 밖에서 준비하고 다음 전용 wrapper를 사용합니다. 저장소에는 원본 영상이나 음악이 포함되지 않습니다.

```powershell
sura examples/bad_apple_from_video.sura -- "C:\path\to\authorized-video.mp4"
sura examples/bad_apple_from_video.sura -- "C:\path\to\authorized-video.mp4" --preview
sura examples/bad_apple_from_video.sura -- "C:\path\to\authorized-video.mp4" --convert-only
sura examples/bad_apple_from_video.sura -- "C:\path\to\authorized-video.mp4" --ffmpeg "C:\path\to\ffmpeg.exe"
```

이 wrapper는 96열, 30fps, 흑백 2단계 ramp로 최대 7000개 프레임을 변환합니다. 이는 소스에서 파생된 문자 표현이며 원본 픽셀·음향 자체와 동일한 복제본은 아닙니다.
