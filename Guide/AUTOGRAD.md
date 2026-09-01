# Sura 네이티브 Tensor와 자동미분

최종 업데이트: 2026-07-12

`use autograd`는 row-major 연속 메모리의 typed Tensor와 1차 reverse-mode 자동미분을 제공한다. `float64`, `float32`, `float16`, `bfloat16` packed 저장 형식을 지원하며 기본값은 `float64`다. 작은 모델, 사용자 정의 손실, 수치 실험, Transformer 블록을 Sura 안에서 직접 조합할 때 사용하는 저수준 API다. 미리 구성된 dense MLP가 필요하면 더 단순한 [`nn` 가이드](AI.md)를, causal Transformer를 만들려면 [`Transformer 가이드`](TRANSFORMER.md)를 참고한다.

Sura 1.8의 resident CUDA Tensor는 `float32`, `float16`, `bfloat16` storage를 지원하며 f16/bf16 payload는 실제 2-byte/element다. typed projection은 저정밀 storage를 직접 읽고 f32로 누적·출력하며 persistent gradient는 f32다. 저정밀 trainable parameter는 f32 master weight에서 SGD/Adam을 계산한 뒤 visible storage로 다시 pack한다. 현재 나머지 resident elementwise, GELU/LayerNorm, embedding, attention, sparse CE와 reduction kernel은 f32-only라서 저정밀 입력을 launch 전에 거부한다. `autocast`는 storage를 바꾸지 않고 matmul compute plan만 선택한다. causal attention에는 no-O(T²)-workspace fused recomputation이 있지만 shared-memory tiled/Tensor Core·저정밀 FlashAttention은 아니다. 일반 CUDA broadcasting, batched-right matmul, CUDA token/target Tensor 입력·softmax·dense/one-hot `cross_entropy`와 NCCL도 아직 없다. 전체 경계는 [`GPU_AND_SCALE.md`](GPU_AND_SCALE.md)에 정리되어 있다.

## 선형 회귀 학습

```sura
use autograd

x is autograd.tensor([[0], [1], [2], [3]])
y is autograd.tensor([[1], [3], [5], [7]])
w is autograd.parameter([[0]])
b is autograd.parameter([0])
parameters is [w, b]

repeat 400 do
  autograd.zero_grad(parameters)
  prediction is autograd.linear(x, w, b)
  loss is autograd.mse(prediction, y)
  autograd.backward(loss)
  autograd.adam(parameters, 0.05)
end

prediction is autograd.linear(x, w, b)
loss is autograd.mse(prediction, y)
print(autograd.data(w))
print(autograd.data(b))
print(autograd.item(loss))
```

실행 가능한 파일은 `examples/native_autograd.sura`에 있다. 기본 연산, 수치 gradient check, 실제 학습은 각각 `tests/13_autograd_core.sura`, `tests/14_autograd_gradcheck.sura`, `tests/15_autograd_training.sura`에서 검증한다.

## Tensor와 그래프 규칙

