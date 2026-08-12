#pragma once
// Small shell-command helpers shared by the CLI-driven container
// backends (DockerClient, KubernetesClient). Internal to control-plane;
// not part of the public include/faas/ API.

#include <string>

namespace faas {

struct ExecResult {
    int exit_code = -1;
    std::string output;
};

// Runs a shell command, capturing combined stdout+stderr.
ExecResult exec_capture(const std::string& command);

std::string trim(const std::string& s);
std::string first_line(const std::string& s);

// Defends against shell-metacharacter injection through values that get
// interpolated into a shell command (image names, resource IDs, etc.).
bool is_safe_token(const std::string& token);

} // namespace faas
