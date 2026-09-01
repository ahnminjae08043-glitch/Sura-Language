# Sura Transformer 가이드

최종 업데이트: 2026-07-12

Sura의 `autograd` 모듈로 작은 causal Transformer를 직접 구성하고 학습할 수 있다. Sura 1.11.1은 packed dtype storage와 byte/byte-level BPE tokenizer, byte-token seek loader, 제한된 CUDA-resident forward/backward/SGD/Adam, 파일 기반 CPU gradient 동기화, Safetensors/ONNX weight 교환과 bounded CPU ONNX 실행을 제공한다. 현재 목표는 연산과 gradient의 정확성을 검증하고 작은 언어 모델 실험을 실행하는 것이다. 완성된 대형 모델 프레임워크나 ChatGPT급 서비스 런타임을 제공하는 것은 아니다.

## 현재 실행 범위

- Tensor는 `float64`, `float32`, `float16`, `bfloat16` packed CPU storage를 지원한다. resident CUDA는 f32와 실제 2-byte f16/bf16 storage를 지원한다. typed projection은 low storage를 직접 읽지만 output/gradient는 f32이고, low trainable parameter는 f32 master를 사용한다. 나머지 resident operator는 아직 f32-only다.
- 기본 live host Tensor buffer 합계는 512 MiB이고 Tensor당 기본 한도는 10,000,000 elements다. CPU parameter뿐 아니라 double gradient, optimizer moment, 중간 activation도 같은 host memory 예산을 사용한다. resident CUDA 연산의 output host storage는 관찰 전까지 할당하지 않으며 device allocation은 `cuda_stats()`로 별도 확인한다.
- causal attention forward는 CPU와 resident CUDA `float32`에서 online softmax로 전체 score 행렬을 저장하지 않지만 계산량은 sequence 길이 `T`에 대해 O(T²)다. 기본 CUDA `T >= 8` plan은 f32 warp-per-row forward와 probability를 재계산하는 deterministic fused backward를 사용해 O(T²) workspace 없이 full q/k/v graph를 3개 kernel로 실행한다. 이는 shared-memory tiled/Tensor Core FlashAttention은 아니다.
- CUDA는 제한된 f32-storage graph를 GPU memory에 상주시켜 contiguous `[...,M,K] @ [K,N]` shared-right projection, rank 2–8 transpose, CUDA weight embedding, causal attention, host-target sparse `cross_entropy_ids`, bias/elementwise/GELU/LayerNorm/reduction, backward와 SGD/Adam까지 실행한다. projection은 f32/f16/bf16 compute를 선택하며 autocast·scaled backward·transactional unscale을 지원한다. reshape/transpose로 두 개 이상의 head를 split/merge하는 Transformer도 끝까지 GPU에서 학습할 수 있다. embedding ID와 sparse CE target은 host에서 검증해 매 forward 한 번 raw `uint32`로 upload하고 backward에서 재사용한다. 아직 CUDA token/target Tensor 입력, softmax/dense cross-entropy, 일반 broadcasting과 batched right operand, tiled/Tensor Core 저정밀 attention은 없다.
- byte tokenizer, bounded byte-level BPE와 chunk-boundary 보존 pack, seek loader가 있다. mmap/prefetch는 없다.
- 공유 파일시스템 CPU gradient all-reduce가 있다. NCCL이나 GPU collective는 없다.
- KV cache는 제공하지 않는다.
- padding mask나 임의 attention mask, dropout도 현재 `causal_attention` API에 포함되지 않는다.

따라서 짧은 sequence와 작은 hidden dimension으로 구조와 학습을 검증하는 용도에 맞는다. 긴 문맥 학습이나 빠른 autoregressive 생성에는 맞지 않는다.

## Shape 약속

Transformer 예제에서는 다음 이름을 사용한다.

| 기호 | 의미 |
|---|---|
| `B` | batch 크기 |
| `T` | sequence 길이 |
| `V` | vocabulary 크기 |
| `D` | model dimension |
| `H` | attention head 수 |
| `Dh` | head dimension, 일반적으로 `D / H` |
| `F` | feed-forward hidden dimension |

주요 Tensor shape는 다음과 같다.

```text
input_ids        [B, T]
embedding weight [V, D]
hidden state     [B, T, D]
split q, k, v    [B, H, T, Dh]
attention output [B, H, T, Dh]
logits           [B, T, V]
target ids       [B, T]
loss             scalar
```

