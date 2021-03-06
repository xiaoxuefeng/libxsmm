/******************************************************************************
** Copyright (c) 2015-2017, Intel Corporation                                **
** All rights reserved.                                                      **
**                                                                           **
** Redistribution and use in source and binary forms, with or without        **
** modification, are permitted provided that the following conditions        **
** are met:                                                                  **
** 1. Redistributions of source code must retain the above copyright         **
**    notice, this list of conditions and the following disclaimer.          **
** 2. Redistributions in binary form must reproduce the above copyright      **
**    notice, this list of conditions and the following disclaimer in the    **
**    documentation and/or other materials provided with the distribution.   **
** 3. Neither the name of the copyright holder nor the names of its          **
**    contributors may be used to endorse or promote products derived        **
**    from this software without specific prior written permission.          **
**                                                                           **
** THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS       **
** "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT         **
** LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR     **
** A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT      **
** HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,    **
** SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED  **
** TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR    **
** PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF    **
** LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING      **
** NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS        **
** SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.              **
******************************************************************************/
/* Hans Pabst (Intel Corp.)
******************************************************************************/
#include <libxsmm.h>

#if defined(LIBXSMM_OFFLOAD_TARGET)
# pragma offload_attribute(push,target(LIBXSMM_OFFLOAD_TARGET))
#endif
#include <algorithm>
#include <stdexcept>
#include <cstdlib>
#include <cstring>
#include <cassert>
#include <cstdio>
#include <cmath>
#if defined(_OPENMP)
# include <omp.h>
#endif
#if defined(LIBXSMM_OFFLOAD_TARGET)
# pragma offload_attribute(pop)
#endif

#if !defined(REAL_TYPE)
# define REAL_TYPE double
#endif


LIBXSMM_INLINE LIBXSMM_RETARGETABLE void init(libxsmm_blasint seed, REAL_TYPE *LIBXSMM_RESTRICT dst,
  libxsmm_blasint nrows, libxsmm_blasint ncols, libxsmm_blasint ld, double scale)
{
  const double seed1 = scale * (seed + 1);
  libxsmm_blasint i;
  for (i = 0; i < ncols; ++i) {
    libxsmm_blasint j = 0;
    for (; j < nrows; ++j) {
      const libxsmm_blasint k = i * ld + j;
      dst[k] = (REAL_TYPE)(seed1 / (k + 1));
    }
    for (; j < ld; ++j) {
      const libxsmm_blasint k = i * ld + j;
      dst[k] = (REAL_TYPE)seed;
    }
  }
}


