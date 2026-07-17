# Sura 네이티브 AI 가이드

최종 업데이트: 2026-07-15

Sura 1.11.1은 Python, NumPy, PyTorch를 설치하지 않아도 작은 신경망을 만들고 학습할 수 있다. `use nn`을 권장하며 `use ai`도 같은 모듈의 별칭이다. 이 고수준 모듈은 CPU 네이티브 dense MLP에 초점을 맞춘다. 직접 Tensor 연산과 사용자 정의 gradient graph를 조합하려면 [Tensor와 자동미분 가이드](AUTOGRAD.md)의 `use autograd`를 사용한다. 작은 causal 언어 모델은 [Transformer 가이드](TRANSFORMER.md), CUDA·dtype·데이터·분산·모델 호환의 정확한 범위는 [AI backend와 scale 가이드](GPU_AND_SCALE.md)에 있다.

## 1분 안에 XOR 학습하기

```sura
use nn

inputs is [[0, 0], [0, 1], [1, 0], [1, 1]]
targets is [0, 1, 1, 0]

model is nn.mlp([2, 8, 1], {activation: "tanh", task: "binary", seed: 2026})
run is nn.train(model, inputs, targets, {optimizer: "adam", epochs: 3000, learning_rate: 0.05, batch_size: 4, target_loss: 0.01, seed: 2026})

print(run.loss)
print(nn.predict(run.model, inputs))
print(nn.classify(run.model, inputs))
print(nn.evaluate(run.model, inputs, targets))

nn.save(run.model, "xor-model.json")
loaded is nn.load("xor-model.json")
print(nn.classify(loaded, inputs))
```

실행 가능한 전체 예제는 `examples/native_xor_ai.sura`, 회귀와 다중 분류까지 포함한 회귀 테스트는 `tests/12_native_nn.sura`에 있다.

## 모델 만들기

`nn.mlp(layer_sizes, options)`의 첫 배열은 입력 크기, 은닉층 크기, 출력 크기다. 예를 들어 `[4, 16, 8, 3]`은 입력 4개, 은닉층 2개, 출력 클래스 3개인 모델이다.

주요 생성 옵션:

| 옵션 | 기본값 | 의미 |
|---|---:|---|
| `activation` | `"relu"` | 은닉층 활성화: `relu`, `tanh`, `sigmoid`, `linear` |
| `task` | 출력 1개면 `binary`, 아니면 `multiclass` | `binary`, `multiclass`, `regression` |
| `output_activation` | task에서 자동 선택 | `sigmoid`, `softmax`, `linear` 등을 직접 지정 |
| `init` | `"auto"` | `auto`, `xavier`, `he`, `zeros` |
| `seed` | `42` | 재현 가능한 가중치 초기화 시드 |

모델은 `sura.nn.mlp.v1` 형식의 평범한 Sura 딕셔너리다. 레이어, 가중치, 편향, 활성화를 직접 확인할 수 있고 JSON으로 저장된다.

## 학습하기

`nn.train(model, inputs, targets, options)`는 원본 모델을 바꾸지 않고 다음 필드를 가진 결과를 반환한다.

- `model`: 학습된 새 모델
- `loss`, `loss_name`: 마지막 손실과 손실 종류
- `epochs`, `samples`, `optimizer`: 실행 정보
- `history`: `{epoch, loss}` 기록
- `stopped_early`, `converged`: 조기 종료 상태

지원 학습 기능:

- 네이티브 역전파
- Adam과 momentum SGD
- 미니배치와 재현 가능한 셔플
- binary/categorical cross entropy와 MSE
- gradient norm clipping과 weight decay
- `patience`/`min_delta` 조기 종료와 `target_loss` 종료
- binary scalar, one-hot 배열, zero-based class label 자동 처리
- 재사용 가능한 feature standardizer와 seed 기반 train/test split

주요 학습 옵션은 `optimizer`, `epochs`, `learning_rate`, `batch_size`, `shuffle`, `seed`, `loss`, `momentum`, `beta1`, `beta2`, `epsilon`, `weight_decay`, `clip_norm`, `patience`, `min_delta`, `restore_best`, `target_loss`, `history_every`다.

## API

