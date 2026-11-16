// volume helpers of the audio backend
#include "audiobackend.h"
#include "test.h"

int main() {
  CHECK_NEAR(Audio::level({0.2f, 0.5f}), 0.5);
  CHECK_NEAR(Audio::level({}), 0.0);
  auto v = Audio::scaled({0.25f, 0.5f}, 1.0f); // keeps the balance
  CHECK_EQ(v.size(), 2u);
  CHECK_NEAR(v[0], 0.5);
  CHECK_NEAR(v[1], 1.0);
  auto z = Audio::scaled({0.0f, 0.0f}, 0.4f); // from silence: all channels equal
  CHECK_NEAR(z[0], 0.4f);
  CHECK_NEAR(z[1], 0.4f);
  return test_result("audio");
}