- 데이터는 row-major 연속 packed 버퍼에 저장된다. 지원 dtype는 `float64`, `float32`, `float16`, `bfloat16`이며 `dtype(tensor)`와 `storage_bytes(tensor)`로 확인한다. CPU kernel은 값을 `double`로 읽어 계산하고 결과를 선택한 dtype로 다시 양자화하며, CPU gradient와 optimizer 상태도 `double`이다.
- `autograd.tensor(data, true)`와 `autograd.parameter(data)`는 gradient를 받는 leaf Tensor를 만든다. `autograd.requires_grad(tensor)`로 현재 추적 여부를 확인한다.
- `tensor`, `parameter`, `zeros`, `ones`, `randn`의 options에 `{device: "cuda"}` 또는 `{device: "cuda:N"}`를 지정하면 resident CUDA Tensor를 만든다. CUDA dtype 기본값은 `float32`이며 resident storage는 `float32`, `float16`, `bfloat16`을 허용한다. `SURA_CUDA_DEVICE`로 프로세스 장치를 시작 전에 선택하며 한 프로세스 안에서 여러 CUDA 장치를 섞을 수는 없다.
- `device(tensor)`는 `cpu` 또는 `cuda:N`을 반환한다. `to(tensor, device)`는 장치 사이를 명시적으로 복사하며 같은 장치를 요청하면 원래 Tensor를 반환한다. CUDA 학습 leaf는 `to()`로 CPU leaf를 옮기기보다 처음부터 `parameter(..., {device: "cuda"})`로 만들어야 한다. 현재 CUDA backward는 CPU/CUDA가 섞인 graph를 거부한다.
- 중첩 배열은 직사각형이어야 한다. 빈 차원, ragged/cyclic 배열, NaN/Infinity는 거부한다.
- CPU elementwise 이항 연산은 NumPy 방식의 trailing-dimension broadcasting을 지원한다. resident CUDA의 Tensor-Tensor `add`/`sub`/`mul`은 현재 두 shape이 같아야 하지만, `add([..., features], [features])`는 표준 linear bias로 특별 지원한다. 그 bias backward는 모든 앞쪽 행·batch·token 위치를 GPU에서 줄인다. 숫자 scalar와의 덧셈·뺄셈·곱셈 및 Tensor를 0이 아닌 숫자 scalar로 나누는 연산은 지원하지만 일반 CUDA broadcasting과 Tensor-Tensor division은 아직 없다.
- `reshape(tensor, shape)`는 원소 순서를 유지한다. shape에는 정확히 하나의 `-1`을 넣어 해당 차원의 크기를 추론할 수 있다.
- `matmul`은 rank 2 이상 Tensor의 마지막 두 차원을 행렬로 취급하고, 앞쪽 batch 차원을 trailing 방식으로 broadcast한다.
- 두 입력이 resident CUDA Tensor이면 `matmul(left, right, [options])`는 contiguous `float32` left `[..., rows, inner]`와 rank-2 right `[inner, cols]`의 shared-weight projection forward/backward를 GPU에서 실행하고 결과 `[..., rows, cols]`도 GPU에 남긴다. 기본 `compute_dtype`은 `float32`다. `float16`/`bfloat16`을 명시하거나 autocast로 선택하면 f32 operand를 저정밀 compute 형식으로 반올림해 f32로 누적·출력하며, forward가 선택한 형식은 두 backward matmul에도 고정된다. 호환 cuBLAS의 `cublasGemmEx` FAST compute를 우선하고 미지원이면 동일 의미의 reference PTX로 fallback한다. `cuda_info()`와 dtype별 matmul/cublas-fast/fallback counter로 실제 dispatch를 확인한다. right에도 batch prefix가 있는 일반 batched matmul과 batch-prefix broadcasting은 아직 CPU 전용이다. CPU rank-2 `float32` 입력에 `matmul(left, right, {backend: "cuda"})`를 지정하는 기존 one-shot 경로도 남아 있지만, 매 호출마다 H2D/D2H copy를 하고 CPU Tensor를 반환하므로 resident 학습 경로가 아니다.
- `transpose(tensor)`는 기본적으로 마지막 두 축을 바꾼다. `transpose(tensor, axis1, axis2)`는 음수 축을 포함한 임의의 서로 다른 두 축을 바꾼다.
- `sum`과 `mean`은 현재 Tensor 전체를 scalar로 축약한다. resident CUDA forward를 지원하며, CUDA backward에서는 현재 이 reduction이 scalar loss graph의 루트여야 한다. `mse`는 `sub` → `mul` → `mean` 합성으로 이 조건을 만족한다. 마지막 축 정규화는 CPU와 resident CUDA `float32`의 `layer_norm`을 사용한다.
- `backward`는 기본적으로 사용한 그래프를 해제한다. 같은 그래프에서 다시 역전파해야 할 때만 세 번째 인수 `true`로 `retain_graph`를 지정한다.
- leaf gradient는 누적된다. CUDA leaf gradient도 GPU allocation에 계속 남아 다음 backward와 합쳐지며, `grad(tensor)`를 호출할 때만 host 배열로 복사한다. `grad_info(tensor)`는 payload를 복사하지 않고 `present`, gradient `dtype`/`device`, `elements`, `storage_bytes`, loss `scale`, `scaled`, `leaf`, `requires_grad`, `optimizer_ready` metadata를 반환한다. CUDA gradient는 실제 f32 allocation, CPU gradient는 Tensor storage dtype와 무관한 f64(`double`) allocation으로 보고된다. scaled gradient는 같은 declared scale끼리만 누적할 수 있고 optimizer 전에 반드시 unscale해야 한다. 학습 반복마다 `zero_grad`를 먼저 호출한다.
- forward 이후 optimizer가 같은 Tensor를 갱신하면 저장된 버전이 달라지므로 이전 그래프의 backward는 안전하게 오류가 난다.
- `json.stringify(tensor)`는 shape에 맞춘 중첩 숫자 배열을 기록한다. gradient, 그래프, optimizer 상태는 직렬화하지 않으므로 로드한 숫자 배열은 `autograd.tensor`나 `autograd.parameter`로 다시 감싼다.
- 여러 Tensor와 optimizer 상태를 이어서 학습할 목적으로 저장할 때는 JSON 대신 `save_checkpoint`와 `load_checkpoint`를 사용한다.

비 scalar 출력에서 역전파를 시작할 때는 출력과 같은 shape의 gradient를 명시한다.

```sura
use autograd

x is autograd.parameter([1, 2, 3])
y is autograd.mul(x, x)
autograd.backward(y, [1, 1, 1])
print(autograd.grad(x))
```

## CUDA mixed compute와 loss scaling

`autograd.autocast()`는 현재 thread의 `{enabled, dtype}`를 반환한다. bool, `"float16"`/`"bfloat16"` 문자열 또는 options dict로 바꾸면 이전 상태를 반환하므로 중첩 범위를 안전하게 복원할 수 있다. autocast는 resident CUDA `matmul`과 이를 호출하는 `linear`에만 적용되고 CPU 연산과 다른 CUDA 연산의 dtype를 바꾸지 않는다. 명시적 `{compute_dtype: "float32"|"float16"|"bfloat16"}` 옵션은 `matmul(left, right, options)`만 받으며 autocast보다 우선한다. `linear(input, weight, [bias])`는 options 인수가 없으므로 호출 시점의 현재 autocast 상태를 사용한다.

```sura
previous is autograd.autocast({enabled: true, dtype: "bfloat16"})
prediction is autograd.linear(input, weight, bias)
loss is autograd.cross_entropy_ids(prediction, targets)
autograd.autocast(previous)
```

`autocast` 자체는 compute-only라서 Tensor storage를 바꾸지 않는다. 별도로 실제 f16/bf16 CUDA storage를 생성하거나 `cast`할 수 있으며 typed matmul은 이를 직접 읽는다. matmul output과 gradient는 f32이고, low-input backward의 두 mixed GEMM도 f32 compute/output을 사용한다. cuBLAS FAST counter는 Tensor-Core-eligible 요청 성공 증거이지 모든 shape의 실제 Tensor Core instruction 실행을 단독으로 증명하지는 않는다.

loss scaling은 reduction graph를 곱셈 노드로 감싸지 않고 scalar root seed에서 시작한다.