| API | 역할 |
|---|---|
| `nn.mlp(sizes, [options])` | dense MLP 생성 |
| `nn.forward(model, inputs)` | 순전파 |
| `nn.predict(model, inputs)` | 확률 또는 회귀값 반환 |
| `nn.train(model, inputs, targets, [options])` | 모델 학습 |
| `nn.classify(model, inputs, [threshold])` | binary/multilabel/softmax 클래스 반환 |
| `nn.evaluate(model, inputs, targets, [options])` | loss와 가능한 경우 accuracy 반환 |
| `nn.summary(model)` | 구조, 활성화, 파라미터 수 반환 |
| `nn.one_hot(labels, class_count)` | 클래스 번호를 one-hot으로 변환 |
| `nn.fit_standardizer(inputs)` | feature별 평균과 표준편차 학습 |
| `nn.standardize(inputs, standardizer)` | 학습한 통계로 한 샘플 또는 배치 표준화 |
| `nn.split(inputs, targets, [options])` | 입력/정답 쌍을 재현 가능하게 train/test 분할 |
| `nn.save(model, path)` | 검증 후 JSON 저장 |
| `nn.load(path)` | JSON 모델 로드 및 검증 |

각 API는 `nn_train`처럼 `nn_` 접두사를 붙인 직접 함수로도 사용할 수 있다.

## 작은 Transformer 만들기

`nn`은 dense MLP 전용이고 Transformer 모델 객체를 자동으로 만들어 주지는 않는다. 대신 `autograd`에는 다음과 같은 Transformer 구성 요소가 있다.

- 한 개의 `-1` 추론 차원을 허용하는 `reshape`
- 앞쪽 batch 차원을 broadcast하는 rank 2 이상 `matmul`
- 임의의 두 축을 바꾸는 `transpose`
- exact `gelu`와 마지막 축 `layer_norm`
- 반복 token ID의 gradient를 합산하는 `embedding`
- rank 2 이상 CPU/resident CUDA online-softmax `causal_attention`
- one-hot target이 필요 없고 CPU 및 host-target resident CUDA logits을 지원하는 `cross_entropy_ids`

이 연산으로 embedding, pre-norm attention, residual, GELU feed-forward, language head로 이루어진 한 블록 모델을 작성할 수 있다. `tests/19_tiny_transformer_training.sura`는 vocabulary 4, sequence 길이 3인 결정적 데이터에 모델을 실제로 학습해 손실과 다음-token 예측을 검증한다. 이는 학습 경로의 정확성을 입증하는 회귀 테스트이지 일반적인 자연어 능력이나 대형 언어 모델 품질을 입증하는 benchmark는 아니다.

`nn` 고수준 MLP API는 현재 CPU다. 저수준 `autograd`에서는 projection·embedding·GELU·LayerNorm·reshape·rank 2–8 transpose·causal attention·sparse `cross_entropy_ids`를 resident CUDA `float32`로 연결할 수 있다. `tests/43_cuda_multihead_training.sura`는 두 head의 split/merge부터 q/k/v backward와 Adam까지 실제 GPU에서 학습해 이 경로를 검증한다.

## 제한된 GPU-resident 학습

`autograd.tensor`/`parameter`/`zeros`/`ones`/`randn`에 `{device: "cuda"}`를 주면 dtype 기본값은 `float32`가 되고 Tensor는 GPU memory를 가진다. `autograd.device(tensor)`로 placement를 확인하고 `autograd.to(tensor, "cpu"|"cuda")`로 명시적으로 복사할 수 있다. CUDA 학습 parameter는 mixed-device backward를 피하기 위해 처음부터 CUDA에서 생성한다.

```sura
use autograd

x is autograd.tensor([[1, 2], [3, 4]], {device: "cuda"})
y is autograd.tensor([[1], [0]], {device: "cuda"})
w is autograd.parameter([[0.1], [0.2]], {device: "cuda"})

repeat 20 do
  autograd.zero_grad([w])
  prediction is autograd.relu(autograd.matmul(x, w))
  loss is autograd.mse(prediction, y)
  autograd.backward(loss)
  autograd.sgd([w], 0.01, {momentum: 0.9, weight_decay: 0.0001})
end

print(autograd.item(loss))
```

