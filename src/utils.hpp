#pragma once

#include <filesystem>
#include <string>
#include <vector>

std::vector<std::string> get_files(const std::string &path,
                                   bool recursive = false) {
  std::vector<std::string> files;

  for (const auto &entry : std::filesystem::directory_iterator(path)) {
    if (std::filesystem::is_regular_file(entry)) {
      std::string stem = entry.path().stem().string();
      std::string extension = entry.path().extension().string();
      if (extension == ".gz") {
        if (stem.size() >= 4 && stem.substr(stem.size() - 4) == ".pgn") {
          files.push_back(entry.path().string());
        }
      } else if (extension == ".pgn") {
        files.push_back(entry.path().string());
      }
    } else if (recursive && std::filesystem::is_directory(entry)) {
      auto subdir_files = get_files(entry.path().string(), true);
      files.insert(files.end(), subdir_files.begin(), subdir_files.end());
    }
  }

  return files;
}

[[nodiscard]] inline std::vector<std::vector<std::string>>
split_chunks(const std::vector<std::string> &pgns, int target_chunks) {
  const int chunks_size = (pgns.size() + target_chunks - 1) / target_chunks;

  auto begin = pgns.begin();
  auto end = pgns.end();

  std::vector<std::vector<std::string>> chunks;

  while (begin != end) {
    auto next =
        std::next(begin, std::min(chunks_size,
                                  static_cast<int>(std::distance(begin, end))));
    chunks.push_back(std::vector<std::string>(begin, next));
    begin = next;
  }

  return chunks;
}

class CommandLine {
public:
  CommandLine(int argc, char const *argv[]) {
    for (int i = 1; i < argc; i++) {
      args.push_back(argv[i]);
    }
  }

  [[nodiscard]] inline std::string get(const std::string &flag) const {
    auto it = std::find(args.begin(), args.end(), flag);
    if (it != args.end() && ++it != args.end()) {
      return *it;
    }
    return "";
  }

  [[nodiscard]] inline bool has(const std::string &flag) const {
    return std::find(args.begin(), args.end(), flag) != args.end();
  }

private:
  std::vector<std::string> args;
};