#include "faas/docker_client.hpp"

#include "shell_exec.hpp"

namespace faas {

ContainerBackend::RunResult DockerClient::run_container(const std::string& image, int container_port) {
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
    result.id = trim(first_line(exec.output));
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
