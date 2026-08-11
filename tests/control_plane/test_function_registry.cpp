#include "faas/function_registry.hpp"
#include "test_framework.hpp"

using namespace faas;

TEST_CASE(register_and_get_function) {
    FunctionRegistry registry;
    FunctionSpec spec;
    spec.name = "hello";
    spec.image = "hello:v1";
    registry.register_function(spec);

    auto fn = registry.get("hello");
    CHECK(fn.has_value());
    CHECK_EQ(fn->spec.image, "hello:v1");
    CHECK_EQ(fn->spec.name, "hello");
}

TEST_CASE(get_missing_function_returns_nullopt) {
    FunctionRegistry registry;
    CHECK(!registry.get("does-not-exist").has_value());
}

TEST_CASE(remove_function) {
    FunctionRegistry registry;
    FunctionSpec spec;
    spec.name = "hello";
    spec.image = "hello:v1";
    registry.register_function(spec);

    CHECK(registry.remove("hello"));
    CHECK(!registry.get("hello").has_value());
    CHECK(!registry.remove("hello")); // second removal is a no-op, not an error
}

TEST_CASE(list_returns_all_registered_functions) {
    FunctionRegistry registry;
    FunctionSpec a;
    a.name = "a";
    a.image = "a:v1";
    FunctionSpec b;
    b.name = "b";
    b.image = "b:v1";
    registry.register_function(a);
    registry.register_function(b);

    CHECK_EQ(registry.list().size(), 2u);
}

TEST_CASE(re_registering_overwrites_existing_function) {
    FunctionRegistry registry;
    FunctionSpec spec;
    spec.name = "hello";
    spec.image = "hello:v1";
    registry.register_function(spec);

    spec.image = "hello:v2";
    registry.register_function(spec);

    auto fn = registry.get("hello");
    CHECK_EQ(fn->spec.image, "hello:v2");
}

TEST_CASE(default_spec_values_are_applied) {
    FunctionRegistry registry;
    FunctionSpec spec;
    spec.name = "defaults";
    spec.image = "defaults:v1";
    auto fn = registry.register_function(spec);

    CHECK_EQ(fn.spec.version, 1);
    CHECK_EQ(fn.spec.timeout_ms, 5000);
    CHECK_EQ(fn.spec.memory_mb, 256);
    CHECK_EQ(fn.spec.max_concurrency, 1);
}
