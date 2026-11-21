#include "kernels.h"

#include <cstring>

#if defined(ESP_PLATFORM)
#include "soc/soc_caps.h"
#endif

#if defined(SOC_CPU_HAS_PIE) && SOC_CPU_HAS_PIE
#define HM_BENCH_PIE 1
#else
#define HM_BENCH_PIE 0
#endif

namespace hm_kernels {

void copy_words(void* dst, const void* src, std::size_t bytes) {
  auto* d = static_cast<std::uint8_t*>(dst);
  const auto* s = static_cast<const std::uint8_t*>(src);
  if (((reinterpret_cast<std::uintptr_t>(d) | reinterpret_cast<std::uintptr_t>(s)) & 3U) == 0) {
    auto* dw = reinterpret_cast<std::uint32_t*>(d);
    const auto* sw = reinterpret_cast<const std::uint32_t*>(s);
    std::size_t words = bytes / 4;
    while (words >= 8) {
      const std::uint32_t a = sw[0], b = sw[1], c = sw[2], e = sw[3];
      const std::uint32_t f = sw[4], g = sw[5], h = sw[6], i = sw[7];
      dw[0] = a;
      dw[1] = b;
      dw[2] = c;
      dw[3] = e;
      dw[4] = f;
      dw[5] = g;
      dw[6] = h;
      dw[7] = i;
      dw += 8;
      sw += 8;
      words -= 8;
    }
    while (words-- > 0) *dw++ = *sw++;
    d = reinterpret_cast<std::uint8_t*>(dw);
    s = reinterpret_cast<const std::uint8_t*>(sw);
    bytes &= 3U;
  }
  while (bytes-- > 0) *d++ = *s++;
}

namespace {
inline std::uint16_t blend_px(std::uint16_t f, std::uint16_t b, unsigned a, unsigned ia) {
  const unsigned r = (((f & 0xF800U) * a) >> 8) + (((b & 0xF800U) * ia) >> 8);
  const unsigned g = (((f & 0x07E0U) * a) >> 8) + (((b & 0x07E0U) * ia) >> 8);
  const unsigned bl = (((f & 0x001FU) * a) >> 8) + (((b & 0x001FU) * ia) >> 8);
  return static_cast<std::uint16_t>((r & 0xF800U) | (g & 0x07E0U) | (bl & 0x001FU));
}
}  // namespace

void blend565_c(std::uint16_t* out, const std::uint16_t* fg, const std::uint16_t* bg, std::size_t n,
                unsigned alpha) {
  const unsigned a = alpha > 256U ? 256U : alpha;
  const unsigned ia = 256U - a;
  for (std::size_t i = 0; i < n; ++i) out[i] = blend_px(fg[i], bg[i], a, ia);
}

void lut_fill_c(std::uint16_t* out, const std::uint8_t* idx, const std::uint16_t* lut,
                std::size_t n) {
  std::size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    const std::uint16_t a = lut[idx[i]], b = lut[idx[i + 1]], c = lut[idx[i + 2]],
                        d = lut[idx[i + 3]];
    out[i] = a;
    out[i + 1] = b;
    out[i + 2] = c;
    out[i + 3] = d;
  }
  for (; i < n; ++i) out[i] = lut[idx[i]];
}

void scale2x_c(std::uint16_t* dst, const std::uint16_t* src, int w, int h) {
  const int sw = w / 2;
  for (int y = 0; y < h / 2; ++y) {
    const std::uint16_t* s = src + static_cast<std::size_t>(y) * sw;
    std::uint16_t* d0 = dst + static_cast<std::size_t>(2 * y) * w;
    auto* d32 = reinterpret_cast<std::uint32_t*>(d0);
    if ((reinterpret_cast<std::uintptr_t>(d0) & 3U) == 0) {
      for (int x = 0; x < sw; ++x) d32[x] = static_cast<std::uint32_t>(s[x]) * 0x00010001U;
    } else {
      for (int x = 0; x < sw; ++x) d0[2 * x] = d0[2 * x + 1] = s[x];
    }
    std::memcpy(d0 + w, d0, static_cast<std::size_t>(w) * 2U);
  }
}

void rot90_c(std::uint16_t* dst, const std::uint16_t* src, int w, int h) {
  // dst is h wide and w tall; source row y becomes destination column (h - 1 - y).
  for (int y = 0; y < h; ++y) {
    const std::uint16_t* s = src + static_cast<std::size_t>(y) * w;
    std::uint16_t* d = dst + (h - 1 - y);
    for (int x = 0; x < w; ++x) d[static_cast<std::size_t>(x) * h] = s[x];
  }
}

bool pie_available() {
  return HM_BENCH_PIE != 0;
}

#if HM_BENCH_PIE

