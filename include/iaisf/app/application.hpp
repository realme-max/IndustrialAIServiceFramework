#pragma once

#include <ostream>
#include <string>
#include <vector>

namespace iaisf {

class Application {
public:
    Application(std::ostream& output, std::ostream& error_output);

    /**
     * Runs the Phase 1 command-line application.
     *
     * args contains command-line arguments excluding the executable name.
     */
    int run(const std::vector<std::string>& args);

    [[nodiscard]] static std::string version_text();
    [[nodiscard]] static std::string usage_text();

private:
    int run_with_config(const std::string& path);
    int report_invalid_arguments(const std::string& message);

    std::ostream& output_;
    std::ostream& error_output_;
};

}  // namespace iaisf

