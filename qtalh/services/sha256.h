#pragma once
// SHA-256 (FIPS 180-4) for alarm log fingerprints and checkpoints. Results are
// identical to QCryptographicHash::Sha256, which Qt implements a byte at a time;
// whole blocks are hashed here, with x86 SHA extensions when the CPU has them.
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#if defined(_MSC_VER) && defined(_M_X64)
#include <immintrin.h>
#include <intrin.h>
#define QTALH_SHA_NI
#define QTALH_SHA_NI_TARGET
#elif (defined(__GNUC__) || defined(__clang__)) && defined(__x86_64__)
#include <cpuid.h>
#include <immintrin.h>
#define QTALH_SHA_NI
#define QTALH_SHA_NI_TARGET __attribute__((target("sha,sse4.1,ssse3")))
#endif

namespace alh {
class Sha256 {
public:
  using Digest = std::array<char, 32>;
  // Detected once; tests may disable it to check the portable implementation.
  static bool& accelerated() {
    static bool enabled = hardware();
    return enabled;
  }
  void add(const char* data, std::size_t size) {
    total += size;
    if (used) {
      const auto take = std::min(size, sizeof(buffer) - used);
      std::memcpy(buffer + used, data, take);
      used += take;
      data += take;
      size -= take;
      if (used < sizeof(buffer)) return;
      compress(buffer);
      used = 0;
    }
    for (; size >= sizeof(buffer); data += sizeof(buffer), size -= sizeof(buffer))
      compress(reinterpret_cast<const unsigned char*>(data));
    std::memcpy(buffer, data, size);
    used = size;
  }
  Digest finish() {
    const std::uint64_t bits = total * 8;
    buffer[used++] = 0x80;
    if (used > 56) {
      std::memset(buffer + used, 0, sizeof(buffer) - used);
      compress(buffer);
      used = 0;
    }
    std::memset(buffer + used, 0, 56 - used);
    for (int i = 0; i < 8; ++i)
      buffer[56 + i] = static_cast<unsigned char>(bits >> (56 - 8 * i));
    compress(buffer);
    Digest digest;
    for (int i = 0; i < 8; ++i)
      for (int j = 0; j < 4; ++j)
        digest[4 * i + j] = static_cast<char>(state[i] >> (24 - 8 * j));
    *this = Sha256();
    return digest;
  }
  static Digest of(const char* data, std::size_t size) {
    Sha256 hash;
    hash.add(data, size);
    return hash.finish();
  }
  // Log fingerprints hash each record with its slot, as "<index>:<record>".
  void addIndex(unsigned long long index) {
    char prefix[24];
    add(prefix, std::size_t(std::snprintf(prefix, sizeof(prefix), "%llu:", index)));
  }
  static Digest record(unsigned long long index, const char* data, std::size_t size) {
    Sha256 hash;
    hash.addIndex(index);
    hash.add(data, size);
    return hash.finish();
  }

private:
  std::uint32_t state[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                            0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  unsigned char buffer[64];
  std::size_t used = 0;
  std::uint64_t total = 0;
  static constexpr std::uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
        0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
        0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
        0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
        0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
        0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
        0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
        0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
        0xc67178f2};
  static bool hardware() {
#if defined(QTALH_SHA_NI) && defined(_MSC_VER)
    int registers[4];
    __cpuid(registers, 0);
    if (registers[0] < 7) return false;
    __cpuidex(registers, 1, 0);
    const bool sse = (registers[2] & (1 << 9)) && (registers[2] & (1 << 19));
    __cpuidex(registers, 7, 0);
    return sse && (registers[1] & (1 << 29));
#elif defined(QTALH_SHA_NI)
    unsigned a = 0, b = 0, c = 0, d = 0;
    if (!__get_cpuid(1, &a, &b, &c, &d) || !(c & bit_SSSE3) || !(c & bit_SSE4_1)) return false;
    return __get_cpuid_count(7, 0, &a, &b, &c, &d) && (b & (1u << 29));
#else
    return false;
#endif
  }
  static std::uint32_t rotate(std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
  void compress(const unsigned char* block) {
#ifdef QTALH_SHA_NI
    if (accelerated()) {
      compressHardware(state, block);
      return;
    }
#endif
    std::uint32_t w[64];
    for (int i = 0; i < 16; ++i)
      w[i] = std::uint32_t(block[4 * i]) << 24 | std::uint32_t(block[4 * i + 1]) << 16 |
             std::uint32_t(block[4 * i + 2]) << 8 | std::uint32_t(block[4 * i + 3]);
    for (int i = 16; i < 64; ++i) {
      const auto s0 = rotate(w[i - 15], 7) ^ rotate(w[i - 15], 18) ^ (w[i - 15] >> 3);
      const auto s1 = rotate(w[i - 2], 17) ^ rotate(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    auto a = state[0], b = state[1], c = state[2], d = state[3];
    auto e = state[4], f = state[5], g = state[6], h = state[7];
    for (int i = 0; i < 64; ++i) {
      const auto t1 = h + (rotate(e, 6) ^ rotate(e, 11) ^ rotate(e, 25)) + ((e & f) ^ (~e & g)) +
                      k[i] + w[i];
      const auto t2 = (rotate(a, 2) ^ rotate(a, 13) ^ rotate(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
      h = g;
      g = f;
      f = e;
      e = d + t1;
      d = c;
      c = b;
      b = a;
      a = t1 + t2;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
  }
#ifdef QTALH_SHA_NI
  // Intel SHA extensions: state is kept as ABEF/CDGH, four rounds per step.
  QTALH_SHA_NI_TARGET static void compressHardware(std::uint32_t* state, const unsigned char* block) {
    const __m128i swap = _mm_set_epi64x(0x0c0d0e0f08090a0bULL, 0x0405060700010203ULL);
    auto cdab = _mm_shuffle_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i*>(state)), 0xB1);
    auto efgh = _mm_shuffle_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i*>(state + 4)), 0x1B);
    auto abef = _mm_alignr_epi8(cdab, efgh, 8);
    auto cdgh = _mm_blend_epi16(efgh, cdab, 0xF0);
    const auto abefSaved = abef, cdghSaved = cdgh;
    // Message words W[4j..4j+3], kept as a ring of the last four groups.
    __m128i w[4];
    for (int j = 0; j < 16; ++j) {
      if (j < 4) {
        w[j] = _mm_shuffle_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(block + 16 * j)), swap);
      } else {
        auto next = _mm_sha256msg1_epu32(w[j & 3], w[(j + 1) & 3]);
        next = _mm_add_epi32(next, _mm_alignr_epi8(w[(j + 3) & 3], w[(j + 2) & 3], 4));
        w[j & 3] = _mm_sha256msg2_epu32(next, w[(j + 3) & 3]);
      }
      auto words = _mm_add_epi32(w[j & 3], _mm_loadu_si128(reinterpret_cast<const __m128i*>(k + 4 * j)));
      cdgh = _mm_sha256rnds2_epu32(cdgh, abef, words);
      abef = _mm_sha256rnds2_epu32(abef, cdgh, _mm_shuffle_epi32(words, 0x0E));
    }
    abef = _mm_add_epi32(abef, abefSaved);
    cdgh = _mm_add_epi32(cdgh, cdghSaved);
    const auto feba = _mm_shuffle_epi32(abef, 0x1B);
    const auto dchg = _mm_shuffle_epi32(cdgh, 0xB1);
    _mm_storeu_si128(reinterpret_cast<__m128i*>(state), _mm_blend_epi16(feba, dchg, 0xF0));
    _mm_storeu_si128(reinterpret_cast<__m128i*>(state + 4), _mm_alignr_epi8(dchg, feba, 8));
  }
#endif
};
} // namespace alh