`matmul`은 마지막 두 차원을 행렬로 계산하고 앞쪽 차원을 broadcast한다. 따라서 `[B,T,D] @ [D,F]`는 `[B,T,F]`를 만든다. 이 shared-right projection 형태는 CPU와 resident CUDA 모두 forward/backward를 지원한다. 양쪽 operand에 batch prefix가 있는 일반 broadcast matmul은 아직 CPU 전용이다.

## Transformer 연산

### Reshape

`reshape(tensor, shape)`는 row-major 원소 순서를 유지한다. shape의 원소 수는 같아야 하며 정확히 하나의 `-1`을 사용할 수 있다.

```sura
x is autograd.tensor([[[1, 2, 3, 4], [5, 6, 7, 8]]])
heads is autograd.reshape(x, [1, 2, 2, -1])
# shape: [1, 2, 2, 2]
```

### ND/batched matmul

rank 2 이상의 입력을 받고 앞쪽 batch 차원을 trailing 방식으로 broadcast한다.

```sura
hidden is autograd.randn([2, 3, 4], {seed: 1})
weight is autograd.randn([4, 8], {seed: 2})
projected is autograd.matmul(hidden, weight)
# shape: [2, 3, 8]
```

resident CUDA에서는 왼쪽 `[...,M,K]`를 연속 `[prefix*M,K]` matrix로 평탄화하고 오른쪽 `[K,N]` weight를 모든 앞쪽 위치가 공유하는 위 projection 형태만 지원한다. `[B,H,T,D] @ [B,H,D,T]`처럼 오른쪽도 batched인 attention matmul과 batch-prefix broadcasting은 아직 CPU 전용이다.

`matmul(left, right, {compute_dtype: "float32"|"float16"|"bfloat16"})`만 compute dtype를 명시적으로 override할 수 있다. `linear(input, weight, [bias])`에는 options 인수가 없으므로 resident CUDA에서는 호출 시점의 현재 `autograd.autocast()` 상태를 사용한다. 선택된 dtype는 graph에 저장되어 matching backward projection에도 유지된다.

### 임의 축 transpose

인수를 하나만 주면 마지막 두 축을 바꾼다. 축 두 개를 함께 주면 해당 축을 바꾸며 음수 축도 허용한다.

```sura
head_major is autograd.transpose(heads, 1, 2)
# [B, T, H, Dh] -> [B, H, T, Dh]

last_two_swapped is autograd.transpose(head_major)
```

축은 둘 다 지정하거나 둘 다 생략해야 한다.

resident CUDA `float32`도 rank 2–8에서 임의 두 축 transpose와 inverse backward를 지원한다. shape/stride는 kernel parameter로 전달되므로 metadata H2D 없이 `[B,T,H,Dh] ↔ [B,H,T,Dh]` multi-head layout을 device에서 바꾼다. `cuda_stats().transpose_launches`로 forward/backward 실행을 구분할 수 있다.

### Exact GELU와 LayerNorm

`gelu`는 `erf` 기반 exact GELU다. `layer_norm`은 각 마지막 축 행을 독립적으로 정규화한다.

```sura
norm_weight is autograd.ones([4], true)
norm_bias is autograd.zeros([4], true)

normalized is autograd.layer_norm(hidden, norm_weight, norm_bias)
activated is autograd.gelu(projected)
```

`layer_norm(tensor, weight, bias, epsilon)`에서 기본 epsilon은 `0.00001`이다. affine 변환이 필요 없으면 weight와 bias를 생략할 수 있고, bias만 생략하면서 epsilon을 지정하려면 `nil`을 사용한다.

```sura
normalized is autograd.layer_norm(hidden, norm_weight, nil, 0.00001)
```

GELU와 LayerNorm은 CPU와 resident CUDA `float32`에서 forward/backward를 지원한다. CUDA GELU도 `x * Phi(x)` exact-form semantics를 따르며 tanh approximation이 아니다. CUDA LayerNorm은 마지막 축의 각 행을 독립적으로 정규화하고 optional `[D]` weight/bias, input gradient와 필요한 affine gradient를 지원한다. gradient가 필요한 output은 행별 `float32` mean/rstd를 device에 저장해 backward에서 재사용하며, `retain_graph`가 아니면 성공한 backward 뒤 해제한다. gradient가 필요 없는 forward는 saved row buffer를 만들지 않는다. output, saved state와 gradient는 명시적으로 관찰하기 전까지 device에 남는다.