이 예제의 tensor payload, 같은-shape elementwise MSE 합성, ReLU, 전체 mean, backward와 persistent GPU gradient는 device에 상주한다. matmul은 호환 cuBLAS가 동적으로 로드되면 SGEMM으로 dispatch하고, 없거나 `SURA_CUBLAS_DISABLE=1`이면 resident reference PTX로 fallback한다. SGD는 candidate update를 device에서 계산한 뒤 공유 finite-status 4-byte만 D2H로 확인해 전체 commit/rollback한다. 같은 경로는 contiguous ND projection, exact GELU, LayerNorm, CUDA weight embedding, rank 2–8 transpose, causal attention과 sparse `cross_entropy_ids`의 forward/backward도 지원한다. embedding ID와 sparse CE target은 host에서 검증해 매 forward raw `uint32`로 한 번 upload한다. 마지막 update를 `autograd.adam([w], 0.01)`으로 바꾸면 resident CUDA Adam을 사용한다. `data`/`item`/`grad`/`to(..., "cpu")`와 weight 저장은 실제 숫자를 읽는 host 경계다. `grad_info`는 gradient payload를 복사하지 않고 실제 gradient dtype/device/byte 수와 scale 상태만 조회한다. `cuda_info`와 `cuda_stats`/`cuda_reset_stats`로 matmul backend, allocation, transfer와 `transpose_launches`·`attention_launches`·`reference_attention_launches`·`warp_attention_launches`·`parallel_attention_launches`·`fused_attention_launches`·`fast_attention_forward_launches`·`cross_entropy_launches`를 확인할 수 있다.

projection matmul과 `linear`는 f32 storage를 유지하면서 f16/bf16 compute autocast를 선택할 수 있다. 명시적 `compute_dtype` options는 `matmul`에만 있고 autocast보다 우선한다. options 인수가 없는 `linear`는 호출 시점의 현재 autocast를 사용한다. loss scaling은 모든 gradient 후보를 먼저 finite 검사한 뒤 전체 commit한다.

```sura
scale is 65536
previous is autograd.autocast("bfloat16")
prediction is autograd.linear(x, w)
loss is autograd.mse(prediction, y)
autograd.autocast(previous)

autograd.backward_scaled(loss, scale)
status is autograd.unscale_gradients([w])
if status.found_inf then
  autograd.zero_grad([w])
else
  autograd.adam([w], 0.001)
end
```

`autocast`는 storage를 바꾸지 않고 matmul compute plan만 낮춘다. 별도로 actual f16/bf16 CUDA Tensor를 만들거나 cast할 수 있고 typed matmul은 그 2-byte storage를 직접 읽는다. output/gradient는 f32이며 low parameter는 f32 master를 사용한다. cuBLAS FAST counter는 Tensor-Core-eligible 요청 증거이지 profiler 증거를 대신하지 않는다. loss scaling과 `grad_info`의 scale/byte 계약은 dtype와 무관하게 f32 CUDA gradient에 적용된다.

이 subset은 visible f32/f16/bf16 CUDA storage를 지원하지만 typed low storage를 직접 소비하는 연산은 현재 matmul뿐이다. output/gradient와 GELU, LayerNorm, embedding, transpose, attention, sparse CE 등 나머지 resident graph는 f32다. CUDA SGD/Adam은 low parameter에 f32 master를 사용하고 4-byte status transaction으로 전체 commit한다. 일반 broadcasting, gradient clipping, dense softmax/cross-entropy와 batched-right matmul은 아직 없다.

checkpoint v3는 visible CUDA weight와 f32 master, SGD velocity, Adam m/v·step·beta product를 저장한다. exact resume는 `load_checkpoint(path, {optimizer: true, device: "cuda"})`로 CUDA leaf를 직접 복원한다. CUDA optimizer state의 기본 CPU restore는 거부하며 `{optimizer: false}`는 visible weight만 복원한다. gradient와 graph는 저장하지 않고 v1/v2도 읽는다.

## 텍스트 데이터와 모델 weight 연결

`tokenizer.byte`는 모든 UTF-8 text를 raw byte ID로 바꾼다. `tokenizer.train_bpe`는 bounded byte-level BPE를 결정적으로 학습하고 `tokenizer.encode`/`decode`와 versioned `.suratok` 저장을 공유한다. `dataset.pack_text`는 byte와 BPE tokenizer를 받아 chunk 경계의 BPE merge를 보존하고, `dataset.open`/`next`는 uint32 shard에서 다음-token batch를 seek 방식으로 읽는다.

```sura
use tokenizer
use dataset

tok is tokenizer.byte()
dataset.pack_text(["train-000.txt", "train-001.txt"], tok, "train.suradata", {input: "files", chunk_bytes: 1048576})
loader is dataset.open("train.suradata", {batch_size: 8, sequence_length: 128, shuffle: true, seed: 42})
batch is dataset.next(loader)
```

반환되는 `batch.input_ids`와 `batch.target_ids`는 embedding과 `cross_entropy_ids`에 바로 넣을 수 있는 non-gradient CPU `float32` Tensor다. CUDA weight embedding에는 `batch.input_ids`, resident CUDA logits의 sparse CE에는 `batch.target_ids`를 CPU 상태 그대로 전달한다. ID는 host에서 shape·정수·범위를 검증한 뒤 raw `uint32`로 한 번 upload되며, CUDA token/target Tensor로 먼저 옮기면 해당 v1 경로가 거부한다. loader는 mmap/prefetch가 아닌 file seek 방식이다. 실제 작은 학습 코드는 `examples/tokenizer_dataset_training.sura`에 있다.

