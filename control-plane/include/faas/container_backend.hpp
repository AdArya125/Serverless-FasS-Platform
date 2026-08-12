#pragma once
// The runtime manager's view of "somewhere to run a container," kept
// deliberately narrow (create, find its port, remove) so any backend
// that can satisfy these three operations is interchangeable. Docker
// and Kubernetes both implement this same interface - the runtime
// manager itself does not know or care which one it is talking to.

#include <string>

namespace faas {

class ContainerBackend {
public:
    struct RunResult {
        bool ok = false;
        std::string id;
        std::string error;
    };

    virtual ~ContainerBackend() = default;

    // Starts a detached runtime from `image`, exposing `container_port`.
    virtual RunResult run_container(const std::string& image, int container_port) = 0;

    // Returns the host-reachable port for `container_port` on the
    // runtime identified by `id`, or -1 if it could not be determined.
    virtual int get_host_port(const std::string& id, int container_port) = 0;

    // Stops and removes the runtime. Safe to call on one that is
    // already gone.
    virtual bool remove_container(const std::string& id) = 0;
};

} // namespace faas
