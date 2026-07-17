param(
    [string]$Engine = ".\SuraLanguage.exe"
)

$ErrorActionPreference = "Stop"
if (Get-Variable -Name PSNativeCommandUseErrorActionPreference -ErrorAction SilentlyContinue) {
    $PSNativeCommandUseErrorActionPreference = $false
}

$EnginePath = (Resolve-Path -LiteralPath $Engine).Path
$temp = Join-Path ([System.IO.Path]::GetTempPath()) ("sura_autograd_" + [System.Guid]::NewGuid().ToString("N"))
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)

function Run-SuraCase {
    param([string]$Name, [string]$Source, [string]$Expected)
    $script = Join-Path $temp ($Name + ".sura")
    [System.IO.File]::WriteAllText($script, ($Source.Trim() + "`n"), $utf8NoBom)
    $invocations = @(
        @{ Label = "vm"; Args = @($script) },
        @{ Label = "jit"; Args = @("--jit", $script) }
    )
    foreach ($invocation in $invocations) {
        $old = $ErrorActionPreference
        $ErrorActionPreference = "Continue"
        try {
            [string[]]$engineArgs = $invocation.Args
            $output = & $EnginePath @engineArgs 2>&1 | ForEach-Object { "$_" }
            $code = $LASTEXITCODE
        } finally {
            $ErrorActionPreference = $old
        }
        $text = $output -join "`n"
        if ($code -eq 0 -or $text -notmatch $Expected) {
            Write-Output $text
            throw "$Name/$($invocation.Label) should fail with /$Expected/"
        }
    }
}

