#pragma once
// Thin wrapper around the `docker` CLI for the handful of operations the
// runtime manager needs. Shelling out (rather than talking to the Docker
// Engine API directly) keeps this small and avoids adding a socket/HTTP
// dependency just to reach the daemon.

#include <string>

namespace faas {

class DockerClient {
public:
    struct RunResult {
        bool ok = false;
        std::string container_id;
        std::string error;
    };

    // Starts a detached container from `image`, letting Docker
    // auto-assign a host port for `container_port`.
    RunResult run_container(const std::string& image, int container_port);

    // Returns the host port Docker assigned for container_port, or -1
    // if it could not be determined.
    int get_host_port(const std::string& container_id, int container_port);

    // Force-stops and removes a container. Safe to call on an already
    // dead container.
    bool remove_container(const std::string& container_id);
};

} // namespace faas