학습 weight는 표준 Safetensors로 Python 없이 교환할 수 있다. PyTorch `.pt`/`.pth`는 `tools/sura_torch_bridge.py`로 Safetensors를 거친다. `save_onnx_weights`/`load_onnx_weights`는 initializer weight를 읽고 쓰고, `autograd.run_onnx(path, inputs)`는 검증된 CPU ONNX 일부를 실행한다. 실행 범위는 IR 3~10, 기본 opset 7~18, 최대 4,096 node이며 `Identity`, `Add`, `Sub`, `Mul`, `Div`, `Neg`, `MatMul`, `Relu`, `Tanh`, `Sigmoid`, `Gemm`, `Transpose`, `Flatten`, `Reshape`, 마지막 축 `Softmax`를 지원한다. `Transpose`는 전체 `perm` 순열 또는 기본 역순을, `Flatten`은 유효한 `axis`를 검사한다. `Reshape`의 두 번째 입력은 raw-data INT64 rank-1 initializer만 허용하며 최대 8개 차원, `0` 축 복사, 한 개의 `-1` 추론을 지원한다. 세 연산 모두 autograd 연결을 유지한다. ValueInfo에 dtype·정적 shape가 있으면 입력부터 중간값·출력까지 일치 여부를 검사하고 symbolic dimension은 거부한다. 임의 ONNX 모델 실행을 뜻하지 않는다.

## 적합한 부분과 아직 아닌 부분

작은 CPU 모델과 임베디드 앱에서는 Sura 쪽이 더 단순하다. 별도 가상환경과 패키지 설치가 없고, 모델이 언어 기본 값이라 JSON·HTTP·파일·게임/자동화 런타임과 바로 연결된다. 재현 가능한 초기화와 학습, 저장, 평가가 하나의 작은 표준 모듈 안에 있다.

하지만 대형 모델 생태계 전체에서 Python/PyTorch보다 낫다고 주장할 단계는 아니다. 실제 2-byte CUDA weight storage, checkpoint v3, score matrix와 O(T²) backward workspace를 만들지 않는 deterministic fused attention은 추가됐지만 동일 RTX 4060 attention 및 mixed-compute benchmark에서는 Sura가 PyTorch보다 느렸다. mixed backward 가속, low activation/output, shared-memory tiled/Tensor Core·저정밀 FlashAttention, 일반 broadcast·batched-right matmul, dense softmax/cross-entropy와 NCCL은 아직 없다.

기본 한도는 Tensor당 10,000,000 elements, live Tensor host buffer 512 MiB다. 실제 2-byte low weight가 있어도 Adam steady state는 low `18N`, f32 `16N`이며 activation/output은 f32다. 외부 tokenizer 호환, mmap/prefetch, instruction-level Tensor Core 검증, tiled/저정밀 FlashAttention, AdamW, KV cache, NCCL, 고차 미분과 임의 ONNX graph·Conv·동적 shape/control-flow 실행은 아직 없다.

공개한 2026-07-11 RTX 4060, 256×256 f32 resident benchmark에서 Sura의 cuBLAS forward 중앙값은 0.158 ms, PyTorch는 0.0894 ms였고, SGD training 중앙값은 각각 3.132 ms/step과 1.4153 ms/step이었다. Sura/PyTorch 비율은 forward 1.767×, training 2.213×로 이 좁은 workload에서도 Sura가 느렸다. setup/upload는 제외했고 training 10 step의 D2H 40 bytes는 transactional SGD status다. 상세 조건과 raw sample은 `artifacts/ai_resident_cuda_cublas_benchmark.md`에 있다.

지금 보장하는 범위는 Sura만으로 실제 MLP를 생성·학습·평가·저장할 수 있고, 작은 한 블록 causal Transformer를 직접 조합해 gradient와 학습을 검증할 수 있다는 것이다. ChatGPT급 모델, 장문 context 서비스, 고속 생성 서버를 완성했다는 뜻은 아니다.

## 검증

