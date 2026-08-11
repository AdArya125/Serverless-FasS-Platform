#include "faas/function_registry.hpp"

namespace faas {

Function FunctionRegistry::register_function(const FunctionSpec& spec) {
    std::lock_guard<std::mutex> lock(mutex_);

    Function fn;
    fn.spec = spec;
    fn.status = FunctionStatus::READY;
    fn.created_at = std::chrono::system_clock::now();

    functions_[spec.name] = fn;
    return fn;
}

std::optional<Function> FunctionRegistry::get(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = functions_.find(name);
    if (it == functions_.end()) return std::nullopt;
    return it->second;
}

bool FunctionRegistry::remove(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    return functions_.erase(name) > 0;
}

std::vector<Function> FunctionRegistry::list() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Function> result;
    result.reserve(functions_.size());
    for (const auto& entry : functions_) {
        result.push_back(entry.second);
    }
    return result;
}

} // namespace faas
