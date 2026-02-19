#include "test_module.hpp"

#include <iostream>

void TestModule::print_message() const { std::cout << _msg << '\n'; }
