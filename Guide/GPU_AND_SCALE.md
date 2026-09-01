# Sura 1.11 AI 백엔드: GPU, dtype, 데이터, 분산, 호환성

최종 업데이트: 2026-07-12

이 문서는 Sura 1.11 AI 백엔드의 **실제로 구현되고 검증된 범위**를 정리한다. 현재 Sura는 작은 모델을 직접 학습하고, typed weight를 교환하고, 제한된 `float32` graph를 GPU memory에 유지해 forward/backward/SGD/Adam을 실행할 수 있다. 지원되는 SGEMM은 호환 cuBLAS를 동적으로 찾으면 cuBLAS로 실행하고, 없으면 resident reference PTX로 fallback한다. causal attention에는 score 행렬과 O(T²) backward workspace를 만들지 않는 결정적 CUDA fused-recomputation 경로가 있다. shared-memory tiled/Tensor Core FlashAttention과 NCCL은 현재 구현 범위에 포함되지 않는다.

## 한눈에 보는 현재 범위

| 목표 | Sura 1.11에서 구현된 범위 | 아직 구현되지 않은 범위 |
|---|---|---|
| CUDA GPU backend | CUDA Driver API + resident `float32` Tensor, optional dynamic cuBLAS SGEMM/GemmEx와 reference PTX fallback, contiguous ND-left × rank-2 shared-weight projection forward/backward, rank 2–8 transpose, CUDA weight embedding gather/deterministic backward, score-matrix-free causal attention warp forward와 deterministic no-O(T²)-workspace fused-recomputation backward, legacy packed/serial fallback, host-target sparse `cross_entropy_ids`, elementwise·bias·GELU·LayerNorm·reduction, persistent GPU gradient, transactional SGD/Adam | 실제 instruction-level Tensor Core profiling과 전용 kernel, warp-parallel LayerNorm, batched-right matmul과 일반 broadcasting, CUDA token/target Tensor 입력, softmax·dense/one-hot cross-entropy, AdamW |
| `float32`/`float16`/`bfloat16` | dtype 크기에 맞는 packed CPU/CUDA storage; typed matmul은 low storage를 직접 읽어 f32 output/gradient를 만들고 low parameter는 f32 master로 SGD/Adam을 수행 | low activation/output, mixed backward 가속, 저정밀 attention/LayerNorm, 압축 optimizer state |
| 고속 attention | CPU와 resident CUDA `float32` online-softmax causal attention; 기본 `T >= 8` CUDA plan은 f32 warp forward 1회와 확률을 재계산하는 deterministic dQ·dK/dV backward 2회로 실행하며 O(T²) workspace를 쓰지 않음 | shared-memory tiled/Tensor Core·저정밀 FlashAttention, padding/임의 mask, dropout, KV cache |
| tokenizer/loader | UTF-8 raw-byte tokenizer, bounded deterministic byte-level BPE, BPE chunk-boundary 보존 pack, checksum 형식, seek 기반 uint32 shard loader | Hugging Face tokenizer 호환, WordPiece/SentencePiece, memory mapping, prefetch worker |
| 분산 학습 | 공유 파일시스템을 이용한 동기식 CPU gradient sum/average | resident CUDA gradient all-reduce, NCCL, 네트워크 transport, fault-tolerant elastic training |
| 모델 호환 | 표준 Safetensors, PyTorch 변환 helper, ONNX typed initializer 입출력과 bounded CPU operator subset | 임의 PyTorch 객체, 임의 ONNX graph·Conv·동적 shape/control flow·GPU 실행 |
| benchmark | 동일 Safetensors 입력의 one-shot matmul과 RTX 4060 resident f32 forward/training 공개 benchmark | Transformer 전체 학습 성능이나 모델 품질 비교 |

## 1. dtype는 저장 형식과 계산 형식을 구분한다

`float64`, `float32`, `float16`, `bfloat16` Tensor는 실제 dtype 크기의 row-major packed CPU buffer를 사용한다. 기본 dtype는 `float64`다.

```sura
use autograd

f32 is autograd.tensor([1, 2, 3], {dtype: "float32", requires_grad: true})
f16 is autograd.cast(f32, "float16")
bf16 is autograd.cast(f32, "bfloat16")
print(autograd.dtype(f16))
print(autograd.storage_bytes(bf16))
```

중요한 경계가 있다. CPU kernel은 값을 `double`로 계산하고 CPU gradient/optimizer state도 `double`이다. resident CUDA visible Tensor는 f32 또는 실제 2-byte f16/bf16 storage를 사용한다. typed matmul만 low storage를 직접 지원하며 output, persistent gradient, master, velocity와 Adam moment는 f32다. 나머지 resident operator는 아직 f32-only다. 따라서 frozen/inference low weight는 작지만 activation과 전체 training state가 자동으로 작아지는 것은 아니다.

이항 연산은 dtype promotion을 적용한다. `float16 + bfloat16`은 `float32`로 승격되며, 명시적 `float64` Tensor가 섞이면 결과는 `float64`가 될 수 있다. 실제 결과 dtype는 `autograd.dtype(tensor)`로 확인한다.

## 2. resident CUDA로 제한된 float32 학습 graph를 실행한다

CUDA backend는 CUDA Driver API와 내장 PTX를 사용한다. cuBLAS가 로드되면 f32는 `cublasSgemm`, 같은 low dtype의 f16/bf16 A/B storage는 f32 C를 쓰는 `cublasGemmEx`로 dispatch한다. 미지원·비활성화 시 typed storage를 직접 읽는 reference PTX로 fallback한다. f32와 low storage가 섞인 backward GEMM은 현재 f32 compute reference PTX라서 정확하지만 고성능 경로는 아니다. FAST counter는 Tensor-Core-eligible 요청 성공 증거이지 실제 HMMA 실행의 profiler 증거를 대신하지 않는다.