```sura
scale is 65536
autograd.zero_grad(parameters)
autograd.backward_scaled(loss, scale)
status is autograd.unscale_gradients(parameters)
if status.found_inf then
  autograd.zero_grad(parameters)
  scale is scale / 2
else
  autograd.adam(parameters, 0.001)
end
```

`backward_scaled(loss, scale, [retain_graph])`는 resident CUDA scalar loss만 받는다. scale은 양의 유한한 f32로 표현 가능해야 하고 `1`이면 안 되며, 그 역수도 양의 유한한 f32로 표현 가능해야 한다. scale 1의 일반 역전파에는 `backward()`를 사용한다. 같은 leaf에는 같은 declared scale만 누적할 수 있다.

`unscale_gradients(parameters, [scale])`의 parameter 목록 자체는 비어 있을 수 없다. scale을 생략하면 목록에 있는 0이 아닌 모든 gradient scale이 같아야 하며, 명시하면 0이 아닌 모든 scale과 정확히 일치해야 한다. 실제 후보를 나눌 때 그 역수도 양의 유한한 f32로 표현 가능해야 한다. 이미 scale 1인 gradient는 다시 unscale하지 않고 오류로 거부한다. 아직 gradient가 없거나 `zero_grad`가 만든 scale-0 buffer뿐인 유효한 목록은 `gradient_tensors: 0`, `committed: true`인 성공 no-op이며 finite-status D2H를 만들지 않는다. 실제 후보가 있으면 모든 f32 unscaled gradient를 별도 allocation에 만들고 공유 4-byte finite status를 한 번 읽는다. 하나라도 NaN/Inf이면 `{finite: false, found_inf: true, committed: false}`를 반환하고 원래 scaled gradient 전체를 보존한다. 모두 유한할 때만 전부 동시에 교체하고 optimizer-ready scale 1로 바꾼다. SGD/Adam은 unscale되지 않은 gradient를 step 전에 거부하며 `zero_grad`는 scale-neutral zero 상태로 되돌린다. dynamic growth/backoff 정책과 scale 저장은 사용자 학습 loop가 관리한다.

`grad_info`는 이 상태를 payload D2H 없이 확인한다. `optimizer_ready`는 Tensor가 leaf이고 `requires_grad: true`이면서 gradient가 없거나 scale이 0/1인 기본 optimizer 자격을 뜻한다. gradient가 있으면서 scale이 0/1이 아니면 `scaled: true`, `optimizer_ready: false`다. 이 metadata는 실제 optimizer 호출의 전체 parameter/device/state 검증을 대신하지 않는다.

## Transformer 핵심 연산

다음 연산은 모두 자동미분 그래프를 보존한다.

- `gelu(tensor)`는 `x * Phi(x)` exact-form GELU를 원소별로 계산하며 일반적인 tanh approximation을 사용하지 않는다. CPU와 resident CUDA `float32` 모두 forward/backward를 지원하고 CUDA 결과와 gradient는 관찰 전까지 device에 남는다.
- `layer_norm(tensor, [weight], [bias], [epsilon])`은 마지막 축의 각 행을 독립적으로 정규화한다. 기본 epsilon은 `0.00001`이며, 선택적인 affine weight와 bias의 shape은 마지막 차원과 같은 1차원이어야 한다. 필요하지 않은 선택 인수는 `nil`로 넘길 수 있다. resident CUDA `float32` forward/backward는 input과 선택적 weight/bias gradient를 지원한다. gradient가 필요한 output은 forward의 행별 mean과 reciprocal standard deviation을 device에 저장하고, backward는 input/affine gradient에 필요한 행·feature reduction을 correctness-first 방식으로 다시 계산한다. `retain_graph`가 아니면 성공한 backward 뒤 saved state를 해제한다. 이 saved state와 output/gradient는 host 관찰 전까지 device-only지만, 현재 one-thread-per-row/feature reduction은 기능 경로이지 고성능 LayerNorm 구현이 아니다.
- `embedding(token_ids, weight)`은 숫자 배열 또는 gradient를 요구하지 않는 CPU Tensor의 정수 token ID를 `[vocabulary, dimensions]` weight에서 조회한다. CPU와 resident CUDA `float32` weight를 지원한다. CUDA 경로는 host에서 ID shape·정수 여부·vocabulary 범위를 검증하고 raw `uint32`로 pack해 매 forward에 한 번 H2D한다. output이 소유한 device ID buffer를 backward가 다시 사용하므로 ID 재업로드는 없다. gradient graph에서는 retained backward 동안 보존되고 성공한 non-retained backward 뒤 해제되며, frozen weight의 no-grad output에서는 output 수명과 함께 유지된다. 같은 ID가 반복되면 dimension별 thread가 원래 token 순서대로 합산해 결정적인 weight gradient를 만든다. CUDA token Tensor 입력은 아직 거부하므로 loader가 반환한 CPU ID Tensor를 그대로 전달한다.
- `causal_attention(query, key, value, [options])`은 rank 2 이상의 입력에서 미래 위치를 가리는 scaled dot-product attention을 수행한다. 마지막 두 축은 `[sequence, dimensions]`이며 앞쪽 축은 batch/head 축이다. options에는 `scale`과 `precision: "auto" | "fast" | "strict"`가 있다. `auto`는 최적화와 fallback을 자동 선택하고, `fast`는 resident CUDA `T >= 8` fused 경로를 요구해 불가능하면 오류를 내며, `strict`는 f64 reference를 고정한다. CPU와 resident CUDA `float32` forward는 전체 score 행렬을 저장하지 않는다. 기본 non-strict `T >= 8` plan은 f32 warp online-softmax forward 1회와 probability를 재계산하는 deterministic dQ·결합 dK/dV backward 2회, 총 3개 kernel로 full q/k/v graph를 실행한다. 행별 max·reciprocal sum만 저장하므로 O(T²) workspace가 없다. forward plan은 graph에 고정된다. `T < 8` 또는 parallel 비활성화된 `auto`는 f64 reference forward/backward로 fallback한다. `SURA_CUDA_ATTENTION_FUSED=0`은 `auto`에서 `T >= 8`, parallel 활성, workspace 한도 충족 시 legacy packed 5-kernel backward를 선택하고, 아니면 serial reference로 내려간다. packed 경로에만 `4 * B * T * (T+1) / 2` bytes workspace 한도가 적용된다. only-Q/K/V에서는 필요한 kernel과 gradient allocation만 만든다. 공개 output/gradient는 `float32`다. 이는 shared-memory tiled/Tensor Core FlashAttention은 아니다.
- `cross_entropy_ids(logits, class_ids)`는 logits의 마지막 축을 class 축으로 사용한다. class ID shape은 logits에서 마지막 축을 뺀 shape와 같아야 하며, 모든 위치의 손실 평균을 scalar로 반환한다. CPU logits와 resident CUDA `float32` logits을 지원하고, ID는 숫자/직사각형 배열 또는 gradient를 요구하지 않는 CPU Tensor여야 한다. shape·정수 여부·class 범위를 host에서 먼저 검증하므로 CUDA target Tensor는 명시적으로 거부한다. CUDA forward는 검증된 ID를 행마다 4-byte raw `uint32`로 한 번 H2D한 뒤, 행 병렬 kernel로 max와 reciprocal exponential sum을 구하고 고정 순서의 단일-thread kernel로 결정적인 scalar 평균 손실을 만든다. backward는 각 logit 원소를 병렬 처리하며 저장한 max·inv-sum·ID를 재사용해 `(softmax - one_hot) * upstream / rows`를 계산하므로 ID를 다시 upload하지 않는다. scalar output이 세 saved allocation을 소유한다. `retain_graph=true`이면 다음 backward까지 유지되고 성공한 non-retained backward 뒤 해제되며, no-grad output에서는 output이 수거될 때 함께 해제된다. loss와 logits gradient는 관찰하기 전까지 device에 남는다.

