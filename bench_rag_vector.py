import time


def query_component(dim):
    return ((dim * 37 + 11) % 101) - 50


def doc_component(doc, dim):
    return (((doc + 3) * (dim * 17 + 5) + doc * doc + dim * dim * 7) % 113) - 56


def score_doc(doc, dims):
    dot = 0
    doc_norm = 0
    query_norm = 0
    dim = 0
    while dim < dims:
        q = query_component(dim)
        v = doc_component(doc, dim)
        dot += q * v
        doc_norm += v * v
        query_norm += q * q
        dim += 1
    return dot * 100000 / (1 + doc_norm + query_norm)


runs = 5
docs = 12_000
dims = 16
total_ms = 0.0
checksum = 0

for _ in range(runs):
    start = time.perf_counter()
    doc = 0
    best_score = -999_999_999
    best_doc = -1
    top_hits = 0
    total_score = 0
    while doc < docs:
        score = score_doc(doc, dims)
        if score > best_score:
            best_score = score
            best_doc = doc
        if score > 3500:
            top_hits += 1
        total_score += score
        doc += 1
    total_ms += (time.perf_counter() - start) * 1000
    checksum = best_doc + top_hits * 17 + best_score + total_score

avg = total_ms / runs
print(f"rag vector docs: {docs}")
print(f"checksum: {checksum}")
print(f"avg ({runs} runs): {avg:.2f} ms")