`SURA_CUBLAS_LIBRARY`에 cuBLAS DLL/shared-library 경로를 명시할 수 있고, `SURA_CUBLAS_DISABLE=1`은 reference PTX 경로를 강제한다. 실제 상태는 `cuda_info()`의 `backend`, `matmul_backend`, `cublas_available`, `cublas_library`, `cublas_error`로 확인한다. cuBLAS 탐색 실패는 CUDA 전체 실패나 CPU fallback이 아니며 matmul만 reference PTX를 사용한다.

```sura
use autograd

info is autograd.cuda_info()
if info.available then
  x is autograd.tensor([[1, 2], [3, 4]], {device: "cuda"})
  target is autograd.tensor([[1], [0]], {device: "cuda"})
  weight is autograd.parameter([[0.1], [0.2]], {device: "cuda"})

  autograd.zero_grad([weight])
  prediction is autograd.relu(autograd.matmul(x, weight))
  loss is autograd.mse(prediction, target)
  autograd.backward(loss)
  autograd.sgd([weight], 0.01, {momentum: 0.9, weight_decay: 0.0001})
  print(autograd.item(loss))
end
```

`tensor`, `parameter`, `zeros`, `ones`, `randn`의 options에 `device`를 지정할 수 있다. `{device: "cuda"}`에서 dtype를 생략하면 `float32`가 기본값이고, 현재 CUDA resident Tensor는 `float32`만 허용한다. `autograd.device(tensor)`는 `cpu` 또는 `cuda:N`을 반환하고 `autograd.to(tensor, "cpu"|"cuda"|"cuda:N")`는 명시적으로 복사한다. CUDA 학습 leaf는 `to()`로 CPU leaf를 옮기기보다 처음부터 CUDA `parameter`로 만드는 것이 필요하다. 현재 backward는 CPU/CUDA가 섞인 graph를 거부하기 때문이다.

현재 resident 범위는 다음과 같다.

- contiguous `[..., rows, inner] @ [inner, cols]` shared-weight projection forward와, 모든 앞쪽 위치를 합산하는 transpose-aware backward
- shape이 같은 CUDA Tensor 사이의 `add`, `sub`, `mul`; 숫자 scalar와의 산술; `neg`
- D2D `reshape`, `clone`, `detach`
- ReLU와 exact-form GELU forward/backward
- 마지막 축 LayerNorm forward/backward와 선택적인 `[features]` weight/bias gradient
- host-validated CPU ID를 한 번 raw `uint32`로 upload하는 CUDA embedding gather와 deterministic duplicate-ID weight backward
- host-validated CPU target을 한 번 raw `uint32`로 upload하는 sparse `cross_entropy_ids`의 안정적인 평균 손실과 logits backward
- Tensor 전체 `sum`/`mean` forward와 scalar loss root backward
- `sub` → `mul` → `mean`으로 합성되는 같은-shape MSE
- leaf마다 GPU에 유지되고 여러 backward에서 누적되는 `float32` gradient
- momentum과 coupled `weight_decay` 옵션을 포함한 transactional GPU SGD
- 누적 bias correction과 coupled L2 `weight_decay`를 지원하는 GPU Adam

CUDA Tensor-Tensor division, scalar/Tensor division, 일반 broadcasting, 오른쪽 operand에도 batch prefix가 있는 batched-right matmul, CUDA token/target Tensor 입력, softmax, dense/one-hot `cross_entropy`, gradient norm/clipping은 아직 지원하지 않는다. 왼쪽 ND Tensor와 rank-2 shared weight의 projection, `[features]` bias, rank 2–8 임의 두 축 transpose, exact-form GELU, 마지막 축 LayerNorm, CUDA weight embedding, causal attention과 sparse `cross_entropy_ids`는 resident CUDA에서 forward/backward를 지원한다. 따라서 reshape/transpose로 `[B,T,H,Dh] ↔ [B,H,T,Dh]`를 구성하는 multi-head graph가 GPU에서 끝까지 이어진다. embedding token ID와 sparse CE target은 숫자/직사각형 배열 또는 non-gradient CPU Tensor여야 하며 CUDA Tensor이면 명시적으로 거부한다. 지원되지 않은 연산과 CPU/CUDA 혼합 operand는 CPU로 fallback하지 않고 명시적으로 오류를 낸다. SGD와 Adam도 한 호출에 CPU/CUDA parameter를 섞으면 거부한다.

현재 CUDA LayerNorm은 정확성과 결정성을 우선해 forward와 input backward에서 CUDA thread 하나가 마지막 축의 행 하나를 순회하고, affine parameter backward에서는 thread 하나가 feature 하나의 모든 행을 합산한다. forward의 행별 `float32` mean/rstd는 저장해 backward에서 재사용하지만, backward gradient에 필요한 행·feature reduction은 다시 계산한다. 이 내부 합계와 gradient reduction은 `float64`로 누적한다. saved buffer는 non-retained backward 뒤 해제되고, gradient가 필요 없는 forward는 만들지 않는다. output, saved state와 gradient는 device에 남으므로 metadata 조회나 graph 실행 중 H2D/D2H가 발생하지 않는다. 이는 검증된 기능 경로이지 고성능 LayerNorm 주장은 아니며, warp/block-parallel Welford 계열 kernel은 아직 성능 과제로 남아 있다.

CUDA embedding v1은 weight만 resident CUDA `float32` Tensor로 받는다. ID shape, 정수 여부와 vocabulary 범위는 host에서 먼저 검증되고 token마다 4-byte raw `uint32`로 매 forward 한 번 H2D된다. output이 이 device ID allocation을 소유해 backward에서 재업로드 없이 재사용한다. gradient graph에서는 retained backward 동안 유지되고 성공한 non-retained backward 뒤 해제되며, frozen weight의 no-grad output에서는 output이 수거될 때 함께 해제된다. backward는 embedding dimension별 thread가 token을 원래 순서대로 순회하므로 반복 ID도 atomics/race 없이 결정적으로 합산된다. CUDA token Tensor 직접 입력, ID cache/prefetch와 고성능 sparse/segmented reduction은 v1 범위가 아니다.

