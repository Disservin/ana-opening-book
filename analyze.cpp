#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "./external/gzip/gzstream.h"
#include "chess.hpp"
#include "external/parallel_hashmap/phmap.h"
#include "external/threadpool.hpp"

using namespace chess;

struct Statistics {
  int wins = 0;
  int draws = 0;
  int losses = 0;
};

using map_t = phmap::parallel_flat_hash_map<
    std::string, Statistics, std::hash<std::string>, std::equal_to<std::string>,
    std::allocator<std::pair<const std::string, Statistics>>, 8, std::mutex>;

map_t occurance_map = {};
std::atomic<std::size_t> total_chunks = 0;

enum class Result { WIN = 'W', DRAW = 'D', LOSS = 'L', UNKNOWN = 'U' };

class Analyzer : public pgn::Visitor {
public:
  virtual ~Analyzer(){};

  void startPgn() override { result = Result::UNKNOWN; }

  void header(std::string_view key, std::string_view value) override {
    if (key == "Result") {
      if (value == "1-0") {
        result = Result::WIN;
      } else if (value == "0-1") {
        result = Result::LOSS;
      } else if (value == "1/2-1/2") {
        result = Result::DRAW;
      }
    }

    if (key == "FEN") {
      occurance_map.lazy_emplace_l(
          std::string(value),
          [&](map_t::value_type &v) {
            if (result == Result::WIN) {
              v.second.wins++;
            } else if (result == Result::DRAW) {
              v.second.draws++;
            } else if (result == Result::LOSS) {
              v.second.losses++;
            }
          },
          [&](const map_t::constructor &ctor) {
            ctor(std::string(value),
                 Statistics{result == Result::WIN, result == Result::DRAW,
                            result == Result::LOSS});
          });
      skipPgn(true);
    }
  }

  void startMoves() override {}

  void move(std::string_view move, std::string_view comment) override {}

  void endPgn() override {}

private:
  Result result = Result::UNKNOWN;
};

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

void analyze_pgn(const std::vector<std::string> &files) {
  for (const auto &file : files) {
    const auto pgn_iterator = [&](std::istream &iss) {
      auto vis = std::make_unique<Analyzer>();

      pgn::StreamParser parser(iss);

      try {
        parser.readGames(*vis);
      } catch (const std::exception &e) {
        std::cout << "Error when parsing: " << file << std::endl;
        std::cerr << e.what() << '\n';
      }
    };

    if (file.size() >= 3 && file.substr(file.size() - 3) == ".gz") {
      igzstream input(file.c_str());
      pgn_iterator(input);
    } else {
      std::ifstream pgn_stream(file);
      pgn_iterator(pgn_stream);
      pgn_stream.close();
    }
  }
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

void process(const std::string &path) {
  int concurrency = std::max(1, int(std::thread::hardware_concurrency()));
  int target_chunks = 4 * concurrency;

  const auto files_pgn = get_files(path, true);

  auto files_chunked = split_chunks(files_pgn, target_chunks);

  // Mutex for progress success
  std::mutex progress_mutex;

  // Create a thread pool
  ThreadPool pool(concurrency);

  // Print progress
  std::cout << "\rProgress: " << total_chunks << "/" << files_chunked.size()
            << std::flush;

  for (const auto &files : files_chunked) {
    pool.enqueue([&files, &files_chunked, &progress_mutex]() {
      analyze_pgn(files);

      total_chunks++;

      // Limit the scope of the lock
      {
        const std::lock_guard<std::mutex> lock(progress_mutex);

        // Print progress
        std::cout << "\rProgress: " << total_chunks << "/"
                  << files_chunked.size() << std::flush;
      }
    });
  }

  // Wait for all threads to finish
  pool.wait();
}

void write_results() {
  std::ofstream out("results.csv");

  out << "FEN, Wins, Draws, Losses\n";

  for (const auto &[fen, stats] : occurance_map) {
    out << fen << ", " << stats.wins << ", " << stats.draws << ", "
        << stats.losses << "\n";
  }

  out.close();
}

int main(int argc, char const *argv[]) {

  std::string path = "./pgns";

  if (argc > 1) {
    path = argv[1];
  }

  process(path);

  write_results();

  return 0;
}