현재 LayerNorm kernel은 정확성과 결정성을 위한 reference path다. forward와 input backward에서는 CUDA thread 하나가 행 하나를 순회하고, affine parameter backward에서는 thread 하나가 feature 하나의 행들을 합산한다. 큰 `float32` 입력에서도 통계가 무너지지 않도록 이 accumulation을 `float64`로 수행한 뒤 public `float32` storage로 기록한다. 기능 지원과 warp-level 성능 최적화는 별개이며, warp/block-parallel Welford kernel은 아직 구현해야 한다.

### Embedding

`embedding(token_ids, weight)`의 weight는 `[V,D]`여야 한다. token ID는 `0` 이상 `V-1` 이하의 정수여야 하며 gradient를 요구할 수 없다. `token_ids`는 숫자 배열이나 non-gradient CPU Tensor로 전달할 수 있고, weight는 CPU 또는 resident CUDA `float32` Tensor일 수 있다.

```sura
input_ids is [[0, 1, 2], [1, 2, 3]]
embedding_weight is autograd.randn([4, 8], {std: 0.02, seed: 3, requires_grad: true})
hidden is autograd.embedding(input_ids, embedding_weight)
# shape: [2, 3, 8]
```

같은 token ID가 여러 번 등장하면 backward는 해당 embedding 행에 모든 gradient를 합산한다. CUDA v1은 dimension별 thread가 token을 입력 순서대로 순회하므로 atomics나 race 없이 반복 ID gradient가 결정적이다.

CUDA weight를 사용하면 ID shape·정수 여부·vocabulary 범위를 먼저 host에서 검증하고 token마다 4-byte raw `uint32`로 매 forward 한 번 H2D한다. output이 packed device IDs를 소유해 backward에서 재업로드 없이 사용한다. gradient graph에서는 retained backward 동안 유지되고 성공한 non-retained backward 뒤 해제되며, frozen weight의 no-grad output에서는 output 수명과 함께 유지된다. CUDA token Tensor 입력은 아직 지원하지 않으므로 데이터 loader가 반환한 CPU ID Tensor를 GPU로 옮기지 말고 그대로 전달한다. ID cache/prefetch와 고성능 sparse/segmented backward는 후속 성능 과제다.

### Low-memory forward와 no-O(T²)-workspace CUDA causal attention backward

`causal_attention(q, k, v, [options])`은 마지막 두 축을 `[T,D]`로 해석한다. q, k, v는 같은 rank와 같은 앞쪽 축 및 sequence 길이를 가져야 하고, q와 k의 마지막 차원은 같아야 한다. v의 마지막 차원은 달라도 된다.

```sura
context is autograd.causal_attention(query, key, value, {precision: "auto"})
```

기본 scale은 `1 / sqrt(D)`다. 수치 검증처럼 직접 scale을 정할 때만 `scale` 옵션을 사용한다. `precision`은 `"auto"`, `"fast"`, `"strict"` 중 하나이며 기본값은 `"auto"`다. `auto`는 최적화 경로와 fallback을 자동 선택하고, `fast`는 resident CUDA `T >= 8` fused 경로를 요구해 사용할 수 없으면 오류를 내며, `strict`는 f64 reference를 고정한다.

```sura
context is autograd.causal_attention(query, key, value, {scale: 1, precision: "strict"})
```

rank 4의 `[B,H,T,Dh]` 입력을 그대로 받을 수 있다. 각 batch와 head는 독립적으로 계산되며 미래 token은 attention에서 제외된다.

forward는 안정적인 online-softmax recurrence를 사용해 전체 `[T,T]` score/probability 행렬 대신 출력 행 accumulator만 유지한다. resident CUDA q/k/v는 반드시 모두 같은 장치의 `float32`여야 하며 output과 q/k/v gradient도 device에 남는다. 기본 `precision: "auto"`와 `T >= 8`에서는 한 warp가 한 행의 q·k reduction과 value update를 협력하는 f32 kernel 1회가 실행된다. backward는 저장된 행별 max·reciprocal sum으로 probability를 다시 계산한다. dQ kernel 1회와 결합 dK/dV kernel 1회가 각 출력 요소를 단일 warp에 배정하므로 atomic 없이 결정적이다. full q/k/v graph는 총 3개 attention kernel이다. 선택된 plan은 graph에 저장되어 환경변수가 나중에 바뀌어도 matching backward를 유지한다. `T < 8`, `precision: "strict"`, 병렬 비활성화 시에는 reference forward/backward를 사용한다. strict mode의 score/reduction은 `float64`이며 `3×10^38` value의 평균과 f32 dot-product 범위를 넘는 절대 score도 별도 회귀 테스트로 검증한다.