CUDA sparse CE v1은 resident `float32` logits과 host target을 받는다. target shape가 logits의 마지막 class 축을 뺀 shape인지, 값이 gradient 없는 정수이고 `0 <= id < classes`인지 CPU에서 검증한 뒤 행마다 4-byte raw `uint32` ID를 한 번 H2D한다. CUDA target Tensor는 검증을 위한 암묵적 D2H를 만들지 않고 즉시 거부한다. forward의 첫 kernel은 행별로 병렬 실행하며 max-subtracted exponential sum을 `float64`로 누적하고 `float32` max/inv-sum을 저장한다. 두 번째 kernel은 GPU thread 하나가 행을 고정 순서로 순회해 NLL을 `float64`로 누적한 뒤 평균 `float32` scalar loss를 기록하므로 결과 순서가 결정적이다. backward는 logit 원소별 병렬 kernel이 saved max·inv-sum·raw-u32 IDs와 scalar upstream을 재사용해 mean sparse-CE gradient를 만든다. loss output이 세 saved allocation을 소유하므로 `retain_graph=true` 동안 유지되고 성공한 non-retained backward 뒤 해제된다. no-grad output에서는 output이 수거될 때 해제된다. 이 경로는 수치 정확성과 resident lifetime을 검증한 첫 구현이며, vocabulary 축을 협력 처리하는 고처리량 reduction/fusion은 후속 성능 과제다.

CUDA SGD는 parameter별 candidate weight/velocity를 별도 device buffer에서 계산하고 갱신 대상 전체의 finite 상태를 공유 4-byte D2H 한 번으로 검증한 뒤 한꺼번에 commit한다. 하나라도 실패하면 weight와 velocity를 모두 rollback한다. 따라서 optimizer step마다 tensor payload 전송은 없지만 4-byte control read와 동기화가 있다. `weight_decay`는 coupled L2다.

CUDA Adam은 새 weight와 두 moment를 별도 device buffer에 계산하고, 갱신 대상 parameter 전체가 유효하다는 공유 status를 4-byte D2H로 확인한 뒤 한꺼번에 commit한다. 하나라도 실패하면 weight, moment, step과 beta product가 모두 이전 상태로 남는다. 이 검증 때문에 CUDA Adam step마다 동기화와 4-byte D2H가 한 번 발생한다. `weight_decay`는 gradient에 결합되는 L2 방식이며 decoupled AdamW는 아직 없다.

### compute-only autocast와 안전한 loss scaling

`autograd.autocast()`는 thread-local `{enabled, dtype}` 상태를 조회한다. bool, `"float16"`/`"bfloat16"` 또는 `{enabled, dtype}`로 설정하면 이전 상태를 반환한다. resident CUDA `matmul`과 `linear`의 compute dtype을 바꾼다. 명시적 `{compute_dtype: "float32"|"float16"|"bfloat16"}` 옵션은 `matmul(left, right, options)`에만 있으며 autocast보다 우선한다. `linear(input, weight, [bias])`에는 options 인수가 없어서 호출 시점의 현재 autocast 상태를 사용한다. forward에서 선택한 dtype는 graph에 저장되어 두 backward matmul에도 그대로 적용된다.

```sura
previous is autograd.autocast("bfloat16")
prediction is autograd.linear(input, weight, bias)
loss is autograd.cross_entropy_ids(prediction, targets)
autograd.autocast(previous)

scale is 65536
autograd.backward_scaled(loss, scale)
status is autograd.unscale_gradients(parameters)
if status.found_inf then
  autograd.zero_grad(parameters)
  scale is scale / 2
else
  autograd.adam(parameters, 0.001)
end
```

`backward_scaled`의 scale은 양의 유한한 f32로 표현 가능해야 하고 1이면 안 되며, 역수도 양의 유한한 f32로 표현 가능해야 한다. scale 1에는 일반 `backward`를 사용한다. scaled gradient는 leaf마다 declared scale metadata를 갖고 같은 scale끼리만 누적된다.

`unscale_gradients`의 parameter 목록은 비어 있을 수 없다. scale을 생략하면 0이 아닌 모든 gradient scale이 공통이어야 하고, 명시하면 각 scale과 정확히 일치해야 한다. 실제 후보를 나눌 때 그 역수도 양의 유한한 f32로 표현 가능해야 한다. scale 1인 이미 unscaled gradient를 다시 처리하면 오류다. gradient가 없거나 scale-0 zero buffer뿐인 유효한 목록은 `gradient_tensors: 0`, `committed: true`인 성공 no-op이며 status D2H도 없다. 실제 후보가 있으면 모든 후보를 별도 f32 allocation에 계산하고 공유 4-byte status 한 번으로 finite 여부를 확인한다. 하나라도 NaN/Inf면 원본 scaled gradient 전체를 보존하고 `committed: false`를 반환하며, 모두 유한할 때만 동시에 scale 1로 commit한다. SGD와 Adam은 아직 unscale되지 않은 gradient를 사전 거부한다. scale growth/backoff 정책은 현재 Sura 학습 loop가 관리한다.

`autograd.grad_info(tensor)`는 gradient payload를 host로 읽지 않고 `{present, dtype, device, elements, storage_bytes, scale, scaled, leaf, requires_grad, optimizer_ready}`를 반환한다. resident CUDA gradient metadata는 실제 f32 device allocation을, CPU gradient metadata는 Tensor storage dtype와 관계없이 f64(`double`) allocation을 보고한다. `optimizer_ready`는 leaf·`requires_grad`·scale 0/1이라는 기본 자격만 나타내며 optimizer의 전체 검증을 대신하지 않는다. 따라서 CUDA에서 `grad()`의 payload D2H 없이 loss-scale 상태와 실제 gradient byte 수를 확인할 수 있다.

`cuda_stats()`의 dtype별 matmul counter와 `typed_storage_matmul_launches`, `storage_conversion_launches`, `cublas_fast_matmul_launches`, `mixed_matmul_fallback_launches`로 dispatch를 검증한다. visible low weight는 `2N` bytes지만 output/gradient와 optimizer state는 f32다.

