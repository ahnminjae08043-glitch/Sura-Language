package main

import (
	"fmt"
	"os"
	"sort"
	"strconv"
	"strings"
	"time"
)

const REPS = 5

var sink float64
var sizes = []int{30, 3000000, 1000000, 200000, 200000, 300000, 500000, 256}

func loadSizes() {
	// Read through os.Args so the optimizer cannot treat these as constants
	// and fold an entire benchmark away.
	for i, a := range os.Args[1:] {
		if i < len(sizes) {
			if v, err := strconv.Atoi(a); err == nil {
				sizes[i] = v
			}
		}
	}
	if len(os.Args) < 2 {
		// Same values, but now they came from a slice the compiler cannot fold.
		sizes[len(sizes)-1] = sizes[len(sizes)-1] + len(os.Args) - 1
	}
}

func fib(n int) int {
	if n <= 1 {
		return n
	}
	return fib(n-1) + fib(n-2)
}
func numericLoop(n int) float64 {
	acc := 0.0
	i := 1
	for i <= n {
		acc += float64(i*3-1) / 2.0
		i++
	}
	return acc
}
func arrayWork(n int) float64 {
	a := make([]float64, 0, n)
	i := 0
	for i < n {
		a = append(a, float64(i)*0.5)
		i++
	}
	total := 0.0
	j := 0
	for j < n {
		total += a[j]
		j++
	}
	return total
}
func stringWork(n int) int {
	parts := make([]string, 0, n)
	i := 0
	for i < n {
		parts = append(parts, "item")
		i++
	}
	return len(strings.Join(parts, ","))
}
func dictWork(n int) int {
	counts := make(map[string]int)
	i := 0
	for i < n {
		k := "k" + strconv.Itoa(i%50000)
		counts[k]++
		i++
	}
	return len(counts)
}
func sortWork(n int) int64 {
	a := make([]int64, 0, n)
	var seed int64 = 12345
	i := 0
	for i < n {
		seed = (seed*1103515245 + 12345) % 2147483648
		a = append(a, seed%1000000)
		i++
	}
	sort.Slice(a, func(x, y int) bool { return a[x] < a[y] })
	return a[0] + a[n-1]
}

type Point struct{ x, y float64 }

func objectWork(n int) float64 {
	total := 0.0
	i := 0
	for i < n {
		p := &Point{float64(i), float64(i + 1)}
		p.x = p.x + 1
		total += p.x + p.y
		i++
	}
	return total
}
func matmulWork(n int) float64 {
	a := make([][]float64, n)
	b := make([][]float64, n)
	c := make([][]float64, n)
	for i := 0; i < n; i++ {
		a[i] = make([]float64, n)
		b[i] = make([]float64, n)
		c[i] = make([]float64, n)
		for j := 0; j < n; j++ {
			idx := i*n + j
			a[i][j] = float64(idx%7) * 0.5
			b[i][j] = float64(idx%5) * 0.25
		}
	}
	for r := 0; r < n; r++ {
		for k := 0; k < n; k++ {
			av := a[r][k]
			brow := b[k]
			crow := c[r]
			for q := 0; q < n; q++ {
				crow[q] += av * brow[q]
			}
		}
	}
	return c[0][0]
}
func measure(name string, fn func(int) float64, n int) {
	best := 1e18
	for i := 0; i < REPS; i++ {
		t0 := time.Now()
		sink += fn(n + i)
		dt := float64(time.Since(t0).Nanoseconds()) / 1e6
		if dt < best {
			best = dt
		}
	}
	fmt.Printf("%s=%v\n", name, best)
}
func main() {
	loadSizes()
	for w := 0; w < 60; w++ {
		fib(12)
		numericLoop(200)
		arrayWork(200)
		stringWork(200)
		dictWork(200)
		sortWork(200)
		objectWork(200)
		matmulWork(12)
	}
	measure("fib", func(n int) float64 { return float64(fib(n)) }, sizes[0])
	measure("numeric", func(n int) float64 { return numericLoop(n) }, sizes[1])
	measure("array", func(n int) float64 { return arrayWork(n) }, sizes[2])
	measure("string", func(n int) float64 { return float64(stringWork(n)) }, sizes[3])
	measure("dict", func(n int) float64 { return float64(dictWork(n)) }, sizes[4])
	measure("sort", func(n int) float64 { return float64(sortWork(n)) }, sizes[5])
	measure("object", func(n int) float64 { return objectWork(n) }, sizes[6])
	measure("matmul", func(n int) float64 { return matmulWork(n) }, sizes[7])
	fmt.Printf("checksum=%.3f\n", sink)
}
