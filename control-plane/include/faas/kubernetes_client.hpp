#pragma once
// Runs functions as Kubernetes pods (via kubectl) instead of plain
// Docker containers, satisfying the same ContainerBackend interface -
// the runtime manager cannot tell the difference. Each pod gets a
// matching NodePort Service so it is reachable the same way a Docker
// container is: a host-reachable port on 127.0.0.1.
//
// Assumes kubectl is configured against a cluster reachable at
// 127.0.0.1 - true for a single-node k3s cluster running on the same
// machine as the control plane, which is this project's target setup
// (see docs/architecture.md). A remote or multi-node cluster would need
// the node's actual IP instead.

#include "faas/container_backend.hpp"

namespace faas {

class KubernetesClient : public ContainerBackend {
public:
    RunResult run_container(const std::string& image, int container_port) override;
    int get_host_port(const std::string& id, int container_port) override;
    bool remove_container(const std::string& id) override;
};

} // namespace faas