메모리 계산은 구분해야 한다. inference/frozen weight는 f32 `4N`에서 f16/bf16 `2N`으로 줄어든다. 반면 Adam steady state는 f32가 visible+gradient+m+v=`16N`, low가 visible+master+gradient+m+v=`18N`이다. transaction candidate와 workspace peak도 별도이므로 현재 구현을 “저정밀 학습 VRAM 절감”으로 설명하면 안 된다.

CPU `float32` Tensor에 `matmul(left, right, {backend: "cuda"})`를 지정하는 기존 one-shot 경로도 호환을 위해 남아 있다. 이 경로는 매 호출마다 두 입력을 H2D copy하고 결과를 D2H copy해 CPU Tensor를 반환하며 backward도 CPU graph다. 반복 학습에서는 입력 자체를 CUDA에 배치하는 resident 경로와 구분해야 한다.

가용성과 정확한 kernel 범위는 런타임에서 확인한다.

```sura
use autograd

print(autograd.cuda_available())
print(autograd.cuda_info())
print(autograd.cuda_stats())
```

`cuda_info().kernel_coverage`는 현재 resident kernel 범위를 문자열로 반환한다. `matmul_backend`는 f32 경로, `mixed_matmul_backend`와 `mixed_matmul_compute_dtypes`는 f16/bf16 compute 경로를 설명한다. `cublas_available`/`cublas_gemm_ex_available`/`cublas_disabled`/`cublas_library`/`cublas_error`로 동적 dispatch 상태를 확인한다. 프로세스 시작 전에 `SURA_CUDA_DEVICE`를 zero-based index로 설정하면 장치를 선택할 수 있고 `cuda_info().device_index`로 확인한다. 한 프로세스에서는 선택된 장치 하나만 사용한다. CUDA가 없거나 잘못된 장치를 요청하면 조용히 CPU로 fallback하지 않고 오류를 낸다.

### host/device copy는 관찰 가능한 경계다

CUDA 연산 출력은 GPU allocation이 authoritative한 상태로 남는다. `data`, `item`, `grad`, `to(tensor, "cpu")`, `json.stringify`, checkpoint/Safetensors/ONNX 저장처럼 실제 숫자를 CPU에서 읽거나 직렬화하는 API가 host 경계다. host mirror가 stale이면 이때 D2H copy가 일어나고, 이미 동기화된 mirror가 있으면 중복 copy는 생략한다. `clone`과 `detach`는 D2D copy이며 `device`, `shape`, `dtype`, `numel` 같은 metadata 조회는 값 전체를 읽지 않는다. H2D는 Tensor 생성, `to(..., "cuda")`, CUDA non-scalar `backward`에 CPU gradient seed를 넘길 때, 또는 CUDA embedding ID와 sparse CE target을 검증 후 forward마다 raw `uint32`로 한 번 upload할 때 발생한다. 두 backward는 saved IDs를 재사용한다. sparse CE의 upload는 정확히 `4 * rows` bytes이고 loss/gradient 관찰 전 D2H는 없다. CUDA SGD와 Adam은 값 전체가 아닌 공유 4-byte transaction status만 optimizer step마다 host에서 확인한다.

`cuda_stats()`는 `allocated_bytes`, `peak_allocated_bytes`, allocation/free 횟수, `h2d_bytes`, `d2h_bytes`, `d2d_bytes`, 전체 및 종류별 kernel launch 횟수를 반환한다. matmul 합계는 `cublas_matmul_launches`와 `reference_matmul_launches`로 분할되고 compute 형식은 `float32_matmul_launches`·`float16_matmul_launches`·`bfloat16_matmul_launches`로 다시 분할된다. `cublas_fast_matmul_launches`와 `mixed_matmul_fallback_launches`는 저정밀 backend 선택을 검증한다. exact-form GELU는 `gelu_launches`, LayerNorm forward와 input/parameter backward는 `layer_norm_launches`, embedding gather/backward는 `embedding_launches`, sparse CE의 row-statistics·scalar-loss·backward는 `cross_entropy_launches`, optimizer kernel은 `optimizer_launches`로 구분된다. attention 합계 `attention_launches`는 서로 배타적인 `reference_attention_launches`·`warp_attention_launches`·`parallel_attention_launches`·`fused_attention_launches`로 분할된다. `fast_attention_forward_launches`는 그중 fast f32 warp forward를 겹쳐 세는 진단 counter다. full q/k/v 기본 fused 경로는 fast warp forward 1회와 fused backward 2회로 합계 3회다. legacy packed fallback은 warp 1회 + parallel 5회, serial fallback은 reference forward/backward 각 1회다. frozen role이 있으면 필요한 backward kernel과 gradient allocation만 만든다. sparse CE forward는 두 kernel이므로 이 counter를 2회, logits backward까지 수행하면 합계 3회 증가시킨다. `cuda_reset_stats()`는 현재 allocation accounting을 보존하면서 transfer/launch counter를 0으로 만든다.

```sura
autograd.cuda_reset_stats()
output is autograd.sum(autograd.relu(autograd.matmul(x, weight)))
before_read is autograd.cuda_stats()
assert_eq(before_read.h2d_bytes, 0)
assert_eq(before_read.d2h_bytes, 0)
value is autograd.item(output)
after_read is autograd.cuda_stats()
assert_eq(after_read.d2h_bytes, 4)
```

CUDA leaf gradient도 `backward`와 `sgd`/`adam` 사이에는 device에 남는다. `grad(parameter)`는 확인을 위해 gradient를 host 배열로 복사하지만 GPU gradient allocation 자체를 없애지 않는다. `zero_grad`는 device gradient를 0으로 만들고 다음 backward는 그 buffer에 다시 누적한다.

checkpoint v3는 CUDA visible weight와 f32 master, SGD velocity 또는 Adam m/v·metadata를 보존한다. exact resume에는 `load_checkpoint(path, {optimizer: true, device: "cuda"})`를 사용한다. CUDA optimizer state를 CPU로 조용히 바꾸는 것은 거부하며 `{optimizer: false}`는 visible weight만 복원한다. v1/v2도 읽는다.

## 3. causal attention은 no-O(T²)-workspace fused recomputation을 사용한다

