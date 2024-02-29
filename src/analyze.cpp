#include <iostream>
#include <regex>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "../external/chess.hpp"
#include "../external/gzip/gzstream.h"
#include "../external/parallel_hashmap/phmap.h"
#include "../external/threadpool.hpp"
#include "./options.hpp"
#include "./test.hpp"
#include "./utils.hpp"

using namespace chess;
namespace fs = std::filesystem;
using json   = nlohmann::json;

enum class Result { WIN = 'W', DRAW = 'D', LOSS = 'L', UNKNOWN = 'U' };

struct Statistics {
    size_t wins   = 0;
    size_t draws  = 0;
    size_t losses = 0;

    double draw_rate() const { return double(draws) / total(); }

    // for sorting according to draw rate

    bool operator<(const Statistics &other) const {
        if (draw_rate() == other.draw_rate()) {
            if (total() == other.total()) {
                if (wins == other.wins) {
                    if (draws == other.draws) {
                        return losses > other.losses;
                    }
                    return draws > other.draws;
                }
                return wins > other.wins;
            }
            return total() > other.total();
        }
        return draw_rate() < other.draw_rate();
    }

    size_t total() const { return wins + draws + losses; }
};

using map_t = phmap::parallel_flat_hash_map<
    std::string, Statistics, std::hash<std::string>, std::equal_to<std::string>,
    std::allocator<std::pair<const std::string, Statistics>>, 8, std::mutex>;
using map_meta = std::unordered_map<std::string, TestMetaData>;

map_t occurance_map                   = {};
std::atomic<std::size_t> total_chunks = 0;
std::atomic<std::size_t> total_games  = 0;

class Analyzer : public pgn::Visitor {
   public:
    Analyzer(const CLIOptions &options) : options(options) {}
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

                total_games++;
            }

            skipPgn(true);
        }
    }

    void startMoves() override {}

    void move(std::string_view, std::string_view) override {}

    void endPgn() override {}

   private:
    Result result = Result::UNKNOWN;
    const CLIOptions &options;
};

[[nodiscard]] map_meta get_metadata(const std::vector<std::string> &file_list,
                                    bool allow_duplicates) {
    map_meta meta_map;
    std::unordered_map<std::string, std::string> test_map;  // map to check for duplicate tests
    std::set<std::string> test_warned;
    for (const auto &pathname : file_list) {
        fs::path path(pathname);
        std::string directory     = path.parent_path().string();
        std::string filename      = path.filename().string();
        std::string test_id       = filename.substr(0, filename.find_last_of('-'));
        std::string test_filename = pathname.substr(0, pathname.find_last_of('-'));

        if (test_map.find(test_id) == test_map.end()) {
            test_map[test_id] = test_filename;
        } else if (test_map[test_id] != test_filename) {
            if (test_warned.find(test_filename) == test_warned.end()) {
                std::cout << (allow_duplicates ? "Warning" : "Error")
                          << ": Detected a duplicate of test " << test_id << " in directory "
                          << directory << std::endl;
                test_warned.insert(test_filename);

                if (!allow_duplicates) {
                    std::cout << "Use --allowDuplicates to continue nonetheless." << std::endl;
                    std::exit(1);
                }
            }
        }

        // load the JSON data from disk, only once for each test
        if (meta_map.find(test_filename) == meta_map.end()) {
            std::ifstream json_file(test_filename + ".json");

            if (!json_file.is_open()) continue;

            json metadata = json::parse(json_file);

            meta_map[test_filename] = metadata.get<TestMetaData>();
        }
    }
    return meta_map;
}

void filter_files_book(std::vector<std::string> &file_list, const map_meta &meta_map,
                       const std::regex &regex_book, bool invert) {
    const auto pred = [&regex_book, invert, &meta_map](const std::string &pathname) {
        std::string test_filename = pathname.substr(0, pathname.find_last_of('-'));

        // check if metadata and "book" entry exist
        if (meta_map.find(test_filename) != meta_map.end() &&
            meta_map.at(test_filename).book.has_value()) {
            bool match = std::regex_match(meta_map.at(test_filename).book.value(), regex_book);

            return invert ? match : !match;
        }

        // missing metadata or "book" entry can never match
        return true;
    };

    file_list.erase(std::remove_if(file_list.begin(), file_list.end(), pred), file_list.end());
}

void filter_files_sprt(std::vector<std::string> &file_list, const map_meta &meta_map) {
    const auto pred = [&meta_map](const std::string &pathname) {
        std::string test_filename = pathname.substr(0, pathname.find_last_of('-'));

        // check if metadata and "sprt" entry exist
        if (meta_map.find(test_filename) != meta_map.end() &&
            meta_map.at(test_filename).sprt.has_value() &&
            meta_map.at(test_filename).sprt.value()) {
            return false;
        }

        return true;
    };

    file_list.erase(std::remove_if(file_list.begin(), file_list.end(), pred), file_list.end());
}