예를 들어 다음 shape가 자연스럽게 이어진다.

```text
token_ids       [batch, sequence]
embedding       [batch, sequence, model_dim]
q, k, v         [batch, heads, sequence, head_dim]
attention       [batch, heads, sequence, value_dim]
logits          [batch, sequence, vocabulary]
target ids      [batch, sequence]
```

구체적인 한 블록 모델 문법과 head 분할/병합은 [`TRANSFORMER.md`](TRANSFORMER.md)에 정리되어 있다.

## 연산 API

| 분류 | API |
|---|---|
| 생성/dtype/device | `tensor`, `parameter`, `zeros`, `ones`, `randn`, `dtype`, `device`, `to`, `storage_bytes`, `cast` |
| 조회/그래프 | `data`, `grad`, `grad_info`, `shape`, `numel`, `limits`, `item`, `detach`, `requires_grad`, `set_requires_grad` |
| 산술 | `add`, `sub`, `mul`, `div`, `neg` |
| shape/행렬 | `reshape`, `matmul`, `transpose`, `linear` |
| 활성화/정규화 | `relu`, `tanh`, `sigmoid`, `gelu`, `layer_norm`, `softmax` |
| Transformer | `embedding`, `causal_attention` |
| 축약 | `sum`, `mean` |
| 손실 | `mse`, `bce`, `bce_logits`, `cross_entropy`, `cross_entropy_ids` |
| 역전파/AMP | `backward`, `backward_scaled`, `zero_grad`, `unscale_gradients`, `grad_norm`, `clip_grad_norm` |
| 최적화 | `sgd`, `adam`, `reset_optimizer` |
| CUDA 상태 | `autocast`, `cuda_available`, `cuda_info`, `cuda_stats`, `cuda_reset_stats`, `cuda_synchronize` |
| 영속성/호환 | `save_checkpoint`, `load_checkpoint`, `save_safetensors`, `load_safetensors`, `save_onnx_weights`, `load_onnx_weights`, `run_onnx` |
| 분산 gradient | `all_reduce_gradients` |

모든 모듈 함수는 `autograd_tensor`처럼 `autograd_` 접두사의 직접 함수로도 호출할 수 있다.

`cross_entropy`는 logits와 같은 shape의 one-hot 또는 soft target을 받는다. 언어 모델처럼 정수 target이 이미 있다면 one-hot 배열을 만들지 않는 `cross_entropy_ids`를 사용한다. `softmax`는 마지막 차원에 적용된다. 현재 resident CUDA에서는 sparse `cross_entropy_ids`만 지원하며 `softmax`와 dense/one-hot `cross_entropy`는 CPU 전용이다. `bce_logits`는 sigmoid를 따로 호출하지 않고 logits에서 안정적으로 binary cross-entropy를 계산한다.

## 최적화

`sgd(parameters, learning_rate, options)`의 옵션은 `momentum`, `weight_decay`다. CPU parameter에는 CPU `double` gradient/state를 사용하고 resident CUDA parameter에는 GPU `float32` gradient와 velocity를 사용한다. CUDA SGD는 모든 parameter의 candidate weight/velocity를 별도 device buffer에서 만든 뒤 공유 finite-status 한 개만 4-byte D2H로 확인하고 전체를 commit한다. 하나라도 유효하지 않으면 weight와 velocity를 모두 rollback하므로 step당 tensor payload H2D/D2H는 없지만 4-byte control read와 동기화는 있다. 한 SGD 호출에 CPU와 CUDA parameter를 섞을 수 없다. `momentum: 0`을 명시한 SGD 단계는 남아 있던 velocity를 초기화하므로 나중에 momentum을 다시 켜면 새 상태에서 시작한다.

