#pragma once

#include <optional>
#include <string>

#include "../external/json.hpp"

struct TestMetaData {
  std::optional<std::string> book;
  std::optional<bool> sprt;
  std::optional<int> book_depth;
};

template <typename T = std::string>
std::optional<T> get_optional(const nlohmann::json &j, const char *name) {
  const auto it = j.find(name);
  if (it != j.end()) {
    return std::optional<T>(j[name]);
  } else {
    return std::nullopt;
  }
}

void from_json(const nlohmann::json &nlohmann_json_j,
               TestMetaData &nlohmann_json_t) {
  auto &j = nlohmann_json_j["args"];

  nlohmann_json_t.book_depth =
      get_optional(j, "book_depth").has_value()
          ? std::optional<int>(std::stoi(get_optional(j, "book_depth").value()))
          : std::nullopt;

  nlohmann_json_t.sprt =
      j.contains("sprt") ? std::optional<bool>(true) : std::nullopt;

  nlohmann_json_t.book = get_optional(j, "book");
}