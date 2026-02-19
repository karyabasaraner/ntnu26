#ifndef WORKSPACES_CORE_CORE_MODULES_TEST_MODULE_TEST_MODULE_HPP
#define WORKSPACES_CORE_CORE_MODULES_TEST_MODULE_TEST_MODULE_HPP

#include <string>

class TestModule {
   public:
    void print_message() const;

   private:
    std::string _msg = "Hello from TestModule!";
};

#endif