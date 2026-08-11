#include "faas/docker_client.hpp"

#include <sys/wait.h>

#include <array>
#include <cctype>
#include <cstdio>

namespace faas {

namespace {

struct ExecResult {
    int exit_code = -1;
    std::string output;
};

// Runs a shell command, capturing combined stdout+stderr.
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

// Defends against shell-metacharacter injection through image names or
// container IDs before they are interpolated into a shell command.
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

} // namespace

DockerClient::RunResult DockerClient::run_container(const std::string& image, int container_port) {
    RunResult result;
    if (!is_safe_token(image)) {
        result.error = "invalid image name: " + image;
        return result;
    }

    std::string cmd = "docker run -d -p " + std::to_string(container_port) + " " + image;
    ExecResult exec = exec_capture(cmd);
    if (exec.exit_code != 0) {
        result.error = trim(exec.output);
        return result;
    }

    result.ok = true;
    result.container_id = trim(first_line(exec.output));
    return result;
}

int DockerClient::get_host_port(const std::string& container_id, int container_port) {
    if (!is_safe_token(container_id)) return -1;

    std::string cmd = "docker port " + container_id + " " + std::to_string(container_port);
    ExecResult exec = exec_capture(cmd);
    if (exec.exit_code != 0) return -1;

    std::string line = trim(first_line(exec.output));
    auto colon = line.find_last_of(':');
    if (colon == std::string::npos) return -1;

    try {
        return std::stoi(line.substr(colon + 1));
    } catch (const std::exception&) {
        return -1;
    }
}

bool DockerClient::remove_container(const std::string& container_id) {
    if (!is_safe_token(container_id)) return false;
    ExecResult exec = exec_capture("docker rm -f " + container_id);
    return exec.exit_code == 0;
}

} // namespace faas