`autograd.causal_attention(q, k, v, [options])`는 CPU Tensor와 같은 장치의 resident CUDA `float32` q/k/v를 지원한다. forward는 전체 `[T,T]` score/probability 행렬을 만들지 않고 online-softmax recurrence를 사용한다. 기본 `precision: "auto"`에서 `T >= 8`이면 한 warp가 한 attention 행의 q·k dot reduction과 value update를 협력하는 f32 kernel 1회를 실행한다. backward는 저장된 행별 max와 reciprocal sum으로 probability를 다시 계산한다. dQ는 query 행과 feature tile별 warp가, dK/dV는 key 행과 feature tile별 warp가 각각 단독 소유하므로 atomic write race 없이 결정적이다. full q/k/v backward는 dQ 1회와 결합 dK/dV 1회, 총 2개 kernel이다. plan은 graph에 저장되어 forward 뒤 환경변수가 바뀌어도 matching backward를 사용한다. 공개 output과 gradient는 `float32`이고 중간 tensor payload H2D/D2H는 없다.

기본 fused plan의 계산량은 여전히 O(T²)이지만 저장하는 attention 보조 상태는 행별 max와 reciprocal sum뿐이라 O(B·T)이다. 확률이나 dScore의 `[T,T]` buffer를 만들지 않는다. `SURA_CUDA_ATTENTION_FUSED=0|false|off`로 fused path를 끄면 기존 warp forward + packed causal backward를 사용하며, 그 workspace가 한도를 넘으면 serial reference forward/backward로 fallback한다. `SURA_CUDA_ATTENTION_PARALLEL=0|false|off`는 fused와 packed 경로를 모두 끈다. `T < 8`도 reference 경로다.

```sura
use autograd

query is autograd.randn([1, 2, 4, 8], {seed: 1, dtype: "float32", requires_grad: true})
key is autograd.randn([1, 2, 4, 8], {seed: 2, dtype: "float32", requires_grad: true})
value is autograd.randn([1, 2, 4, 8], {seed: 3, dtype: "float32", requires_grad: true})
context is autograd.causal_attention(query, key, value, {precision: "auto"})
autograd.backward(autograd.mean(context))
```

`precision: "strict"`는 fast f32 path를 사용하지 않고 score와 reduction을 f64로 계산하는 reference forward/backward를 고정한다. 매우 큰 유한 f32 operand처럼 f32 dot product가 overflow할 수 있는 검증에는 이 mode를 사용한다. `precision: "auto"`는 가능한 최적화 plan을 고르고 안전하게 fallback한다. `precision: "fast"`는 resident CUDA, `T >= 8`, parallel/fused 활성 상태를 명시적으로 요구하며 조건이 맞지 않으면 조용히 느린 경로로 내려가지 않고 오류를 낸다. 이 구현은 score matrix와 O(T²) workspace를 없앤 FlashAttention식 recomputation이라는 점에서는 진전이지만, Q/K/V tile을 shared memory에 올리거나 Tensor Core/저정밀 연산을 융합한 FlashAttention kernel은 아니다.

legacy packed fallback의 pair 수는 flattened batch/head 수를 `B`라고 할 때 `B * T * (T + 1) / 2`이고 workspace는 정확히 `4 * pairs` bytes다. 기본 한도는 64 MiB이며 `SURA_CUDA_ATTENTION_WORKSPACE_MB`로 최대 4096 MiB까지 바꿀 수 있다. 이 한도는 fused 기본 경로가 아니라 legacy packed fallback에만 적용된다.

## 4. byte/BPE tokenizer와 seek-streaming dataset

`tokenizer.byte()`는 UTF-8 문자열의 raw byte를 ID `0..255`로 바꾼다. 선택적인 BOS/EOS/PAD ID를 추가할 수 있고 versioned `.suratok` 파일로 저장한다.

```sura
use tokenizer

tok is tokenizer.byte({bos_id: 256, eos_id: 257, pad_id: 258})
ids is tokenizer.encode(tok, "수라🙂", {add_bos: true, add_eos: true})
print(tokenizer.decode(tok, ids))
tokenizer.save(tok, "byte.suratok")
```

`tokenizer.train_bpe`는 최대 1 MiB corpus와 최대 vocabulary 4096 범위에서 결정적인 byte-level BPE merge를 학습한다. UTF-8을 정규화하지 않고 byte를 보존하므로 학습 corpus에 없던 한국어·이모지도 lossless하게 round-trip한다.

```sura
bpe is tokenizer.train_bpe("banana banana bandana", {vocab_size: 384, min_frequency: 2})
ids is tokenizer.encode(bpe, "수라 banana")
assert_eq(tokenizer.decode(bpe, ids), "수라 banana")
tokenizer.save(bpe, "model.suratok")
```

학습은 64 MiB-token work budget을 넘으면 실패하며 normalization, pre-tokenization, dropout, Hugging Face tokenizer 파일 호환은 제공하지 않는다. `dataset.pack_text`는 학습된 가장 긴 token 길이만큼 raw suffix를 보존한 뒤 다시 encode하여 file/text chunk 경계를 가로지르는 merge를 유지한다.

`dataset.pack_text`는 literal text 또는 파일 목록을 versioned little-endian uint32 token shard로 만든다. `dataset.open`은 checksum과 구조를 검증하고, `dataset.next`는 필요한 sample 위치만 seek/read해서 non-gradient `float32` ID Tensor를 반환한다.

```sura
use tokenizer
use dataset

tok is tokenizer.byte()
dataset.pack_text(["corpus-000.txt", "corpus-001.txt"], tok, "train.suradata", {input: "files", chunk_bytes: 1048576})
loader is dataset.open("train.suradata", {batch_size: 8, sequence_length: 128, stride: 128, shuffle: true, seed: 42, rank: 0, world_size: 1})
batch is dataset.next(loader)
print(batch.sample_ids)
dataset.close(loader)
```

