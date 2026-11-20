#include "harness.h"

// smoke：确认 harness 本身在被使用之前是可信的。
// Step 5 会临时把第一条断言改成 1 + 1 == 3，验证失败路径真的能被报告出来。

SR_TEST(harness_detects_a_passing_assert) { SR_ASSERT(1 + 1 == 2); }

SR_TEST(harness_near_tolerance_is_inclusive) { SR_ASSERT_NEAR(1.0f, 1.000001f, 1e-5f); }

SR_TEST(harness_eq_compares_integer_values) { SR_ASSERT_EQ(0xFF112233u, 0xFF112233u); }
