#pragma once

#include <string>
#include <vector>

[[nodiscard]] std::vector<std::string> get_files(const std::string &path,
                                                 bool recursive = false);

[[nodiscard]] std::vector<std::vector<std::string>>
split_chunks(const std::vector<std::string> &pgns, int target_chunks);

class CommandLine {
public:
  CommandLine(int argc, char const *argv[]);
  [[nodiscard]] std::string get(const std::string &flag) const;
  [[nodiscard]] bool has(const std::string &flag) const;

private:
  std::vector<std::string> args;
};
