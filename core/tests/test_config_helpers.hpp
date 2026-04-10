#ifndef WORKSPACES_CORE_CORE_TESTS_TEST_CONFIG_HELPERS_HPP
#define WORKSPACES_CORE_CORE_TESTS_TEST_CONFIG_HELPERS_HPP

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <system_error>

namespace core::test {

class TempConfigFile {
public:
    explicit TempConfigFile(const std::string& yaml_contents) {
        _path = std::filesystem::temp_directory_path() /
                std::filesystem::path("core-config-test-" + std::to_string(_counter++) + ".yaml");

        std::ofstream output(_path);
        if (!output.is_open()) {
            throw std::runtime_error("Failed to open temp config file for writing");
        }
        output << yaml_contents;
    }

    ~TempConfigFile() {
        std::error_code error;
        std::filesystem::remove(_path, error);
    }

    TempConfigFile(const TempConfigFile&) = delete;
    TempConfigFile& operator=(const TempConfigFile&) = delete;
    TempConfigFile(TempConfigFile&&) = delete;
    TempConfigFile& operator=(TempConfigFile&&) = delete;

    [[nodiscard]] std::string path() const {
        return _path.string();
    }

private:
    inline static std::size_t _counter{0};
    std::filesystem::path _path;
};

} // namespace core::test

#endif
