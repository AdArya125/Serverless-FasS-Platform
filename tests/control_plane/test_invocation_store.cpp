#include "faas/invocation_store.hpp"
#include "test_framework.hpp"

using namespace faas;

TEST_CASE(record_and_get_invocation) {
    Database db(":memory:");
    InvocationStore store(db);

    std::string id = store.record("hello", R"({"name":"Adi"})", "success", R"("Hello, Adi!")", 18, false);

    auto record = store.get(id);
    CHECK(record.has_value());
    CHECK_EQ(record->function_name, "hello");
    CHECK_EQ(record->status, "success");
    CHECK_EQ(record->output, R"("Hello, Adi!")");
    CHECK_EQ(record->duration_ms, 18);
    CHECK(!record->cold_start);
}

TEST_CASE(get_missing_invocation_returns_nullopt) {
    Database db(":memory:");
    InvocationStore store(db);
    CHECK(!store.get("999").has_value());
}

TEST_CASE(get_non_numeric_id_returns_nullopt) {
    Database db(":memory:");
    InvocationStore store(db);
    CHECK(!store.get("not-an-id").has_value());
}

TEST_CASE(successive_records_get_distinct_ids) {
    Database db(":memory:");
    InvocationStore store(db);

    std::string first = store.record("hello", "{}", "success", "\"a\"", 5, true);
    std::string second = store.record("hello", "{}", "success", "\"b\"", 5, false);

    CHECK(first != second);
    CHECK(store.get(first).has_value());
    CHECK(store.get(second).has_value());
}

TEST_CASE(cold_start_flag_round_trips) {
    Database db(":memory:");
    InvocationStore store(db);

    std::string id = store.record("hello", "{}", "success", "\"a\"", 5, true);
    auto record = store.get(id);
    CHECK(record.has_value());
    CHECK(record->cold_start);
}