파일 입력은 chunk 단위로 pack하며, 학습 batch는 seek 기반으로 읽는다. 현재 loader는 `mmap`, background worker, pinned-memory prefetch를 사용하지 않는다. `rank`/`world_size`는 결정적이고 겹치지 않는 sample partition을 제공하지만, gradient 동기화 자체는 다음 절의 별도 API가 담당한다. 실행 가능한 작은 학습 예제는 `examples/tokenizer_dataset_training.sura`에 있다.

## 5. 분산 gradient 동기화는 공유 파일시스템 방식이다

각 프로세스가 같은 parameter 순서와 shape로 backward를 끝낸 뒤 `autograd.all_reduce_gradients`를 호출한다.

```sura
use autograd

report is autograd.all_reduce_gradients(parameters, {rendezvous: "shared/sura-gradients", run_id: "experiment-42", step: step, rank: rank, world_size: world_size, average: true, timeout_ms: 30000})
```

각 rank는 checksum이 있는 CPU `float64` gradient 파일을 공유 경로에 원자적으로 게시하고, 모든 rank가 도착하면 검증 후 합계 또는 평균을 적용한다. `world_size: 1`은 파일을 만들지 않는 검증된 no-op이다.

프로세스별로 다른 `SURA_CUDA_DEVICE`를 설정하는 process-per-device 실행 자체는 가능하지만, 현재 파일 all-reduce는 resident CUDA gradient를 명시적으로 거부한다. 지원되는 것은 CPU `double` gradient의 공유 파일 동기화뿐이다. NCCL이나 GPU collective가 아니며 GPU memory의 gradient를 직접 동기화하지 않는다. 공유 스토리지의 지연·대역폭·정리 정책도 사용자가 운영해야 한다. 매 실행에는 충돌하지 않는 `run_id`와 단조롭게 증가하는 `step`을 사용한다.

## 6. Safetensors, PyTorch, ONNX

Sura는 네 가지 dtype의 표준 Safetensors weight를 Python 없이 직접 읽고 쓴다. resident CUDA Tensor를 저장하면 명시적인 D2H materialization이 발생하며, 파일에는 device placement, gradient 또는 CUDA optimizer 상태가 들어가지 않는다. loader는 CPU Tensor를 반환한다.

```sura
use autograd

autograd.save_safetensors({embedding: embedding_weight, head: head_weight}, "model.safetensors")
weights is autograd.load_safetensors("model.safetensors", {requires_grad: true})
```

legacy `.pt`/`.pth` state dict는 명시적인 helper로 Safetensors를 거쳐 교환한다. 이 helper에는 Python, PyTorch, `safetensors` package가 필요하며 `torch.load(..., weights_only=True)`를 사용한다. 신뢰하지 않는 pickle checkpoint를 안전한 것으로 간주해서는 안 된다.

```powershell
python tools/sura_torch_bridge.py pt-to-safetensors model.pt model.safetensors
python tools/sura_torch_bridge.py safetensors-to-pt model.safetensors model.pt
python tools/sura_torch_bridge.py inspect model.safetensors
```

ONNX typed initializer weight 교환과 제한된 CPU graph 실행은 서로 다른 API다.

```sura
use autograd

autograd.save_onnx_weights({embedding: embedding_weight, head: head_weight}, "model.onnx")
weights is autograd.load_onnx_weights("model.onnx")
x is autograd.tensor([[1, 2]])
outputs is autograd.run_onnx("inference.onnx", {X: x})
```

Sura가 저장한 파일은 initializer를 Identity output으로 노출하는 유효한 ONNX ModelProto다. weights loader는 `FLOAT`, `DOUBLE`, `FLOAT16`, `BFLOAT16` raw-data initializer를 읽는다. `run_onnx`는 CPU에서 IR 3~10, 기본 opset 7~18, 최대 4,096 node의 topological graph를 실행한다. ValueInfo의 dtype·정적 shape 선언은 입력, initializer, 중간값과 출력에 적용한다. 지원 연산은 `Identity`, 기본 산술, `Neg`, `MatMul`, `Relu`, `Tanh`, `Sigmoid`, `Gemm`, 전체 축 순열 `Transpose`, `Flatten`, 제한된 INT64 shape initializer를 쓰는 `Reshape`, 마지막 축 `Softmax`다. custom domain, GPU 입력, external data, 일반 integer Tensor initializer, zero-size output, Conv, symbolic/dynamic shape, control flow와 임의 ONNX graph 실행은 지원하지 않는다.

## 7. 동일 하드웨어 공개 benchmark

네 benchmark 경로의 범위는 서로 다르다. 기존 one-shot benchmark는 256×256 rank-2 `float32` forward matmul 하나를 측정한다. Sura CPU와 PyTorch CPU는 모두 CPU thread 1개로 고정하고, Sura CPU, one-shot Sura CUDA, 선택적인 PyTorch CPU/CUDA가 같은 Safetensors 입력을 사용하며 process startup은 제외한다. 이 경로의 Sura CUDA 시간에는 CPU Tensor를 받는 legacy public API의 host/device copy와 synchronization이 포함된다.

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\sura_ai_benchmark.ps1 -Engine .\SuraLanguage.exe
```

resident benchmark는 setup/upload와 process startup을 제외하고 각 sample을 명시적으로 synchronize한다. forward는 `relu(matmul(input, weight))`, training은 `zero_grad + matmul + relu + mse + backward + SGD`이며 Sura와 PyTorch가 같은 Safetensors 초기 상태를 사용한다.

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\sura_ai_resident_benchmark.ps1
```

2026-07-11 생성된 `artifacts/ai_resident_cuda_cublas_benchmark.md`의 RTX 4060, 256×256 f32, warmup 3회/측정 10회 결과는 다음과 같다.

| 측정 | Sura 1.8 | PyTorch 2.11.0+cu128 | Sura/PyTorch |
|---|---:|---:|---:|
| resident forward 중앙값 | 0.158 ms | 0.0894 ms | 1.767× |
| resident SGD training 중앙값 | 3.132 ms/step | 1.4153 ms/step | 2.213× |