기본 fused plan은 flattened batch/head 수를 `B`라고 할 때 행별 max와 reciprocal sum만 저장하므로 attention 보조 상태가 O(B·T)이고 `[T,T]` probability/dScore workspace가 없다. `SURA_CUDA_ATTENTION_FUSED=0`이면 legacy packed backward를 선택할 수 있다. 이 fallback만 `B * T * (T + 1) / 2`개의 `float32` buffer, 즉 `4 * pairs` bytes를 사용하며 한도를 넘으면 serial reference로 내려간다. 어느 경로도 Q/K/V tile과 softmax를 shared memory에서 융합하거나 Tensor Core를 사용하는 FlashAttention kernel은 아니다.

### Sparse token-ID cross entropy

`cross_entropy_ids(logits, class_ids)`는 one-hot target을 만들지 않는다.

```sura
logits is autograd.randn([2, 3, 4], {seed: 4, requires_grad: true})
targets is [[1, 2, 3], [2, 3, 0]]
loss is autograd.cross_entropy_ids(logits, targets)
```

logits shape가 `[B,T,V]`이면 target shape는 `[B,T]`여야 한다. 손실은 모든 batch/sequence 위치의 평균이다. 현재 `ignore_index`와 label smoothing 옵션은 없다.

CPU와 resident CUDA `float32` logits을 모두 지원한다. target은 숫자/직사각형 배열 또는 gradient를 요구하지 않는 CPU Tensor여야 하며, shape·정수 여부·`0 <= id < V` 범위를 host에서 검증한다. CUDA target Tensor는 검증을 위한 암묵적 D2H를 하지 않고 명시적으로 거부하므로 loader의 `batch.target_ids`를 GPU로 옮기지 말고 그대로 전달한다.

CUDA forward는 검증한 target을 위치마다 4-byte raw `uint32`로 pack해 정확히 한 번 H2D한다. 첫 kernel은 행마다 한 thread를 배치해 max-subtracted exponential sum을 `float64`로 누적하고 `float32` max/inv-sum을 저장한다. 다음 scalar-loss kernel은 한 GPU thread가 행을 고정 순서로 순회해 NLL을 `float64`로 누적한 뒤 결정적인 평균 `float32` loss를 기록한다. 이 때문에 큰 양·음수 logits에서도 raw exponential overflow/underflow를 피한다.

backward는 각 logit 원소를 병렬 처리하며 saved max·inv-sum·raw-u32 IDs와 scalar upstream을 재사용해 `(softmax - one_hot) * upstream / rows`를 계산한다. 여기서 `[B,T,V]` logits의 `rows`는 `B*T`다. ID 재업로드나 loss/gradient의 암묵적 D2H는 없다. scalar loss output이 세 saved allocation을 소유한다. `retain_graph=true`이면 다음 backward를 위해 유지되고 성공한 non-retained backward 뒤 해제되며, gradient가 필요 없는 output에서는 output이 수거될 때 함께 해제된다. `cuda_stats().cross_entropy_launches`는 row-statistics와 scalar-loss forward에서 2회, logits backward까지 수행하면 합계 3회 증가한다.

별도 `softmax`와 dense/one-hot `cross_entropy`는 여전히 CPU 전용이다. CUDA sparse CE v1도 `ignore_index`, label smoothing, CUDA-resident target, vocabulary-parallel reduction이나 fused language-head kernel은 제공하지 않는다.

## Tokenizer와 대용량 text shard

native byte tokenizer는 UTF-8 raw byte를 ID `0..255`로 바꾼다. `tokenizer.train_bpe`는 bounded corpus에서 byte-level merge vocabulary를 결정적으로 학습하고 optional BOS/EOS/PAD를 지원한다. seek dataset packer는 byte와 BPE tokenizer를 모두 받고 BPE file/text chunk 경계를 보존한다.

```sura
use tokenizer
use dataset

tok is tokenizer.byte()
dataset.pack_text(["train-000.txt", "train-001.txt"], tok, "train.suradata", {input: "files", chunk_bytes: 1048576})
loader is dataset.open("train.suradata", {batch_size: 8, sequence_length: 128, stride: 128, shuffle: true, seed: 42})
batch is dataset.next(loader)
```

