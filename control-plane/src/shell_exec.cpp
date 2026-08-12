#include "shell_exec.hpp"

#include <sys/wait.h>

#include <array>
#include <cctype>
#include <cstdio>

namespace faas {

ExecResult exec_capture(const std::string& command) {
    ExecResult result;
    FILE* pipe = popen((command + " 2>&1").c_str(), "r");
    if (!pipe) {
        result.output = "failed to start command: " + command;
        return result;
    }

    std::array<char, 4096> buffer{};
    size_t n;
    while ((n = fread(buffer.data(), 1, buffer.size(), pipe)) > 0) {
        result.output.append(buffer.data(), n);
    }
    int status = pclose(pipe);
    result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return result;
}

std::string trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

std::string first_line(const std::string& s) {
    auto pos = s.find('\n');
    return pos == std::string::npos ? s : s.substr(0, pos);
}

bool is_safe_token(const std::string& token) {
    if (token.empty()) return false;
    for (char c : token) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' ||
              c == '.' || c == ':' || c == '/')) {
            return false;
        }
    }
    return true;
}

} // namespace faas