`adam(parameters, learning_rate, options)`의 옵션은 `beta1`, `beta2`, `epsilon`, `weight_decay`다. CPU Adam은 `double`, resident CUDA Adam은 `float32` parameter·gradient·moment를 사용하며, 두 경로 모두 호출별 beta schedule에도 누적 bias correction을 적용한다. 한 Adam 호출에 CPU와 CUDA parameter를 섞을 수 없다. `weight_decay`는 gradient에 더하는 coupled L2 방식이며 decoupled AdamW가 아니다.

CUDA Adam은 갱신 대상 parameter 전체의 새 weight와 1·2차 moment를 별도 device buffer에 먼저 계산한다. 공유 status만 4-byte D2H로 읽어 모든 결과가 유한하고 유효한지 확인한 뒤 한꺼번에 commit하며, 하나라도 실패하면 weight·moment·step과 beta product를 모두 이전 상태로 유지한다. 따라서 매 CUDA Adam step에는 이 트랜잭션 검증을 위한 4-byte D2H와 동기화가 한 번 있다. optimizer 상태는 각 leaf Tensor에 저장되며 `reset_optimizer(parameters)`로 초기화할 수 있다. CUDA `grad_norm`과 `clip_grad_norm`은 아직 구현되지 않았다.

제한된 resident CUDA 학습 step은 다음처럼 구성한다.

```sura
use autograd

x is autograd.tensor([[1, 2], [3, 4]], {device: "cuda"})
y is autograd.tensor([[1], [0]], {device: "cuda"})
w is autograd.parameter([[0.1], [0.2]], {device: "cuda"})

autograd.zero_grad([w])
prediction is autograd.relu(autograd.matmul(x, w))
loss is autograd.mse(prediction, y)
autograd.backward(loss)
autograd.sgd([w], 0.01, {momentum: 0.9, weight_decay: 0.0001})
```

이 step의 tensor payload, matmul, ReLU, MSE 합성, backward와 gradient 누적은 같은 CUDA 장치에 남고, transactional SGD가 step마다 4-byte finite-status만 D2H로 읽는다. 마지막 줄을 `autograd.adam([w], 0.01)`으로 바꾸면 resident CUDA Adam으로 갱신한다. ND input과 rank-2 shared weight를 쓰는 `linear`, vector bias, exact-form GELU, 마지막 축 LayerNorm, CUDA weight embedding, 임의 두 축 transpose, causal attention과 sparse `cross_entropy_ids`도 이 경로에서 forward/backward가 동작한다. 따라서 reshape/transpose로 `[B,T,H,Dh]`와 `[B,H,T,Dh]`를 오가며 multi-head causal graph를 끝까지 GPU에서 학습할 수 있다. embedding ID와 cross-entropy target은 host-validated CPU 값이어야 하며 forward마다 한 번 raw `uint32`로 upload된다. CUDA token/target Tensor는 받지 않는다. 오른쪽 operand도 batched인 일반 matmul, softmax와 dense/one-hot `cross_entropy`는 아직 이 경로에 섞을 수 없다.

## CUDA 상주와 host 관찰 경계

GPU 연산 결과는 host 배열을 즉시 만들지 않는다. `data`, `item`, `grad`, `to(tensor, "cpu")`, `json.stringify`, checkpoint/Safetensors/ONNX 저장처럼 실제 숫자를 CPU에서 관찰하거나 직렬화하는 API가 명시적인 host 경계다. host mirror가 stale이면 이때 D2H가 발생하고, 이미 동기화된 mirror가 있으면 불필요한 copy는 생략한다. 반대 방향의 H2D는 CUDA Tensor 생성, `to(..., "cuda")`, CUDA non-scalar `backward`에 CPU gradient seed를 전달할 때, 그리고 CUDA embedding ID나 `cross_entropy_ids` target을 host에서 검증해 forward마다 raw `uint32`로 한 번 upload할 때 발생한다. 두 backward 모두 output이 소유한 saved ID buffer를 재사용하며, cross-entropy의 target upload 크기는 `4 * rows` bytes다. CUDA SGD와 Adam은 optimizer transaction마다 tensor payload가 아닌 공유 4-byte status만 D2H로 확인한다. 이 경계는 원래 Tensor를 CPU로 자동 이동시키지 않으며 GPU allocation과 동기화된 host mirror를 만든다. `clone`과 `detach`는 resident CUDA Tensor끼리 D2D copy한다.

`cuda_stats()`는 현재/최대 allocation bytes, allocation/free 횟수, H2D/D2H/D2D bytes, 전체 kernel launch와 matmul/elementwise/ReLU/GELU/LayerNorm/embedding/transpose/attention/cross-entropy/reduction/optimizer launch를 반환한다. matmul은 `cublas_matmul_launches`와 `reference_matmul_launches`, transpose는 `transpose_launches`로 구분한다. attention 합계 `attention_launches`는 서로 배타적인 `reference_attention_launches`·`warp_attention_launches`·`parallel_attention_launches`·`fused_attention_launches`로 분할된다. `fast_attention_forward_launches`는 fast f32 warp forward를 겹쳐 세는 진단 값이다. 기본 full q/k/v 경로는 `attention_launches == 3`, `warp_attention_launches == 1`, `fused_attention_launches == 2`, `parallel_attention_launches == 0`이다. legacy packed fallback은 warp 1회 + parallel 5회이고 serial fallback은 reference 2회다. frozen q/k/v role이 있으면 필요한 backward kernel만 실행한다. sparse CE forward는 `cross_entropy_launches`를 2회, logits backward까지 실행하면 합계 3회 증가시킨다. `cuda_reset_stats()`는 현재 allocation 크기를 보존하면서 transfer와 launch counter를 초기화한다. 따라서 setup upload 뒤 counter를 초기화하고 graph를 실행하면 중간 tensor payload 왕복이 없는지 확인할 수 있다. CUDA embedding/CE의 ID upload와 CUDA SGD/Adam의 트랜잭션 status 4-byte D2H는 의도적인 경계다.