- Windows interpreter/JIT: `tests/12_native_nn.sura`
- Invalid model/data/options: `tools/sura_nn_smoke.ps1`
- Ubuntu/macOS: cross-platform smoke workflow
- Ubuntu ASan/UBSan: 번호 기반 전체 테스트 루프
- 모듈/API 탐색: `tools/sura_stdlib_modules_smoke.ps1`, `tools/sura_lsp_smoke.js`, `surapkg info/search`
- Tensor/자동미분: `tests/13_autograd_core.sura`, `tests/14_autograd_gradcheck.sura`, `tests/15_autograd_training.sura`, `tools/sura_autograd_smoke.ps1`
- Transformer 연산과 broadcast backward: `tests/16_transformer_ops.sura`
- Transformer 핵심 연산 유한차분 검증: `tests/17_transformer_gradcheck.sura`
- Transformer checkpoint와 optimizer 연속성 검증: `tests/18_transformer_checkpoint.sura`
- 한 블록 causal 모델 학습: `tests/19_tiny_transformer_training.sura`
- dtype/Safetensors/CUDA/data: `tests/20_safetensors.sura`, `tests/21_dtype_storage.sura`, `tests/22_cuda_backend.sura`, `tests/23_tokenizer_dataset.sura`
- shared-filesystem gradient all-reduce: `tools/sura_distributed_autograd_smoke.ps1`
- ONNX typed initializer 교환: `tests/25_onnx_weights.sura`
- bounded CPU ONNX 연산·입력 검증·backward·오류 경계: `tests/71_onnx_execution.sura`
- CUDA device 생성과 `device`/`to`: `tests/26_cuda_device_placement.sura`
- GPU-resident 연산 chain과 transfer 통계: `tests/27_cuda_residency.sura`
- CUDA backward, persistent gradient와 GPU SGD: `tests/28_cuda_backward.sura`
- CUDA Adam parity, transaction status와 rollback: `tests/30_cuda_adam.sura`
- CUDA SGD transaction status와 rollback: `tests/36_cuda_sgd_transaction.sura`
- optional cuBLAS SGEMM dispatch/reference PTX fallback: `tests/37_cuda_cublas_dispatch.sura`
- CUDA embedding raw-u32 ID upload와 deterministic duplicate-ID backward: `tests/38_cuda_embedding.sura`
- CUDA sparse CE의 CPU target validation/CUDA target 거부, raw-u32 upload 1회, stable loss와 logits backward, CPU parity 및 host-lazy transfer: `tests/39_cuda_cross_entropy_ids.sura`
- CUDA causal attention의 deterministic no-O(T²)-workspace fused backward, CPU parity, 결정성, frozen role, retain-graph, memory/launch counter와 legacy packed·serial fallback: `tests/44_cuda_attention_parallel.sura`
- fast f32 warp online-softmax forward, extreme score, frozen no-grad와 matching fused backward: `tests/45_cuda_attention_warp_forward.sura`; strict f64 extreme-score 경계: `tests/41_cuda_causal_attention_extreme.sura`
- rank-2/4의 T=8/9/17 및 D/Dv=1/31/32/33/65 warp/lane tail과 기본 3-launch 계약: `tests/46_cuda_attention_warp_edges.sura`
- attention precision invalid/CPU·short·env-disabled fast fail-closed, auto fused·legacy·reference dispatch, 양방향 sticky plan, strict f64 parity, q=k=v alias, scaled backward와 retain/release memory 계약: `tests/52_cuda_attention_precision.sura`
- f32-storage mixed compute/autocast: `tests/47_cuda_mixed_matmul.sura`, `tests/48_cuda_autocast.sura`
- actual 2-byte CUDA storage, typed matmul/master optimizer: `tests/50_cuda_typed_storage.sura`
- CUDA Adam/SGD checkpoint v3 exact resume: `tests/31_cuda_adam_checkpoint.sura`, `tests/51_cuda_sgd_checkpoint.sura`
- CUDA autocast 상태 복원·matmul override·linear의 현재 autocast·sticky backward plan: `tests/48_cuda_autocast.sura`
- scaled backward, payload 무전송 `grad_info`, empty-gradient no-op, transactional unscale와 found-inf rollback: `tests/49_cuda_loss_scaling.sura`
- 동일 입력·동일 하드웨어 one-shot matmul: `tools/sura_ai_benchmark.ps1`
- 동일 입력·동일 RTX 4060 resident forward/training: `tools/sura_ai_resident_benchmark.ps1`, `artifacts/ai_resident_cuda_cublas_benchmark.md`
- 동일 입력·동일 하드웨어 f32/f16/bf16 compute: `tools/sura_mixed_compute_benchmark.ps1` (유휴 GPU의 유효한 실행만 성능 수치로 사용)
- 동일 입력·동일 하드웨어 직접 causal attention: `tools/sura_attention_benchmark.ps1`