`dataset.pack_text`는 파일을 chunk 단위로 읽어 versioned uint32 shard를 만든다. `dataset.open`은 shard checksum과 구조를 검사하고, `dataset.next`는 sample 위치로 seek해 CPU `batch.input_ids`/`batch.target_ids` `float32` Tensor를 반환한다. `batch.input_ids`는 CUDA weight embedding에, `batch.target_ids`는 resident CUDA logits의 sparse CE에 그대로 전달할 수 있으며 host validation 뒤 각각 raw `uint32`로 upload된다. corpus 전체를 Sura 배열로 올리지는 않지만 mmap, worker prefetch, pinned memory는 사용하지 않는다. `examples/tokenizer_dataset_training.sura`는 embedding과 sparse CE까지 연결하는 실행 예제다.

## Multi-head 분할과 병합

projection 결과 `[B,T,D]`를 `[B,H,T,Dh]`로 바꿀 때 `reshape`와 임의 축 `transpose`를 함께 쓴다.

```sura
func split_heads(x, batch_size, sequence_length, head_count, head_dim) do
  shaped is autograd.reshape(x, [batch_size, sequence_length, head_count, head_dim])
  return autograd.transpose(shaped, 1, 2)
end

func merge_heads(x, batch_size, sequence_length, model_dim) do
  sequence_major is autograd.transpose(x, 1, 2)
  return autograd.reshape(sequence_major, [batch_size, sequence_length, model_dim])
end
```

사용 예:

```sura
query is split_heads(query_projection, B, T, H, Dh)
key is split_heads(key_projection, B, T, H, Dh)
value is split_heads(value_projection, B, T, H, Dh)

head_context is autograd.causal_attention(query, key, value)
context is merge_heads(head_context, B, T, D)
```

`reshape`와 `transpose` 모두 backward 경로를 기록하므로 projection weight까지 gradient가 전달된다.

## 한 블록 Transformer 문법

다음 함수는 `tests/19_tiny_transformer_training.sura`와 같은 pre-norm causal 블록 구조다. 예제의 weight와 bias는 바깥에서 만든 trainable leaf Tensor라고 가정한다.

```sura
func tiny_transformer(input_ids) do
  embedded is autograd.embedding(input_ids, token_embedding)

  normalized1 is autograd.layer_norm(embedded, norm1_weight, norm1_bias)
  query is autograd.matmul(normalized1, query_weight)
  key is autograd.matmul(normalized1, key_weight)
  value is autograd.matmul(normalized1, value_weight)

  context is autograd.causal_attention(query, key, value)
  attention_output is autograd.matmul(context, attention_output_weight)
  residual1 is autograd.add(embedded, attention_output)

  normalized2 is autograd.layer_norm(residual1, norm2_weight, norm2_bias)
  feedforward_hidden is autograd.gelu(autograd.linear(normalized2, feedforward_in_weight, feedforward_in_bias))
  feedforward_output is autograd.linear(feedforward_hidden, feedforward_out_weight, feedforward_out_bias)
  residual2 is autograd.add(residual1, feedforward_output)

  return autograd.linear(residual2, language_head_weight, language_head_bias)
end
```

학습 반복은 다른 `autograd` 모델과 같다.

```sura
step is 0
while step < 120 do
  autograd.zero_grad(parameters)
  logits is tiny_transformer(tokens)
  loss is autograd.cross_entropy_ids(logits, targets)
  autograd.backward(loss)
  autograd.clip_grad_norm(parameters, 1)
  autograd.adam(parameters, 0.03)
  step is step + 1
end
```

`parameters`에는 embedding, q/k/v projection, attention output, 두 LayerNorm의 affine parameter, feed-forward, language head의 모든 trainable Tensor를 넣어야 한다.

## 제한 확인과 checkpoint

`autograd.limits()`는 기본 CPU compute/gradient dtype, 지원 storage dtype, CUDA 가용성, rank, element, graph, memory 한도와 추적 중인 memory 사용량을 반환한다. CUDA attention dispatch는 `cuda_attention_parallel_min_sequence`, `cuda_attention_workspace_limit_bytes`, `cuda_attention_parallel`, `cuda_attention_fused`로 확인한다.

`autograd.grad_info(tensor)`는 gradient payload를 복사하지 않고 `present`, 실제 gradient `dtype`/`device`, `elements`, `storage_bytes`, loss `scale`, `scaled`, `leaf`, `requires_grad`, `optimizer_ready`를 반환한다. CUDA gradient는 f32 device allocation으로, CPU gradient는 Tensor storage dtype와 무관한 f64 allocation으로 보고되므로 큰 Transformer parameter의 gradient byte 수와 unscale 상태를 payload D2H 없이 점검할 수 있다. `optimizer_ready`는 leaf·`requires_grad`·scale 0/1이라는 기본 자격이며 optimizer의 전체 검증 결과는 아니다.