```sura
autograd.cuda_reset_stats()
loss is autograd.sum(autograd.relu(autograd.matmul(x, w)))
before_read is autograd.cuda_stats()
print(before_read.h2d_bytes) # 0
print(before_read.d2h_bytes) # 0
print(autograd.item(loss))   # 여기서 scalar 4 bytes D2H
```

지원되지 않는 resident CUDA 연산이나 CPU/CUDA 혼합 연산은 CPU fallback 없이 오류를 낸다. 필요하면 `to(tensor, "cpu")`로 경계를 코드에 직접 표시한다.

CPU 학습에서 gradient 폭주를 제한할 때는 업데이트 전에 전역 L2 norm을 확인하고 자른다. resident CUDA gradient에는 이 API가 아직 없다.

```sura
use autograd

p is autograd.parameter([3, 4])
loss is autograd.sum(autograd.mul(p, p))
autograd.backward(loss)
before is autograd.clip_grad_norm([p], 1)
print(before)
autograd.sgd([p], 0.1)
```

## Binary checkpoint

state dict는 이름을 key로, Tensor를 value로 갖는 딕셔너리다. 저장 함수는 SHA-256 footer가 있는 binary 파일을 원자적으로 기록하고 최종 파일 바이트 수를 반환한다. SHA-256은 손상 탐지용이며 서명이나 출처 인증을 대신하지 않는다.

```sura
use autograd

state is {token_embedding: token_embedding, query_weight: query_weight, language_head_weight: language_head_weight}

written_bytes is autograd.save_checkpoint(state, "tiny.surackpt")
loaded is autograd.load_checkpoint("tiny.surackpt")
print(written_bytes)
print(autograd.shape(loaded.token_embedding))
```

binary checkpoint v3는 visible CUDA weight와 `{optimizer: true}`일 때 f32 master, SGD velocity 또는 Adam m/v·step·beta product를 보존한다. 정확한 CUDA optimizer 재개는 `load_checkpoint(path, {optimizer: true, device: "cuda"})`를 사용하며 결과는 dtype와 `requires_grad`가 보존된 CUDA leaf다. CUDA optimizer state를 기본 CPU target으로 조용히 바꾸는 것은 거부한다. `{optimizer: false}`는 hidden state를 제외하고 visible weight만 CPU 또는 명시한 CUDA target으로 복원한다. gradient와 계산 graph는 저장하지 않으며 v1/v2 파일도 읽는다. 하나의 Tensor 객체를 여러 이름 아래 넣으면 저장을 거부한다.

## Weight 호환과 분산 gradient

표준 Safetensors는 네 가지 dtype와 shape를 보존하며 Sura가 Python 없이 직접 읽고 쓴다.

```sura
use autograd

autograd.save_safetensors({weight: w, bias: b}, "model.safetensors")
weights is autograd.load_safetensors("model.safetensors", {requires_grad: true})
```

`tools/sura_torch_bridge.py`는 legacy PyTorch `.pt`/`.pth` state dict와 Safetensors 사이를 변환한다. helper에는 PyTorch가 필요하지만 Sura의 native Safetensors API에는 필요하지 않다. `save_onnx_weights`/`load_onnx_weights`는 ONNX typed initializer를 교환한다.

`autograd.run_onnx(path, inputs, [options])`는 raw-data float initializer, 제한된 INT64 shape initializer와 CPU Tensor 입력으로 검증된 ONNX subset을 실행하고 graph output 이름의 Tensor 딕셔너리를 반환한다. 입력에서 결과로 이어지는 Sura autograd graph를 유지하므로 `backward`가 입력까지 전파된다. v1 범위는 IR 3~10, 기본 opset 7~18, 최대 4,096 node, node당 입력·출력 16개와 attribute 64개다. ValueInfo에 Tensor dtype과 정적 shape가 있으면 입력, initializer, 중간값과 출력을 모두 대조한다. 지원 연산은 `Identity`, `Add`, `Sub`, `Mul`, `Div`, `Neg`, `MatMul`, `Relu`, `Tanh`, `Sigmoid`, `Gemm`, `Transpose`, `Flatten`, `Reshape`, 마지막 축 `Softmax`다. `Transpose`는 전체 `perm` 순열 또는 생략 시 축 역순을 지원하고, `Flatten`은 기본 `axis=1`을 포함해 `[-rank, rank]` 범위의 축을 지원한다. `Reshape` shape는 raw-data INT64 rank-1 initializer, 최대 8개 값으로 제한되며 `allowzero=0`의 축 복사와 한 개의 `-1` 추론을 지원한다. graph는 topological 순서여야 하며 custom domain, GPU 입력, external data, 일반 integer Tensor initializer, zero-size output, Conv, symbolic/dynamic shape, control flow와 그 밖의 연산은 거부한다.

```sura
use autograd

x is autograd.tensor([[1, 2]])
outputs is autograd.run_onnx("model.onnx", {X: x})
print(autograd.data(outputs.Y))
```

