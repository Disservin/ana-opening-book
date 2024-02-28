#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "../external/chess.hpp"
#include "../external/gzip/gzstream.h"
#include "../external/parallel_hashmap/phmap.h"
#include "../external/threadpool.hpp"

#include "./options.hpp"
#include "./utils.hpp"

using namespace chess;

enum class Result { WIN = 'W', DRAW = 'D', LOSS = 'L', UNKNOWN = 'U' };

struct Statistics {
  size_t wins = 0;
  size_t draws = 0;
  size_t losses = 0;

  // for sorting so that wins > draws > losses
  bool operator<(const Statistics &other) const {
    if (wins != other.wins) {
      return wins > other.wins;
    } else if (draws != other.draws) {
      return draws > other.draws;
    } else {
      return losses > other.losses;
    }
  }

  size_t total() const { return wins + draws + losses; }
};

using map_t = phmap::parallel_flat_hash_map<
    std::string, Statistics, std::hash<std::string>, std::equal_to<std::string>,
    std::allocator<std::pair<const std::string, Statistics>>, 8, std::mutex>;

map_t occurance_map = {};
std::atomic<std::size_t> total_chunks = 0;

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
      if (result != Result::UNKNOWN) {
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
      }

      skipPgn(true);
    }
  }

  void startMoves() override {}

  void move(std::string_view, std::string_view) override {}

  void endPgn() override {}

private:
  Result result = Result::UNKNOWN;
};

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

void write_results(bool conclusive) {
  std::ofstream out("results.csv");

  out << "FEN, Wins, Draws, Losses\n";

  // Sort the map by the number of wins, draws, and losses
  std::vector<std::pair<std::string, Statistics>> sorted_map(
      occurance_map.begin(), occurance_map.end());

  std::sort(sorted_map.begin(), sorted_map.end(),
            [](const auto &a, const auto &b) { return a.second < b.second; });

  for (const auto &[fen, stats] : sorted_map) {
    // when we have conclusive results, we only want to print the FENs that
    // have a clear result, i.e. only wins, only draws, or only losses
    if (conclusive)

    {
      if ((stats.wins > 0 && stats.total() == stats.wins) ||
          (stats.draws > 0 && stats.total() == stats.draws) ||
          (stats.losses > 0 && stats.total() == stats.losses)) {
        out << fen << ", " << stats.wins << ", " << stats.draws << ", "
            << stats.losses << "\n";
      }

      continue;
    }

    out << fen << ", " << stats.wins << ", " << stats.draws << ", "
        << stats.losses << "\n";
  }

  out.close();
}

/// @brief ./analyze [--path path] [-conclusive]
/// @param argc
/// @param argv
/// @return
int main(int argc, char const *argv[]) {
  CommandLine cmd(argc, argv);

  CLIOptions options;

  if (cmd.has("--path")) {
    options.path = cmd.get("--path");
  }

  if (cmd.has("-conclusive")) {
    options.conclusive = true;
  }

  process(options.path);

  write_results(options.conclusive);

  return 0;
}