void analyze_pgn(const std::vector<std::string> &files, const CLIOptions &options) {
    for (const auto &file : files) {
        const auto pgn_iterator = [&](std::istream &iss) {
            auto vis = std::make_unique<Analyzer>(options);

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

void process(const CLIOptions &options) {
    int target_chunks = 4 * options.concurrency;

    auto files_pgn = get_files(options.dir, true);

    const auto meta_map = get_metadata(files_pgn, options.allow_duplicates);

    if (!options.match_book.empty()) {
        std::cout << "Filtering pgn files " << (options.matchBookInverted ? "not " : "")
                  << "matching the book name " << options.match_book << std::endl;
        std::regex regex(options.match_book);
        filter_files_book(files_pgn, meta_map, regex, options.matchBookInverted);
    }

    if (options.only_sprt) {
        std::cout << "Filtering pgn files that are not part of a SPRT test" << std::endl;
        filter_files_sprt(files_pgn, meta_map);
    }

    auto files_chunked = split_chunks(files_pgn, target_chunks);

    // Mutex for progress success
    std::mutex progress_mutex;

    // Create a thread pool
    ThreadPool pool(options.concurrency);

    // Print progress
    std::cout << "\rProgress: " << total_chunks << "/" << files_chunked.size() << std::flush;

    for (const auto &files : files_chunked) {
        pool.enqueue([&files, &files_chunked, &progress_mutex, &options]() {
            analyze_pgn(files, options);

            total_chunks++;

            // Limit the scope of the lock
            {
                const std::lock_guard<std::mutex> lock(progress_mutex);

                // Print progress
                std::cout << "\rProgress: " << total_chunks << "/" << files_chunked.size()
                          << std::flush;
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
    std::vector<std::pair<std::string, Statistics>> sorted_map(occurance_map.begin(),
                                                               occurance_map.end());

    std::sort(sorted_map.begin(), sorted_map.end(),
              [](const auto &a, const auto &b) { return a.second < b.second; });

    std::size_t wins   = 0;
    std::size_t draws  = 0;
    std::size_t losses = 0;

    for (const auto &[fen, stats] : sorted_map) {
        // when we have conclusive results, we only want to print the FENs that
        // have a clear result, i.e. only wins, only draws, or only losses
        if (conclusive) {
            if (!((stats.wins > 0 && stats.total() == stats.wins) ||
                  (stats.draws > 0 && stats.total() == stats.draws) ||
                  (stats.losses > 0 && stats.total() == stats.losses))) {
                continue;
            }
        }

        wins += stats.wins;
        draws += stats.draws;
        losses += stats.losses;

        out << fen << ", " << stats.wins << ", " << stats.draws << ", " << stats.losses << "\n";
    }

    std::cout << "Analyzed " << total_games << " games in total (W/D/L = " << wins << "/" << draws
              << "/" << losses << ")" << std::endl;
    std::cout << "Wrote results to results.csv" << std::endl;

    out.close();
}

/// @brief ./analysis [--dir path] [--concurrency n] [--matchBook book]
/// [-allowDuplicates] [-onlySprt] [-conclusive] [-matchBookInverted]
/// @param argc
/// @param argv
/// @return
int main(int argc, char const *argv[]) {
    CommandLine cmd(argc, argv);

    CLIOptions options;

    if (cmd.has("--dir")) {
        options.dir = cmd.get("--dir");
    }

    if (cmd.has("-conclusive")) {
        options.conclusive = true;
    }

    if (cmd.has("--concurrency")) {
        options.concurrency = std::stoi(cmd.get("--concurrency"));
    } else {
        options.concurrency = std::max(1, int(std::thread::hardware_concurrency()));
    }

    if (cmd.has("matchBook")) {
        options.match_book = cmd.get("matchBook");

        if (options.match_book.empty()) {
            std::cerr << "Error: --matchBook cannot be empty" << std::endl;
            return 1;
        }

        if (cmd.has("--matchBookInverted")) {
            options.matchBookInverted = true;
        }
    }

    if (cmd.has("--allowDuplicates")) {
        options.allow_duplicates = true;
    }

    if (cmd.has("--onlySprt")) {
        options.only_sprt = true;
    }

    const auto t0 = std::chrono::high_resolution_clock::now();
    process(options);
    const auto t1 = std::chrono::high_resolution_clock::now();

    std::cout << "\nTime taken: "
              << std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count() / 1000.0
              << "s" << std::endl;

    write_results(options.conclusive);

    return 0;
}
