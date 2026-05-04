#include "orchestrator/system_aec.hpp"

#include <doctest/doctest.h>

#include <string_view>

using acva::orchestrator::detail::parse_echo_cancel_module_line;

TEST_CASE("parse_echo_cancel_module_line: acva-loaded with custom names") {
    constexpr std::string_view line =
        "536870916\tmodule-echo-cancel\t"
        "source_name=acva-echo-source sink_name=acva-echo-sink "
        "aec_method=webrtc "
        "aec_args=analog_gain_control=0 digital_gain_control=1 "
        "noise_suppression=1 high_pass_filter=1";
    const auto m = parse_echo_cancel_module_line(line);
    REQUIRE(m.has_value());
    CHECK(m->id == "536870916");
    CHECK(m->source_name == "acva-echo-source");
    CHECK(m->sink_name == "acva-echo-sink");
}

TEST_CASE("parse_echo_cancel_module_line: tokens in either order") {
    constexpr std::string_view line =
        "42\tmodule-echo-cancel\t"
        "sink_name=foo-sink aec_method=webrtc source_name=foo-source";
    const auto m = parse_echo_cancel_module_line(line);
    REQUIRE(m.has_value());
    CHECK(m->id == "42");
    CHECK(m->source_name == "foo-source");
    CHECK(m->sink_name == "foo-sink");
}

TEST_CASE("parse_echo_cancel_module_line: source_name is the trailing token") {
    constexpr std::string_view line =
        "7\tmodule-echo-cancel\tsink_name=s aec_method=webrtc source_name=src";
    const auto m = parse_echo_cancel_module_line(line);
    REQUIRE(m.has_value());
    CHECK(m->source_name == "src");
    CHECK(m->sink_name == "s");
}

TEST_CASE("parse_echo_cancel_module_line: tokens absent (PipeWire defaults)") {
    // This is the case the new code refuses to start on: module is
    // loaded but its args don't tell us the PA-side node names.
    constexpr std::string_view line =
        "12\tmodule-echo-cancel\taec_method=webrtc";
    const auto m = parse_echo_cancel_module_line(line);
    REQUIRE(m.has_value());
    CHECK(m->id == "12");
    CHECK(m->source_name.empty());
    CHECK(m->sink_name.empty());
}

TEST_CASE("parse_echo_cancel_module_line: empty args column") {
    const auto m = parse_echo_cancel_module_line("99\tmodule-echo-cancel\t");
    REQUIRE(m.has_value());
    CHECK(m->id == "99");
    CHECK(m->source_name.empty());
    CHECK(m->sink_name.empty());
}

TEST_CASE("parse_echo_cancel_module_line: only one of the two names present") {
    {
        constexpr std::string_view line =
            "1\tmodule-echo-cancel\tsource_name=only-src aec_method=webrtc";
        const auto m = parse_echo_cancel_module_line(line);
        REQUIRE(m.has_value());
        CHECK(m->source_name == "only-src");
        CHECK(m->sink_name.empty());
    }
    {
        constexpr std::string_view line =
            "1\tmodule-echo-cancel\tsink_name=only-sink";
        const auto m = parse_echo_cancel_module_line(line);
        REQUIRE(m.has_value());
        CHECK(m->source_name.empty());
        CHECK(m->sink_name == "only-sink");
    }
}

TEST_CASE("parse_echo_cancel_module_line: rejects non-echo-cancel modules") {
    CHECK_FALSE(parse_echo_cancel_module_line(
        "3\tmodule-null-sink\tsource_name=x").has_value());
    CHECK_FALSE(parse_echo_cancel_module_line("").has_value());
    CHECK_FALSE(parse_echo_cancel_module_line("garbage").has_value());
}

TEST_CASE("parse_echo_cancel_module_line: rejects non-numeric id") {
    CHECK_FALSE(parse_echo_cancel_module_line(
        "abc\tmodule-echo-cancel\tsource_name=x").has_value());
    CHECK_FALSE(parse_echo_cancel_module_line(
        "1a\tmodule-echo-cancel\t").has_value());
}

TEST_CASE("parse_echo_cancel_module_line: rejects line that starts with whitespace") {
    CHECK_FALSE(parse_echo_cancel_module_line(
        "\tmodule-echo-cancel\tsource_name=x sink_name=y").has_value());
}