```sura
limits is autograd.limits()
print(limits.max_elements)
print(limits.max_attention_scores)
print(limits.cuda_attention_workspace_limit_bytes)
print(limits.cuda_attention_parallel)
print(limits.cuda_attention_fused)
print(limits.memory_used_bytes)
```

실행 전에 `SURA_TENSOR_MAX_ELEMENTS`로 Tensor당 한도를 최대 1,000,000,000 elements까지, `SURA_TENSOR_MEMORY_LIMIT_MB`로 memory 한도를 최대 65,536 MiB까지, `SURA_ATTENTION_MAX_SCORES`로 attention score 한도를 최대 5,000,000,000개까지 확장할 수 있다. 기본값은 각각 10,000,000, 512 MiB, 50,000,000 scores다. `SURA_CUDA_ATTENTION_FUSED=0|false|off`는 no-O(T²)-workspace fused 경로를 끄고 legacy packed 경로를 선택한다. legacy workspace는 기본 64 MiB이며 `SURA_CUDA_ATTENTION_WORKSPACE_MB`로 최대 4096 MiB까지 바꿀 수 있다. `SURA_CUDA_ATTENTION_PARALLEL=0|false|off`는 fused와 packed 경로를 모두 비활성화해 serial reference forward/backward를 사용한다. 이 설정은 O(T²) 계산량 자체를 없애지 않는다.

resident 경로에서는 input과 weight를 `{device: "cuda"}`로 직접 만들고 `autograd.device()`로 placement를 확인한다. 이 Tensor들의 shared-right projection은 별도 backend option 없이 CUDA forward/backward를 사용하고 output도 device에 남는다. CPU `float32` Tensor에 `{backend: "cuda"}`를 주는 기존 one-shot rank-2 matmul은 매 호출마다 H2D/D2H를 수행하고 CPU graph를 반환하는 호환 경로일 뿐이다. 프로세스를 시작하기 전에 `SURA_CUDA_DEVICE`를 zero-based index로 설정하면 사용할 장치를 선택할 수 있다.

학습 상태는 이름별 Tensor 딕셔너리로 저장한다.

```sura
state is {token_embedding: token_embedding, query_weight: query_weight, key_weight: key_weight, value_weight: value_weight, language_head_weight: language_head_weight}

bytes is autograd.save_checkpoint(state, "tiny-transformer.surackpt")
loaded is autograd.load_checkpoint("tiny-transformer.surackpt")
```

binary checkpoint v3는 SHA-256과 함께 CUDA visible weight, f32 master, SGD velocity, Adam m/v·step·beta product를 보존한다. exact CUDA resume는 `{optimizer: true, device: "cuda"}`로 로드하며 결과는 trainable leaf다. 기본 CPU target은 CUDA optimizer state 손실을 막기 위해 거부되고, `{optimizer: false}`는 visible weight만 복원한다. gradient와 계산 graph는 저장하지 않으며 v1/v2 파일도 읽는다.

외부 framework와 weight만 교환할 때는 `save_safetensors`/`load_safetensors`를 우선 사용한다. PyTorch `.pt`/`.pth`는 `tools/sura_torch_bridge.py`가 Safetensors로 변환한다. `save_onnx_weights`/`load_onnx_weights`는 ONNX initializer를 다룬다. 별도의 `autograd.run_onnx(path, inputs)`는 CPU에서 IR 3~10, opset 7~18의 제한된 topological graph를 실행하며 지원 연산은 `Identity`, 기본 산술, `Neg`, `MatMul`, `Relu`, `Tanh`, `Sigmoid`, `Gemm`, `Transpose`, `Flatten`, 제한된 `Reshape`, 마지막 축 `Softmax`다. `Reshape` shape는 raw-data INT64 rank-1 initializer로 제한한다. Conv, 동적 shape, control flow, 외부 data와 임의 ONNX graph는 지원하지 않는다.

## Process-per-device 실험과 gradient 동기화

여러 프로세스를 각기 다른 GPU에 연결하려면 프로세스별로 `SURA_CUDA_DEVICE`를 다르게 설정한다. 현재 resident kernel subset은 f32 shared-right projection, host-ID 기반 embedding, host-target sparse CE, exact-form GELU와 last-axis LayerNorm forward/backward, 제한된 elementwise/bias/ReLU/reduction과 SGD/Adam까지 포함하지만 GPU collective와 Transformer 전체 kernel이 없으므로 이것만으로 multi-GPU Transformer 학습이 되지는 않는다.

