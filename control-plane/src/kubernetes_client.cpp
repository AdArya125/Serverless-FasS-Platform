#include "faas/kubernetes_client.hpp"

#include "shell_exec.hpp"

#include <atomic>
#include <chrono>

namespace faas {

namespace {

// Kubernetes resource names must be lowercase alphanumeric plus
// hyphens; a counter plus a timestamp is enough to keep them unique
// within a single control-plane process.
std::string generate_pod_name() {
    static std::atomic<long> counter{0};
    long now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count();
    return "faas-" + std::to_string(now_ms % 10000000) + "-" + std::to_string(++counter);
}

} // namespace

ContainerBackend::RunResult KubernetesClient::run_container(const std::string& image, int container_port) {
    RunResult result;
    if (!is_safe_token(image)) {
        result.error = "invalid image name: " + image;
        return result;
    }

    std::string name = generate_pod_name();

    std::string run_cmd = "kubectl run " + name + " --image=" + image +
                           " --port=" + std::to_string(container_port) +
                           " --image-pull-policy=IfNotPresent --restart=Never";
    ExecResult run_exec = exec_capture(run_cmd);
    if (run_exec.exit_code != 0) {
        result.error = "failed to create pod: " + trim(run_exec.output);
        return result;
    }

    ExecResult wait_exec = exec_capture("kubectl wait --for=condition=Ready pod/" + name + " --timeout=60s");
    if (wait_exec.exit_code != 0) {
        exec_capture("kubectl delete pod " + name + " --ignore-not-found --wait=false");
        result.error = "pod did not become ready: " + trim(wait_exec.output);
        return result;
    }

    std::string expose_cmd = "kubectl expose pod " + name + " --port=" + std::to_string(container_port) +
                              " --name=" + name + " --type=NodePort";
    ExecResult expose_exec = exec_capture(expose_cmd);
    if (expose_exec.exit_code != 0) {
        exec_capture("kubectl delete pod " + name + " --ignore-not-found --wait=false");
        result.error = "failed to expose pod: " + trim(expose_exec.output);
        return result;
    }

    result.ok = true;
    result.id = name;
    return result;
}

int KubernetesClient::get_host_port(const std::string& id, int /*container_port*/) {
    if (!is_safe_token(id)) return -1;

    ExecResult exec = exec_capture("kubectl get svc " + id + " -o jsonpath={.spec.ports[0].nodePort}");
    if (exec.exit_code != 0) return -1;

    try {
        return std::stoi(trim(exec.output));
    } catch (const std::exception&) {
        return -1;
    }
}

bool KubernetesClient::remove_container(const std::string& id) {
    if (!is_safe_token(id)) return false;
    exec_capture("kubectl delete svc " + id + " --ignore-not-found --wait=false");
    ExecResult exec = exec_capture("kubectl delete pod " + id + " --ignore-not-found --wait=false");
    return exec.exit_code == 0;
}

} // namespace faas