int main(int argc, char* argv[])
{
  int result = EXIT_SUCCESS;
  try {
    typedef REAL_TYPE T;
    const libxsmm_blasint benchmark = 1 < argc ? std::atoi(argv[1]) : 0;
    const libxsmm_blasint m = (2 < argc ? std::atoi(argv[2]) : 23);
    const libxsmm_blasint k = (4 < argc ? std::atoi(argv[4]) : m);
    const libxsmm_blasint n = (3 < argc ? std::atoi(argv[3]) : k);

    const libxsmm_blasint lda = m, ldb = k, ldc = m;
    const char transa = 'N', transb = 'N';
    const int flags = LIBXSMM_GEMM_FLAGS(transa, transb);
    const T alpha = 1, beta = 1;

    const libxsmm_blasint asize = lda * k, bsize = ldb * n, csize = ldc * n, aspace = LIBXSMM_ALIGNMENT / sizeof(T);
    const libxsmm_blasint s = (2ULL << 30) / ((asize + bsize + csize) * sizeof(T)); // 2 GByte
    const size_t bwsize_batched = (size_t)((asize/*load*/ + bsize/*load*/ + 2 * csize/*RFO*/) * sizeof(T)); // batched (A, B, and C)
    const size_t bwsize = (size_t)((asize/*load*/ + bsize/*load*/) * sizeof(T)); // omit size of A, B, or C since it is held in cache
    const double gflops = 2.0 * s * m * n * k * 1E-9, scale = 1.0 / s;

    struct raii { // avoid std::vector (first-touch init. causes NUMA issue)
      T *a, *b, *c;
      raii(libxsmm_blasint asize_, libxsmm_blasint bsize_, libxsmm_blasint csize_)
        : a(new T[static_cast<size_t>(asize_)]), b(new T[static_cast<size_t>(bsize_)])
        , c(new T[static_cast<size_t>(csize_)]) {}
      ~raii() { delete[] a; delete[] b; delete[] c; }
    } buffer(s * asize + aspace - 1, s * bsize + aspace - 1, s * csize + aspace - 1);
    T *const a = LIBXSMM_ALIGN(buffer.a, LIBXSMM_ALIGNMENT);
    T *const b = LIBXSMM_ALIGN(buffer.b, LIBXSMM_ALIGNMENT);
    T *c = LIBXSMM_ALIGN(buffer.c, LIBXSMM_ALIGNMENT);

#if defined(_OPENMP)
#   pragma omp parallel for
#endif
    for (libxsmm_blasint i = 0; i < s; ++i) {
      init(42 + i, a + i * asize, m, k, lda, scale);
      init(24 + i, b + i * bsize, k, n, ldb, scale);
      init(22 + i, c + i * csize, m, n, ldc, scale);
    }

#if defined(LIBXSMM_OFFLOAD_TARGET)
#   pragma offload target(LIBXSMM_OFFLOAD_TARGET) in(a: length(s * asize)) in(b: length(s * bsize)) inout(c: length(s * csize))
#endif
    {
#if defined(MKL_ENABLE_AVX512)
      mkl_enable_instructions(MKL_ENABLE_AVX512);
#endif
      // initialize LIBXSMM
      libxsmm_init();

      fprintf(stdout, "m=%lli n=%lli k=%lli size=%lli memory=%.1f MB (%s)\n\n",
        static_cast<long long>(m), static_cast<long long>(n), static_cast<long long>(k), static_cast<long long>(s),
        1.0 * (s * (asize + bsize + csize) * sizeof(T)) / (1 << 20),
        8 == sizeof(T) ? "DP" : "SP");

      switch (benchmark) {
      case 0: { // batched
        fprintf(stdout, "Batched (A,B,C)...\n");
        const unsigned long long start = libxsmm_timer_tick();
#if defined(_OPENMP)
#       pragma omp parallel for
#endif
        for (libxsmm_blasint i = 0; i < s; ++i) {
          LIBXSMM_INLINE_GEMM(flags, m, n, k,
            alpha, a + i * asize, lda, b + i * bsize, ldb,
             beta, c + i * csize, ldc);
        }
        const unsigned long long end = libxsmm_timer_tick(), x = std::max(end, start) - start;
        const double duration = libxsmm_timer_duration(start, end);
        if (0 < duration && 0 != x) {
          fprintf(stdout, "\tpseudo-perf.: %.1f FLOPS/cycle\n", (s * (2.0 * m * n * k - m * n)) / x);
          fprintf(stdout, "\tperformance: %.1f GFLOPS/s\n", gflops / duration);
          fprintf(stdout, "\tbandwidth: %.1f GB/s\n", s * bwsize_batched / (duration * (1 << 30)));
        }
        fprintf(stdout, "\tduration: %.0f ms\n", 1000.0 * duration);
      } break;

      case 1: { // streaming A and C
        fprintf(stdout, "Streamed (A,C)...\n");
        const unsigned long long start = libxsmm_timer_tick();
#if defined(_OPENMP)
#       pragma omp parallel for
#endif
        for (libxsmm_blasint i = 0; i < s; ++i) {
          LIBXSMM_INLINE_GEMM(flags, m, n, k,
            alpha, a + i * asize, lda, b, ldb,
             beta, c + i * csize, ldc);
        }
        const unsigned long long end = libxsmm_timer_tick(), x = std::max(end, start) - start;
        const double duration = libxsmm_timer_duration(start, end);
        if (0 < duration && 0 != x) {
          fprintf(stdout, "\tpseudo-perf.: %.1f FLOPS/cycle\n", (s * (2.0 * m * n * k - m * n)) / x);
          fprintf(stdout, "\tperformance: %.1f GFLOPS/s\n", gflops / duration);
          fprintf(stdout, "\tbandwidth: %.1f GB/s\n", s * bwsize / (duration * (1 << 30)));
        }
        fprintf(stdout, "\tduration: %.0f ms\n", 1000.0 * duration);
      } break;

      case 2: { // streaming B and C
        fprintf(stdout, "Streamed (B,C)...\n");
        const unsigned long long start = libxsmm_timer_tick();
#if defined(_OPENMP)
#       pragma omp parallel for
#endif
        for (libxsmm_blasint i = 0; i < s; ++i) {
          LIBXSMM_INLINE_GEMM(flags, m, n, k,
            alpha, a, lda, b + i * bsize, ldb,
             beta, c + i * csize, ldc);
        }
        const unsigned long long end = libxsmm_timer_tick(), x = std::max(end, start) - start;
        const double duration = libxsmm_timer_duration(start, end);
        if (0 < duration && 0 != x) {
          fprintf(stdout, "\tpseudo-perf.: %.1f FLOPS/cycle\n", (s * (2.0 * m * n * k - m * n)) / x);
          fprintf(stdout, "\tperformance: %.1f GFLOPS/s\n", gflops / duration);
          fprintf(stdout, "\tbandwidth: %.1f GB/s\n", s * bwsize / (duration * (1 << 30)));
        }
        fprintf(stdout, "\tduration: %.0f ms\n", 1000.0 * duration);
      } break;

      case 3: { // streaming A and B
        fprintf(stdout, "Streamed (A,B)...\n");
        const unsigned long long start = libxsmm_timer_tick();
#if defined(_OPENMP)
#       pragma omp parallel for
#endif
        for (libxsmm_blasint i = 0; i < s; ++i) {
#if defined(_OPENMP) /* write to disjunct cachelines (even when unaligned) to measure in-cache performance (TLS would serve as well) */
          const libxsmm_blasint j = LIBXSMM_MIN(omp_get_thread_num() * (libxsmm_blasint)LIBXSMM_UP2(csize, 2 * LIBXSMM_CACHELINE / sizeof(T)), s - csize);
#else
          const libxsmm_blasint j = 0;
#endif
          LIBXSMM_INLINE_GEMM(flags, m, n, k,
            alpha, a + i * asize, lda, b + i * bsize, ldb,
             beta, c + j, ldc);
        }
        const unsigned long long end = libxsmm_timer_tick(), x = std::max(end, start) - start;
        const double duration = libxsmm_timer_duration(start, end);
        if (0 < duration && 0 != x) {
          fprintf(stdout, "\tpseudo-perf.: %.1f FLOPS/cycle\n", (s * (2.0 * m * n * k - m * n)) / x);
          fprintf(stdout, "\tperformance: %.1f GFLOPS/s\n", gflops / duration);
          fprintf(stdout, "\tbandwidth: %.1f GB/s\n", s * bwsize / (duration * (1 << 30)));
        }
        fprintf(stdout, "\tduration: %.0f ms\n", 1000.0 * duration);
      } break;

      case 4: { // cached
        fprintf(stdout, "Cached...\n");
        const unsigned long long start = libxsmm_timer_tick();
#if defined(_OPENMP)
#       pragma omp parallel for
#endif
        for (libxsmm_blasint i = 0; i < s; ++i) {
#if defined(_OPENMP) /* write to disjunct cachelines (even when unaligned) to measure in-cache performance (TLS would serve as well) */
          const libxsmm_blasint j = LIBXSMM_MIN(omp_get_thread_num() * (libxsmm_blasint)LIBXSMM_UP2(csize, 2 * LIBXSMM_CACHELINE / sizeof(T)), s - csize);
#else
          const libxsmm_blasint j = 0;
#endif
          LIBXSMM_INLINE_GEMM(flags, m, n, k,
            alpha, a, lda, b, ldb,
             beta, c + j, ldc);
        }
        const unsigned long long end = libxsmm_timer_tick(), x = std::max(end, start) - start;
        const double duration = libxsmm_timer_duration(start, end);
        if (0 < duration && 0 != x) {
          fprintf(stdout, "\tpseudo-perf.: %.1f FLOPS/cycle\n", (s * (2.0 * m * n * k - m * n)) / x);
          fprintf(stdout, "\tperformance: %.1f GFLOPS/s\n", gflops / duration);
        }
        fprintf(stdout, "\tduration: %.0f ms\n", 1000.0 * duration);
      } break;
      default: throw "invalid case selected!";
      } /*switch*/

      // finalize LIBXSMM
      libxsmm_finalize();
      fprintf(stdout, "Finished\n");
    }
  }
  catch(const std::exception& e) {
    fprintf(stderr, "Error: %s\n", e.what());
    result = EXIT_FAILURE;
  }
  catch(...) {
    fprintf(stderr, "Error: unknown exception caught!\n");
    result = EXIT_FAILURE;
  }

  return result;
}