CPU 학습 graph에서는 각 rank가 CPU backward를 마친 뒤 같은 parameter 순서로 공유 파일시스템 all-reduce를 호출할 수 있다. Resident CUDA gradient는 이 backend가 받지 않으므로 `to()`가 자동 분산 경계를 만들지 않는다.

```sura
use autograd

report is autograd.all_reduce_gradients(parameters, {rendezvous: "shared/gradients", run_id: "transformer-run", step: step, rank: rank, world_size: world_size, average: true, timeout_ms: 30000})
```

이 API는 checksum이 있는 CPU `float64` gradient 파일을 기다렸다가 검증하고 평균 또는 합계를 적용한다. NCCL, GPU-resident all-reduce, network launcher가 아니므로 shared storage의 성능과 `run_id`/`step` lifecycle을 직접 관리해야 한다.

## 테스트가 보장하는 것

### `tests/16_transformer_ops.sura`

- `reshape`의 row-major 순서와 원래 shape로 돌아가는 gradient
- rank 3 임의 축 `transpose`
- rank 3/4 batched matmul 및 broadcast된 입력 gradient 축약
- exact GELU의 forward/backward 기준값
- affine LayerNorm의 입력·weight·bias gradient
- 반복 token ID의 embedding scatter-add
- rank 3/4 causal attention의 미래 mask와 q/k/v gradient
- `[B,T,V]` logits에 대한 sparse token-ID cross entropy

### `tests/17_transformer_gradcheck.sura`

LayerNorm의 입력·weight·bias, causal attention의 q/k/v, sparse CE의 logits, exact GELU의 analytic gradient를 중앙 유한차분 결과와 비교한다. 허용 오차는 테스트에서 `0.00002`다.

### `tests/18_transformer_checkpoint.sura`

동적 Tensor/attention 한도 메타데이터, Adam·momentum-SGD 상태의 연속성, optimizer를 제외한 저장·복원, `requires_grad` 보존, SHA-256 payload 손상 거부를 검증한다.

### `tests/19_tiny_transformer_training.sura`

vocabulary 4, sequence 길이 3, model dimension 2인 결정적 한 블록 모델을 120번 Adam으로 학습한다. 최종 손실이 `0.01`보다 작고 여섯 위치의 다음-token 예측이 목표와 일치하는지 확인한다. 이 테스트는 end-to-end 학습 경로를 검증하지만, 모델 크기와 데이터가 매우 작으므로 일반 언어 성능을 의미하지 않는다.

### Sura 1.8 backend 검증