void copy_pie(void* dst, const void* src, std::size_t bytes) {
  // PIE address operands must be x8..x15 (the assembler rejects others), so pin them.
  register std::uint8_t* d asm("a0") = static_cast<std::uint8_t*>(dst);
  register const std::uint8_t* s asm("a1") = static_cast<const std::uint8_t*>(src);
  std::size_t blocks = bytes / 64;
  while (blocks-- > 0) {
    asm volatile(
        "esp.vld.128.ip q0, %0, 16\n\t"
        "esp.vld.128.ip q1, %0, 16\n\t"
        "esp.vld.128.ip q2, %0, 16\n\t"
        "esp.vld.128.ip q3, %0, 16\n\t"
        "esp.vst.128.ip q0, %1, 16\n\t"
        "esp.vst.128.ip q1, %1, 16\n\t"
        "esp.vst.128.ip q2, %1, 16\n\t"
        "esp.vst.128.ip q3, %1, 16\n\t"
        : "+r"(s), "+r"(d)
        :
        : "memory");
  }
  std::memcpy(d, s, bytes % 64);
}

void blend565_pie(std::uint16_t* out, const std::uint16_t* fg, const std::uint16_t* bg,
                  std::size_t n, unsigned alpha) {
  const unsigned a = alpha > 256U ? 256U : alpha;
  alignas(16) static const std::uint16_t kMasks[3] = {0xF800U, 0x07E0U, 0x001FU};
  alignas(16)
      std::uint16_t av[2] = {static_cast<std::uint16_t>(a), static_cast<std::uint16_t>(256U - a)};
  register const std::uint16_t* mr asm("a3") = &kMasks[0];
  register const std::uint16_t* mg asm("a4") = &kMasks[1];
  register const std::uint16_t* mb asm("a5") = &kMasks[2];
  register const std::uint16_t* pa asm("a0") = &av[0];
  register const std::uint16_t* pia asm("a1") = &av[1];
  register unsigned shift asm("a2") = 8;
  // q6 = alpha, q7 = 256 - alpha, broadcast to all eight 16-bit lanes; SAR = 8 so each
  // esp.vmul.u16 yields (x * y) >> 8.
  asm volatile(
      "esp.vldbc.16.ip q6, %0, 0\n\t"
      "esp.vldbc.16.ip q7, %1, 0\n\t"
      "esp.movx.w.sar %2\n\t"
      :
      : "r"(pa), "r"(pia), "r"(shift)
      : "memory");
  std::size_t vecs = n / 8;
  register const std::uint16_t* f asm("a0") = fg;
  register const std::uint16_t* b asm("a1") = bg;
  register std::uint16_t* o asm("a2") = out;
  while (vecs-- > 0) {
    asm volatile(
        "esp.vld.128.ip q0, %0, 16\n\t"  // q0 = fg
        "esp.vld.128.ip q1, %1, 16\n\t"  // q1 = bg
        // red
        "esp.vldbc.16.ip q5, %3, 0\n\t"
        "esp.andq q2, q0, q5\n\t"
        "esp.andq q3, q1, q5\n\t"
        "esp.vmul.u16 q2, q2, q6\n\t"
        "esp.vmul.u16 q3, q3, q7\n\t"
        "esp.vadd.u16 q2, q2, q3\n\t"
        "esp.andq q4, q2, q5\n\t"
        // green
        "esp.vldbc.16.ip q5, %4, 0\n\t"
        "esp.andq q2, q0, q5\n\t"
        "esp.andq q3, q1, q5\n\t"
        "esp.vmul.u16 q2, q2, q6\n\t"
        "esp.vmul.u16 q3, q3, q7\n\t"
        "esp.vadd.u16 q2, q2, q3\n\t"
        "esp.andq q2, q2, q5\n\t"
        "esp.orq q4, q4, q2\n\t"
        // blue
        "esp.vldbc.16.ip q5, %5, 0\n\t"
        "esp.andq q2, q0, q5\n\t"
        "esp.andq q3, q1, q5\n\t"
        "esp.vmul.u16 q2, q2, q6\n\t"
        "esp.vmul.u16 q3, q3, q7\n\t"
        "esp.vadd.u16 q2, q2, q3\n\t"
        "esp.andq q2, q2, q5\n\t"
        "esp.orq q4, q4, q2\n\t"
        "esp.vst.128.ip q4, %2, 16\n\t"
        : "+r"(f), "+r"(b), "+r"(o)
        : "r"(mr), "r"(mg), "r"(mb)
        : "memory");
  }
  const std::size_t done = (n / 8) * 8;
  blend565_c(out + done, fg + done, bg + done, n - done, a);
}

#else

void copy_pie(void* dst, const void* src, std::size_t bytes) {
  copy_words(dst, src, bytes);
}
void blend565_pie(std::uint16_t* out, const std::uint16_t* fg, const std::uint16_t* bg,
                  std::size_t n, unsigned alpha) {
  blend565_c(out, fg, bg, n, alpha);
}

#endif

}  // namespace hm_kernels