여러 프로세스에서 CPU gradient를 동기화하려면 모든 rank가 같은 shared directory, `run_id`, `step`, parameter 순서로 호출한다.

```sura
use autograd

report is autograd.all_reduce_gradients(parameters, {rendezvous: "shared/gradients", run_id: "train-1", step: step, rank: rank, world_size: world_size, average: true, timeout_ms: 30000})
```

이 구현은 checksum을 검증하는 공유 파일시스템 동기식 sum/average다. NCCL이나 GPU all-reduce가 아니다. 자세한 실행 조건과 benchmark 방법은 [`GPU_AND_SCALE.md`](GPU_AND_SCALE.md)를 참고한다.

## 안전 제한과 성능 경계

기본값은 Tensor rank 8, Tensor당 10,000,000 elements, 그래프 1,000,000 nodes, live host buffer 512 MiB, causal-attention score 50,000,000개다. CUDA optimized attention은 sequence 8부터 선택한다. 기본 fused path는 O(T²) workspace를 쓰지 않는다. `SURA_CUDA_ATTENTION_FUSED=0|false|off`로 fused path를 끄면 legacy packed backward가 선택되고, 이 fallback의 workspace 기본 한도는 64 MiB이며 `SURA_CUDA_ATTENTION_WORKSPACE_MB`로 최대 4096 MiB까지 바꿀 수 있다. `SURA_CUDA_ATTENTION_PARALLEL=0|false|off`는 두 optimized path를 모두 끈다. `autograd.limits()`의 `cuda_attention_parallel_min_sequence`, `cuda_attention_workspace_limit_bytes`, `cuda_attention_parallel`, `cuda_attention_fused`가 실제 dispatch 설정을 반환한다.

현재 프로세스에 실제 적용된 값과 사용량은 `autograd.limits()`로 확인한다.

```sura
limits is autograd.limits()
print(limits.max_elements)
print(limits.max_attention_scores)
print(limits.cuda_attention_workspace_limit_bytes)
print(limits.cuda_attention_parallel)
print(limits.cuda_attention_fused)
print(limits.memory_limit_bytes)
print(limits.memory_used_bytes)
```

`storage_bytes`는 visible payload만 보고한다. frozen/inference weight는 f16/bf16에서 `2N`, f32에서 `4N` bytes다. 하지만 low trainable parameter는 첫 optimizer step 뒤 visible `2N` + master `4N` + gradient `4N`을 가지며 Adam m/v가 각각 `4N`을 더한다. 따라서 Adam steady state는 low `18N`, f32 `16N`으로 현재 low 학습이 전체 VRAM을 줄인다고 말할 수 없다. output/activation도 기본 f32이고 transaction candidate와 workspace peak는 별도다.

`causal_attention` forward는 online softmax로 전체 `T×T` score 행렬을 저장하지 않지만 계산량은 O(T²)다. `T >= 8`의 기본 plan은 f32 warp-parallel forward와 deterministic fused-recomputation backward다. 행별 통계만 보존하므로 memory는 O(B·T)이며 probability/dScore workspace는 없다. legacy packed와 serial reference fallback도 명시적으로 유지한다. 동일 Safetensors 입력·동일 GPU·수치 지문·launch/transfer 계약을 강제하는 최신 Sura/PyTorch 결과는 `artifacts/attention_benchmark.md`와 JSON 원본에 기록한다. attention kernel 자체는 아직 f32이며 shared-memory tiled/Tensor Core·저정밀 FlashAttention이나 KV cache는 아니다.

Sura의 장점은 외부 수치 패키지 없이 실행 파일 하나로 작은 Tensor 모델과 제한된 GPU-resident 학습 graph를 조합하고 검증할 수 있다는 점이다. 실제 2-byte f16/bf16 weight storage, typed cuBLAS/PTX matmul, f32 master optimizer, checkpoint v3와 no-O(T²)-workspace fused attention은 구현됐다. 하지만 mixed backward는 아직 reference PTX이고 tiled/Tensor Core attention과 범용 CUDA operator stack은 없다. 공개 f32 benchmark에서도 PyTorch가 더 빠르므로 이를 Python/PyTorch 생태계보다 우월하다는 근거로 해석해서는 안 된다.

## 검증