- `tests/20_safetensors.sura`: 네 가지 dtype의 표준 Safetensors round-trip
- `tests/21_dtype_storage.sura`: packed storage, promotion, gradient와 checkpoint
- `tests/22_cuda_backend.sura`: 실제 CUDA rank-2 f32 matmul, CPU parity, 오류 경계
- `tests/23_tokenizer_dataset.sura`: UTF-8 byte tokenizer, seek loader, rank partition
- `tests/70_bpe_tokenizer.sura`: deterministic BPE 학습, UTF-8 lossless 왕복, 저장·손상·위조 검증
- `tools/sura_distributed_autograd_smoke.ps1`: 여러 프로세스의 shared-filesystem all-reduce
- `tests/25_onnx_weights.sura`: ONNX typed initializer weights-only round-trip
- `tests/71_onnx_execution.sura`: bounded CPU ONNX 연산, backward와 잘못된 model/input 거부
- `tests/26_cuda_device_placement.sura`: device 생성/이동과 unsupported-op fallback 거부
- `tests/27_cuda_residency.sura`: resident forward chain과 명시적 host 관찰 transfer
- `tests/28_cuda_backward.sura`: resident matmul/ReLU backward, persistent gradient와 SGD
- `tests/29_cuda_linear_bias.sura`: `[features]` bias forward와 column-reduction backward
- `tests/30_cuda_adam.sura`: CPU/CUDA Adam parity, 4-byte status와 transaction rollback
- `tests/31_cuda_adam_checkpoint.sura`, `tests/51_cuda_sgd_checkpoint.sura`: CUDA f16/bf16/f32 master·moment·velocity checkpoint v3와 exact continuation
- `tests/32_cuda_lazy_host_storage.sura`: CUDA output의 lazy host allocation과 단일 materialization
- `tests/33_cuda_nd_linear.sura`: `[B,T,D] @ [D,F] + [F]` resident forward/backward와 CPU parity
- `tests/34_cuda_gelu.sura`: resident exact-form GELU forward/backward, tail 안정성과 CPU parity
- `tests/35_cuda_layer_norm.sura`: resident affine/no-affine last-axis LayerNorm, saved state, large-f32 안정성과 CPU parity
- `tests/38_cuda_embedding.sura`: resident embedding gather, raw-u32 ID upload 1회와 deterministic duplicate-ID backward
- `tests/39_cuda_cross_entropy_ids.sura`: CPU target validation과 CUDA target 거부, rank-3 loss/logits-gradient CPU parity, raw-u32 upload 1회, implicit/explicit scalar seed, extreme-logit 안정성과 host-lazy transfer
- `tests/40_cuda_causal_attention.sura`, `tests/41_cuda_causal_attention_graph.sura`, `tests/41_cuda_causal_attention_extreme.sura`: resident causal attention의 rank-3/4 parity, graph/alias/retain/frozen 경계, causality와 극단 f32 안정성
- `tests/44_cuda_attention_parallel.sura`: no-O(T²)-workspace fused backward의 CPU parity, 반복 결정성, frozen role, retain-graph 누적, 메모리/launch counter와 legacy packed·serial fallback 계약
- `tests/45_cuda_attention_warp_forward.sura`: fast f32 warp online-softmax forward, extreme score, no-grad allocation, 결정성과 matching fused backward
- `tests/46_cuda_attention_warp_edges.sura`: T=8/9/17과 D/Dv=1/31/32/33/65의 rank-2/4 warp/lane tail 및 기본 3-launch 계약
- `tests/52_cuda_attention_precision.sura`: invalid/CPU·short·env-disabled fast fail-closed, auto fused·legacy·reference dispatch, 양방향 sticky plan, strict f64 parity, q=k=v alias, scaled backward와 retain/release memory 계약
- `tests/47_cuda_mixed_matmul.sura`: f16/bf16 projection compute와 matching backward, f32 storage/gradient
- `tests/48_cuda_autocast.sura`: 중첩 autocast 복원, linear의 현재 autocast 적용, matmul explicit override와 sticky graph plan
- `tests/49_cuda_loss_scaling.sura`: scaled gradient 불변식, payload 무전송 `grad_info`, empty-gradient no-op, transactional unscale와 found-inf rollback
- `tests/42_cuda_transpose.sura`: multi-head axis transpose/inverse backward와 무전송·lazy-host 계약
- `tests/43_cuda_multihead_training.sura`: 두 head를 split/merge하는 end-to-end CUDA attention 언어모델의 60-step Adam 손실 감소
- `tools/sura_ai_benchmark.ps1`: 동일 Safetensors 입력과 동일 하드웨어의 공개 one-shot matmul 측정
- `tools/sura_ai_resident_benchmark.ps1`: resident forward/training step, transfer counter와 PyTorch 비교
- `tools/sura_mixed_compute_benchmark.ps1`: f32/f16/bf16 projection compute 비교; 유휴 GPU의 `performance_valid: true` 실행만 성능 수치로 사용
- `tools/sura_attention_benchmark.ps1`: projection을 제외한 직접 causal-attention forward+backward 비교

## 다음 규모로 넘어가기 전에

한도를 확장해도 parameter, gradient/optimizer 상태, activation과 O(T²) attention 비용은 빠르게 커진다. low visible weight는 `2N`이지만 f32 master·gradient·moments 때문에 Adam steady state는 low `18N`, f32 `16N`이다. output/activation도 f32다. host는 `autograd.limits()`, device는 `autograd.cuda_stats()`로 실제 값과 peak를 측정해야 한다.

다음 필수 단계는 low activation/output, mixed backward 가속, CUDA token/target Tensor, dense softmax/cross-entropy, batched-right matmul, shared-memory tiled/Tensor Core·저정밀 FlashAttention, warp-parallel LayerNorm, AdamW·gradient norm, dynamic GradScaler metadata, KV cache, 외부 tokenizer 호환, mmap/prefetch와 NCCL이다. 현재 no-O(T²)-workspace fused recomputation, 실제 2-byte weight storage와 checkpoint v3는 기반일 뿐 PyTorch 생태계보다 우월하거나 ChatGPT급 AI가 완성됐다는 뜻은 아니다.