이 실행에서 Sura forward는 cuBLAS SGEMM 10회와 ReLU 10회를 사용했고 최종 scalar 관찰 전 H2D/D2H/D2D가 모두 0이었다. training 10 step은 tensor payload H2D 0, D2D 18,350,080 bytes였고 transactional SGD의 공유 status 때문에 D2H가 40 bytes였다. 즉 bulk Tensor payload는 resident였지만 완전한 무동기화 경로는 아니다. 결과는 현재 구현된 좁은 eager f32 workload에서 Sura가 PyTorch보다 forward 1.767배, training 2.213배 느렸음을 보여 준다.

이 mixed-compute benchmark artifact는 typed-storage 구현 전의 f32-storage compute-only 경로를 측정한 역사적 기준선이다. Sura의 당시 저정밀 행은 f32 storage/output/gradient이고 PyTorch 행은 native 2-byte storage/output이므로 현재 typed-storage 성능으로 재해석하면 안 된다.

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\sura_mixed_compute_benchmark.ps1 -Engine .\SuraLanguage.exe -Python C:\path\to\python.exe -CublasLibrary C:\path\to\cublas64_12.dll -Warmup 10 -Runs 50
```

공개 수치를 오염시키지 않도록 runner는 실행 전 7개의 1초 `nvidia-smi` 표본을 수집해 중앙값을 gate로 사용하고 원시 표본과 peak도 report에 남긴다. 기본 한도는 5%다. `-AllowBusyGpu`는 기능·counter 진단용 결과만 만들며 report의 `performance_valid`를 false로 기록한다. 2026-07-12 canonical 실행은 Codex desktop 렌더링 기준 부하가 지속되어 한도를 명시적으로 15%로 설정했고, 표본 `13, 13, 12, 12, 13, 12, 11`의 중앙값 12%·peak 13%를 그대로 공개한다. 따라서 이는 동일 하드웨어·동일 입력 비교 증거이지만 완전한 lab-idle 측정으로 해석하면 안 된다.

해당 RTX 4060, aligned 1024×1024, warmup 10회/측정 50회 결과는 다음과 같다. output·dInput·dWeight의 sum/L1/L2/first/center/last fingerprint가 dtype별 hard tolerance를 모두 통과했고, timed payload H2D/D2H는 0이며 `performance_valid: true`다.

| compute dtype / workload | Sura 1.8.1 | PyTorch 2.11.0+cu128 | Sura/PyTorch |
|---|---:|---:|---:|
| f32 forward | 0.6794 ms | 0.3197 ms | 2.125× |
| f32 forward+backward | 3.6830 ms | 0.8921 ms | 4.128× |
| f16 forward | 0.4611 ms | 0.1163 ms | 3.965× |
| f16 forward+backward | 3.2583 ms | 0.4102 ms | 7.943× |
| bf16 forward | 0.4995 ms | 0.1231 ms | 4.058× |
| bf16 forward+backward | 4.0890 ms | 0.4980 ms | 8.211× |

모든 역사적 benchmark 행에서 Sura가 느리다. 이 결과의 engine SHA-256은 `d2fccece42264704b941ae1d7a6c058cd18f09e797476431b9fcae09f27eb70e`이며 typed-storage 구현 이전 결과다. 전체 조건은 `artifacts/cuda_mixed_compute_benchmark.json`과 `.md`에 있다.

직접 causal-attention benchmark는 projection과 head split/merge를 제외하고 `B,H,T,D` 입력의 forward + scalar sum + backward만 측정한다. 같은 Safetensors q/k/v, 같은 GPU, sample 경계 동기화, 수치 fingerprint 허용치, timed H2D/D2H 0과 reference/warp/packed launch 계약을 자동 검증한다.

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\sura_attention_benchmark.ps1 -Engine .\SuraLanguage.exe
```

2026-07-12 RTX 4060, warmup 10회/측정 50회 결과는 다음과 같다.

| 직접 causal attention | Sura 1.8 | PyTorch 2.11.0+cu128 | Sura/PyTorch |
|---|---:|---:|---:|
| B1/H4/T64/D32 중앙값 | 1.5076 ms | 0.4649 ms | 3.242× |
| B1/H4/T128/D64 중앙값 | 5.6562 ms | 0.4282 ms | 13.209× |

T128에서 병렬 경로는 같은 Sura serial reference 215.8950 ms보다 38.170배 빨랐다. 그래도 PyTorch보다 느리며, sequence가 길어질수록 fused tiled FlashAttention 부재가 더 크게 드러난다. 기본 경로의 실행당 attention counter는 warp forward 1회 + packed backward 5회이고 fallback은 reference forward/backward 각 1회다. 세 report가 사용한 engine SHA-256은 `3270a9033f2272b648ac30c7edfa639675fbfb5668bae8ede9f3e5ebaf850577`이다.

one-shot 결과는 `artifacts/ai_benchmark.json`/`.md`, resident 결과는 `artifacts/ai_resident_cuda_cublas_benchmark.json`/`.md`, mixed compute 결과는 `artifacts/cuda_mixed_compute_benchmark.json`/`.md`, attention 결과는 `artifacts/attention_benchmark.json`/`.md`, `artifacts/attention_benchmark_t128.json`/`.md`, `artifacts/attention_benchmark_t128_serial.json`/`.md`에 기록된다. 숫자를 비교할 때 `performance_valid`, engine hash, CPU/GPU, driver, dependency 가용성, cuBLAS library, raw sample을 함께 공개해야 한다. 이 결과로 주장할 수 있는 것은 해당 workload와 하드웨어의 성능뿐이다. Transformer 전체 학습 속도, 수렴 품질, Python 언어 전체보다 우월하다는 결론으로 확대하면 안 된다.

resident CUDA 기능의 회귀 경계는 다음 테스트가 담당한다.