try {
    New-Item -ItemType Directory -Force -Path $temp | Out-Null

    Run-SuraCase "ragged_data" @'
use autograd
autograd.tensor([[1, 2], [3]])
'@ "ragged arrays"

    Run-SuraCase "bad_broadcast" @'
use autograd
a is autograd.tensor([[1, 2], [3, 4]])
b is autograd.tensor([1, 2, 3])
autograd.add(a, b)
'@ "cannot broadcast"

    Run-SuraCase "bad_matmul_shape" @'
use autograd
a is autograd.tensor([[1, 2]])
b is autograd.tensor([[3, 4]])
autograd.matmul(a, b)
'@ "inner dimensions do not match"

    Run-SuraCase "bad_matmul_rank" @'
use autograd
a is autograd.tensor([1, 2])
b is autograd.tensor([3, 4])
autograd.matmul(a, b)
'@ "rank 2 or greater"

    Run-SuraCase "bad_reshape_size" @'
use autograd
x is autograd.tensor([[1, 2], [3, 4]])
autograd.reshape(x, [3, 2])
'@ "requested shape does not match tensor size"

    Run-SuraCase "bad_transpose_axes" @'
use autograd
x is autograd.tensor([[[1, 2], [3, 4]]])
autograd.transpose(x, 1)
'@ "provide both axes or neither"

    Run-SuraCase "bad_embedding_id" @'
use autograd
weight is autograd.parameter([[1, 2], [3, 4]])
autograd.embedding([0, 2], weight)
'@ "token ids must be integers within the vocabulary"

    Run-SuraCase "bad_layer_norm_weight" @'
use autograd
x is autograd.tensor([[1, 2, 3]])
weight is autograd.parameter([1, 1])
autograd.layer_norm(x, weight)
'@ "weight shape must match the last input dimension"

    Run-SuraCase "bad_attention_shape" @'
use autograd
q is autograd.tensor([[[1, 2], [3, 4]]])
k is autograd.tensor([[[1], [2]]])
v is autograd.tensor([[[1, 2], [3, 4]]])
autograd.causal_attention(q, k, v)
'@ "incompatible q, k, and v shapes"

    Run-SuraCase "attention_work_limit" @'
use autograd
q is autograd.zeros([10000, 1])
k is autograd.zeros([10000, 1])
v is autograd.zeros([10000, 1])
autograd.causal_attention(q, k, v)
'@ "score safety limit"

    Run-SuraCase "bad_sparse_target_shape" @'
use autograd
logits is autograd.parameter([[[1, 2, 3], [3, 2, 1]]])
autograd.cross_entropy_ids(logits, [0, 1])
'@ "target shape must equal logits shape without the class dimension"

    Run-SuraCase "duplicate_checkpoint_tensor" @'
use autograd
p is autograd.parameter([1])
autograd.save_checkpoint({first: p, second: p}, "unused.surackpt")
'@ "same tensor appears under multiple names"

    Run-SuraCase "unknown_checkpoint_option" @'
use autograd
p is autograd.parameter([1])
autograd.save_checkpoint({p: p}, "unused.surackpt", {optimzer: true})
'@ "unknown option"

    Run-SuraCase "missing_backward_seed" @'
use autograd
x is autograd.parameter([1, 2])
y is autograd.mul(x, x)
autograd.backward(y)
'@ "non-scalar outputs require an explicit gradient"

    Run-SuraCase "bad_backward_seed_shape" @'
use autograd
x is autograd.parameter([1, 2])
y is autograd.mul(x, x)
autograd.backward(y, [[1, 1]])
'@ "supplied gradient shape does not match output"

    Run-SuraCase "freed_intermediate_reuse" @'
use autograd
x is autograd.parameter([2])
y is autograd.mul(x, x)
loss is autograd.sum(y)
autograd.backward(loss)
autograd.add(y, 1)
'@ "input graph was freed"

    Run-SuraCase "freed_graph" @'
use autograd
x is autograd.parameter([2])
loss is autograd.sum(autograd.mul(x, x))
autograd.backward(loss)
autograd.backward(loss)
'@ "graph was already freed"

    Run-SuraCase "stale_graph" @'
use autograd
x is autograd.parameter([2])
loss is autograd.sum(autograd.mul(x, x))
autograd.backward(loss, nil, true)
autograd.sgd([x], 0.1)
autograd.backward(loss, nil, true)
'@ "modified in-place after forward"

    Run-SuraCase "bad_item" @'
use autograd
x is autograd.tensor([1, 2])
autograd.item(x)
'@ "exactly one value"

    Run-SuraCase "non_leaf_requires_grad" @'
use autograd
x is autograd.parameter([2])
y is autograd.mul(x, x)
autograd.set_requires_grad(y, false)
'@ "only leaf tensors"

    Run-SuraCase "bad_bce_probability" @'
use autograd
p is autograd.parameter([1.5])
autograd.bce(p, [1])
'@ "probabilities and targets must be between 0 and 1"

    Run-SuraCase "non_leaf_optimizer" @'
use autograd
x is autograd.parameter([2])
y is autograd.mul(x, x)
loss is autograd.sum(y)
autograd.backward(loss)
autograd.sgd([y], 0.1)
'@ "optimizers require leaf tensors"

    Run-SuraCase "bad_cross_entropy_target" @'
use autograd
logits is autograd.parameter([[1, 2, 3]])
autograd.cross_entropy(logits, [[1, 1, 0]])
'@ "target row must sum to 1"

    Run-SuraCase "bad_optimizer_rate" @'
use autograd
x is autograd.parameter([2])
autograd.sgd([x], 0)
'@ "learning_rate must be positive"

    Run-SuraCase "duplicate_parameters" @'
use autograd
x is autograd.parameter([2])
autograd.zero_grad([x, x])
'@ "duplicate tensor"

    Run-SuraCase "nonfinite_tensor" @'
use autograd
autograd.tensor([to_float("nan")])
'@ "tensor data must contain finite numbers"

    Run-SuraCase "unknown_randn_option" @'
use autograd
autograd.randn([2], {sed: 1})
'@ "unknown option"

    Run-SuraCase "leaf_gradient_overflow" @'
use autograd
x is autograd.parameter(0)
huge is pow(10, 308)
autograd.backward(x, huge)
autograd.backward(x, huge)
'@ "accumulated leaf gradient is not finite"

    Run-SuraCase "sgd_update_overflow" @'
use autograd
x is autograd.parameter([1, 2])
huge is pow(10, 308)
autograd.backward(x, [huge, 1])
autograd.sgd(x, huge)
'@ "parameter update is not finite"

    Run-SuraCase "adam_gradient_overflow" @'
use autograd
x is autograd.parameter(1)
autograd.backward(x, pow(10, 200))
autograd.adam(x, 0.1)
'@ "gradient magnitude is too large"

    "autograd_smoke: PASS"
}
finally {
    if (Test-Path -LiteralPath $temp) {
        Remove-Item -LiteralPath $temp -Recurse -Force
    }
}