- 기본 연산과 broadcasting: `tests/13_autograd_core.sura`
- 기본 연산 유한차분 gradient 검증: `tests/14_autograd_gradcheck.sura`
- SGD/Adam 및 작은 MLP 학습: `tests/15_autograd_training.sura`
- Transformer 연산, shape, broadcast backward: `tests/16_transformer_ops.sura`
- LayerNorm, causal attention, sparse CE, exact GELU 유한차분 검증: `tests/17_transformer_gradcheck.sura`
- checkpoint, optimizer 상태, `requires_grad`, checksum 손상 검증: `tests/18_transformer_checkpoint.sura`
- 한 블록 causal 언어 모델의 실제 학습: `tests/19_tiny_transformer_training.sura`
- Safetensors dtype/shape round-trip과 손상 거부: `tests/20_safetensors.sura`
- packed dtype 저장, promotion, gradient/checkpoint: `tests/21_dtype_storage.sura`
- 실제 CUDA rank-2 f32 matmul과 CPU parity: `tests/22_cuda_backend.sura`
- byte tokenizer, seek loader, rank partition: `tests/23_tokenizer_dataset.sura`
- bounded byte-level BPE, UTF-8 round-trip, persistence validation: `tests/70_bpe_tokenizer.sura`
- 공유 파일시스템 gradient all-reduce: `tools/sura_distributed_autograd_smoke.ps1`
- ONNX typed initializer round-trip: `tests/25_onnx_weights.sura`
- bounded CPU ONNX 실행, backward와 malformed/unsupported model 거부: `tests/71_onnx_execution.sura`
- CUDA device 생성, `device`/`to`, D2D clone/detach와 명시적 혼합-device 거부: `tests/26_cuda_device_placement.sura`
- resident matmul/ReLU/elementwise/reduction chain과 transfer counter: `tests/27_cuda_residency.sura`
- CUDA matmul/ReLU backward, persistent gradient 관찰과 GPU SGD: `tests/28_cuda_backward.sura`
- CPU/CUDA Adam parity, 4-byte status D2H와 실패 시 전체 rollback: `tests/30_cuda_adam.sura`
- CUDA ND shared-weight `linear` forward/backward, bias 축약과 host-lazy parity: `tests/33_cuda_nd_linear.sura`
- CUDA exact-form GELU forward/backward, tail 안정성, transfer와 host-lazy parity: `tests/34_cuda_gelu.sura`
- CUDA last-axis LayerNorm, 선택 affine gradient, saved row state, 무전송과 host-lazy parity: `tests/35_cuda_layer_norm.sura`
- CUDA SGD의 multi-parameter transaction, 4-byte status D2H와 rollback: `tests/36_cuda_sgd_transaction.sura`
- optional cuBLAS SGEMM dispatch와 reference PTX fallback: `tests/37_cuda_cublas_dispatch.sura`, `tools/sura_cuda_cublas_smoke.ps1`
- CUDA embedding gather, raw-u32 단일 ID upload, 중복-ID backward와 saved-ID 재사용: `tests/38_cuda_embedding.sura`
- CUDA sparse CE의 CPU target 검증과 CUDA target 거부, raw-u32 target upload 1회, rank-3 CPU parity, 안정적인 extreme-logit 평균 손실, saved-state backward, implicit/explicit scalar seed와 host-lazy transfer: `tests/39_cuda_cross_entropy_ids.sura`
- CUDA causal attention rank-3/4 forward/backward, causality, graph lifetime·alias·frozen role, 0-transfer와 극단 f32 안정성: `tests/40_cuda_causal_attention.sura`, `tests/41_cuda_causal_attention_graph.sura`, `tests/41_cuda_causal_attention_extreme.sura`
- CUDA causal attention의 no-O(T²)-workspace fused backward, CPU parity, 반복 결정성, frozen role, retain-graph 누적, memory/launch counter와 legacy packed·serial fallback 계약: `tests/44_cuda_attention_parallel.sura`
- CUDA causal attention의 fast f32 warp forward, tail dimension, extreme score, frozen no-grad 경로와 bit-for-bit 결정성: `tests/45_cuda_attention_warp_forward.sura`; strict f64 extreme-score 경계는 `tests/41_cuda_causal_attention_extreme.sura`
- CUDA causal attention의 T=8/9/17 및 D/Dv=1/31/32/33/65 rank-2/4 warp/lane tail과 3-launch contract: `tests/46_cuda_attention_warp_edges.sura`
- attention precision invalid/CPU·short·env-disabled fast fail-closed, auto fused·legacy·reference dispatch, 양방향 sticky plan, strict f64 parity, q=k=v alias, scaled backward와 retain/release memory 계약: `tests/52_cuda_attention_precision.sura`
- CUDA f16/bf16 matmul compute, f32 public storage·gradient, cuBLAS FAST/reference fallback counter: `tests/47_cuda_mixed_matmul.sura`
- autocast 중첩 복원, linear의 현재 autocast 적용, matmul 명시 override와 forward/backward sticky compute plan: `tests/48_cuda_autocast.sura`
- scaled backward, scale 누적 불변식, payload 무전송 `grad_info`, empty-gradient no-op, transactional finite unscale, found-inf rollback과 optimizer guard: `tests/49_cuda_loss_scaling.sura`
- 실제 2-byte f16/bf16 CUDA storage, typed forward/backward, f32 master와 VRAM truth: `tests/50_cuda_typed_storage.sura`
- CUDA Adam/SGD master·moment·velocity checkpoint v3와 exact leaf resume: `tests/31_cuda_adam_checkpoint.sura`, `tests/51_cuda_sgd_checkpoint.sura`
- rank 2–8 CUDA transpose의 multi-head 축 교환, inverse backward와 전송/launch 계약: `tests/42_cuda_transpose.sura`
- embedding부터 두 head의 split/merge attention, sparse CE와 Adam까지 resident end-to-end 학습: `tests/43_cuda_multihead_training.sura`
- 동일 입력·동일 하드웨어 one-shot matmul benchmark: `tools/sura_ai_benchmark.ps1`
- 동일 입력·동일 RTX 4060 resident forward/training benchmark: `tools/sura_ai_resident_benchmark.ps1`, `artifacts/ai_resident_cuda_cublas_benchmark.md`
- 동일 입력·동일 하드웨어 f32/f16/bf16 compute benchmark: `tools/sura_mixed_compute_benchmark.ps1` (유휴 GPU에서 `performance_valid: true`인 실행만 성능 수치로 인용)
- 동일 입력·동일 하드웨어 직접 causal-attention benchmark: `tools/sura_attention_benchmark.ps1`
- 잘못된 shape/graph/optimizer 입력: `tools/sura_autograd_smoke.ps1`
- 패키지 탐색: `surapkg list`, `surapkg info autograd`, `surapkg search autograd.causal_attention`
- 크로스 플랫폼 및 sanitizer 실행 경로: `.github/workflows/cross-platform-smoke.yml`
