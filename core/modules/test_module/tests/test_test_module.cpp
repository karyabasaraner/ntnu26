#include <gtest/gtest.h>

#include "../test_module.hpp"

TEST(TestModuleTest, PrintMessage) {
    TestModule const test_module;
    EXPECT_NO_THROW(test_module.print_message());
}