- `tests/26_cuda_device_placement.sura`: 생성 시 device 기본 dtype, `device`/`to`, D2D clone/detach, mixed-device와 잘못된 dtype 거부
- `tests/27_cuda_residency.sura`: matmul → ReLU → elementwise → sum chain이 host transfer 없이 실행되고 `item`만 4-byte D2H를 만드는지 검증
- `tests/28_cuda_backward.sura`: matmul/ReLU/reduction backward의 gradient, backward 전 무전송, `grad`의 명시적 D2H, GPU SGD의 4-byte transaction status 검증
- `tests/30_cuda_adam.sura`: CPU/CUDA Adam parity, step당 4-byte status D2H, 실패한 multi-parameter step의 전체 rollback 검증
- `tests/33_cuda_nd_linear.sura`: `[B,T,D] @ [D,F] + [F]` forward/backward, B×T weight·bias 축약, 무전송과 host-lazy parity 검증
- `tests/34_cuda_gelu.sura`: exact-form forward/backward CPU parity, 양쪽 tail 안정성, 무전송과 host-lazy parity 검증
- `tests/35_cuda_layer_norm.sura`: last-axis affine/no-affine forward/backward, large-f32 안정성, saved-state 재사용, 무전송과 host-lazy parity 검증
- `tests/36_cuda_sgd_transaction.sura`: multi-parameter CUDA SGD의 candidate commit, 4-byte status와 실패 rollback 검증
- `tests/37_cuda_cublas_dispatch.sura`: optional cuBLAS SGEMM dispatch와 resident reference PTX fallback 검증
- `tests/38_cuda_embedding.sura`: raw-u32 ID upload 1회, gather parity, deterministic duplicate-ID backward와 host-lazy 관찰 검증
- `tests/39_cuda_cross_entropy_ids.sura`: CPU target의 범위·정수·shape·no-grad 검증과 CUDA target 거부, rank-3 CPU/CUDA loss·logits-gradient parity, raw-u32 target upload 1회, implicit/explicit scalar seed, max-subtracted extreme-logit 안정성, 관찰 전 무-D2H와 정확한 host-lazy transfer 검증
- `tests/40_cuda_causal_attention.sura`, `tests/41_cuda_causal_attention_graph.sura`, `tests/41_cuda_causal_attention_extreme.sura`: CUDA causal attention rank-3/4 parity, causal mask, q/k/v gradient, alias·retain·version 경계, frozen role, 0-transfer와 `3×10^38`/초대형 score 안정성
- `tests/44_cuda_attention_parallel.sura`: no-O(T²)-workspace fused backward의 CPU parity, 반복 결정성, frozen role, retain-graph 누적, memory/launch counter와 legacy packed·serial fallback 계약
- `tests/45_cuda_attention_warp_forward.sura`: fast f32 warp online-softmax forward의 tail dimension, extreme-score 안정성, no-grad allocation, 결정성과 matching fused backward 계약; strict f64 extreme-score 경계는 `tests/41_cuda_causal_attention_extreme.sura`
- `tests/46_cuda_attention_warp_edges.sura`: T=8/9/17과 D/Dv=1/31/32/33/65의 rank-2/4 warp/lane tail parity 및 기본 3-launch partition
- `tests/52_cuda_attention_precision.sura`: invalid/CPU·short·env-disabled fast fail-closed, auto fused·legacy·reference dispatch, 양방향 sticky plan, strict f64 parity, q=k=v alias, scaled backward와 retain/release memory 계약
- `tests/47_cuda_mixed_matmul.sura`: f16/bf16 compute forward/backward, f32 public storage와 gradient, cuBLAS FAST/reference fallback counter
- `tests/48_cuda_autocast.sura`: 중첩 상태 복원, CUDA-only policy, linear의 현재 autocast 적용, matmul의 명시 override와 sticky backward plan
- `tests/49_cuda_loss_scaling.sura`: scaled 누적, reciprocal/scale 검증, payload 무전송 `grad_info`, empty-gradient no-op, transactional unscale, found-inf 전체 rollback과 optimizer guard
- `tests/42_cuda_transpose.sura`: rank-4 multi-head layout transpose와 inverse backward, negative/default axes, lazy host와 전용 counter
- `tests/43_cuda_multihead_training.sura`: embedding→LayerNorm→q/k/v→두 head split/merge→causal attention→sparse CE→Adam의 resident end-to-end 손실 감소

## 지금 대형 AI에 필요한 다음 단계

우선순위는 다음과 같다.

1. cuBLAS 범위를 strided/batched matmul로 확장하고 profiler/SASS로 FAST compute의 실제 Tensor Core instruction 사용을 shape별 검증
2. 일반 broadcasting, CUDA softmax/dense cross-entropy, CUDA target Tensor 경로와 CUDA gradient norm
3. correctness-first one-thread-per-row LayerNorm을 warp/block-parallel Welford kernel로 최적화
4. low-precision activation/output과 mixed backward 가속, 압축 optimizer state, dynamic GradScaler metadata
5. 현재 no-O(T²)-workspace deterministic recomputation을 shared-memory tiled/Tensor Core·저정밀 FlashAttention 계열 kernel로 발전시키고 KV cache 추가
6. 외부 tokenizer 호환, mmap/prefetch/pinned-memory data pipeline
7. resident CUDA gradient용 NCCL all-reduce와 multi-node launcher/checkpoint recovery
8. ONNX executor에 Conv·추가 shape 연산·동적 shape를 단계적으로 추가하고 외부 runtime과 conformance 검증
9. 모델별 end-to-end 학습 throughput, memory, 정확도 benchmark
10. CUDA token/target Tensor 입력, reusable ID cache/prefetch와 고성능 embedding sparse/segmented backward 및 vocabulary-parallel/fused sparse CE

현재 정확한 표현은 “실제 2-byte f16/bf16 CUDA weight storage, typed cuBLAS/PTX matmul, f32 output/gradient/master optimizer, checkpoint v3, no-O(T²)-workspace fused attention과 제한된 resident graph를 갖춘 언어”다. low activation, 빠른 mixed backward, 범용 mixed-precision operator stack, tiled/Tensor Core·저정밀 attention과 distributed GPU stack은 아직 없다. 공개 benchmark에서도 PyTorch가 더 빠르므로 “PyTorch보다 빠른 대형 AI 프레임워크”라고 부를 근거는 없다.
