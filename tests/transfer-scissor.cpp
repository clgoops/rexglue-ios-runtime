#include <rex/graphics/vulkan/transfer_scissor.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

using rex::graphics::vulkan::TransferScissor;
static unsigned checks = 0;
static void check(bool value) {
  ++checks;
  if (!value) { std::fprintf(stderr, "Transfer scissor check %u failed\n", checks); std::abort(); }
}
static bool equal(VkRect2D a, VkRect2D b) {
  return a.offset.x == b.offset.x && a.offset.y == b.offset.y &&
         a.extent.width == b.extent.width && a.extent.height == b.extent.height;
}
static bool covers(VkRect2D area, double x, double y) {
  return x >= area.offset.x && y >= area.offset.y &&
         x < double(area.offset.x) + area.extent.width &&
         y < double(area.offset.y) + area.extent.height;
}
struct Rectangle { uint32_t x, y, width, height; };

// Independently project the actual transfer's float NDC vertex expression back
// to window coordinates, then scan host MSAA sample locations. Every covered
// sample in the original scissor must remain covered. This catches scaling,
// offset, union and rounding mistakes without comparing two copies of Get().
static void raster_check(VkExtent2D extent, uint32_t sx, uint32_t sy,
                         const std::vector<Rectangle>& rectangles, unsigned samples) {
  TransferScissor scissor(extent, sx, sy);
  for (auto r : rectangles) scissor.Include(r.x, r.y, r.width, r.height);
  const auto area = scissor.Get(true);
  check(area.offset.x >= 0 && area.offset.y >= 0);
  check(uint64_t(area.offset.x) + area.extent.width <= extent.width);
  check(uint64_t(area.offset.y) + area.extent.height <= extent.height);
  check(equal(scissor.Get(false), {{0, 0}, extent}));
  auto pow2 = [](uint32_t value) { uint32_t p = 1; while (p < value) p *= 2; return p; };
  const float vw = float(pow2(extent.width)), vh = float(pow2(extent.height));
  const float px = (2.0f / vw) * float(sx), py = (2.0f / vh) * float(sy);
  const std::array<std::array<double, 2>, 4> positions =
      samples == 1 ? std::array<std::array<double, 2>, 4>{{{0.5, 0.5}}} :
      samples == 2 ? std::array<std::array<double, 2>, 4>{{{0.75, 0.75}, {0.25, 0.25}}} :
                    std::array<std::array<double, 2>, 4>{{{0.375, 0.125}, {0.875, 0.375},
                                                       {0.125, 0.625}, {0.625, 0.875}}};
  for (auto r : rectangles) {
    const float x0 = -1.0f + r.x * px, y0 = -1.0f + r.y * py;
    const float x1 = x0 + r.width * px, y1 = y0 + r.height * py;
    const double wx0 = (x0 + 1.0) * vw / 2.0, wy0 = (y0 + 1.0) * vh / 2.0;
    const double wx1 = (x1 + 1.0) * vw / 2.0, wy1 = (y1 + 1.0) * vh / 2.0;
    for (uint32_t y = 0; y < extent.height; ++y) for (uint32_t x = 0; x < extent.width; ++x)
      for (unsigned i = 0; i < samples; ++i) {
        const double xx = x + positions[i][0], yy = y + positions[i][1];
        if (xx >= wx0 && xx < wx1 && yy >= wy0 && yy < wy1) check(covers(area, xx, yy));
      }
  }
}

int main() {
  TransferScissor c({1280, 8192}, 1, 1);
  c.Include(0, 0, 320, 180);
  check(equal(c.Get(true), {{0, 0}, {321, 181}}));
  c.Include(900, 700, 100, 80);
  check(equal(c.Get(true), {{0, 0}, {1001, 781}}));
  check(equal(c.Get(false), {{0, 0}, {1280, 8192}}));
  TransferScissor edge({1280, 720}, 2, 3);
  edge.Include(630, 230, 100, 100);
  check(equal(edge.Get(true), {{1259, 689}, {21, 31}}));
  TransferScissor empty({1280, 720}, 1, 1);
  empty.Include(0, 0, 0, 20); empty.Include(5000, 5000, 10, 10);
  check(equal(empty.Get(true), {{0, 0}, {1280, 720}}));
  TransferScissor invalid({1280, 720}, 0, 1);
  invalid.Include(1, 1, 4, 4);
  check(equal(invalid.Get(true), {{0, 0}, {1280, 720}}));
  TransferScissor overflow({1280, 720}, UINT32_MAX, UINT32_MAX);
  overflow.Include(UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX);
  overflow.Include(0, 0, UINT32_MAX, UINT32_MAX);
  check(equal(overflow.Get(true), {{0, 0}, {1280, 720}}));
  std::mt19937 random(0x045);
  for (unsigned samples : {1u, 2u, 4u}) for (uint32_t sx : {1u, 2u, 3u})
    for (uint32_t sy : {1u, 2u, 4u}) for (unsigned n = 0; n < 90; ++n) {
      std::vector<Rectangle> rectangles;
      for (unsigned r = 0, count = 1 + random() % 5; r < count; ++r)
        rectangles.push_back({uint32_t(random() % 100), uint32_t(random() % 100),
                              uint32_t(random() % 40), uint32_t(random() % 40)});
      raster_check({64, 80}, sx, sy, rectangles, samples);
    }
  std::printf("%u transfer-scissor sample coverage checks passed\n", checks);
}
