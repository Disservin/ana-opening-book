#include <iostream>
#include <fstream>
#include <mutex>
#include <regex>
#include <set>
#include <sstream>
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
    std::size_t wins   = 0;
    std::size_t draws  = 0;
    std::size_t losses = 0;

    double draw_rate() const { return double(draws) / total(); }

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

    std::size_t total() const { return wins + draws + losses; }
};

using map_t = phmap::parallel_flat_hash_map<
    std::string, Statistics, std::hash<std::string>, std::equal_to<std::string>,
    std::allocator<std::pair<const std::string, Statistics>>, 8, std::mutex>;
using map_meta = std::unordered_map<std::string, TestMetaData>;

map_t occurance_map                   = {};
std::atomic<std::size_t> total_chunks = 0;
std::atomic<std::size_t> total_games  = 0;

std::mutex pgn_out_mutex;
std::ofstream pgn_out_stream;

class Analyzer : public pgn::Visitor {
   public:
    Analyzer(std::string_view file, const CLIOptions &options) : file(file), options(options) {}
    virtual ~Analyzer(){};

    // reset
    void startPgn() override {
        result     = Result::UNKNOWN;
        fen        = chess::constants::STARTPOS;
        valid_game = true;
        fen_match  = false;

        if (!options.match_fen.empty()) {
            headers.clear();
            moves.clear();
        }
    }

    void header(std::string_view key, std::string_view value) override {
        if (key == "Result") {
            if (value == "1-0") {
                result = Result::WIN;
            } else if (value == "0-1") {
                result = Result::LOSS;
            } else if (value == "1/2-1/2") {
                result = Result::DRAW;
            }
        } else if (key == "FEN") {
            fen = value;
        } else if (key == "Termination") {
            if (value == "time forfeit" || value == "abandoned" || value == "stalled connection" ||
                value == "illegal move" || value == "unterminated") {
                valid_game = false;
            }
        }
        if (!options.match_fen.empty()) {
            headers.emplace_back(key, value);
        }
    }

    void startMoves() override {
        skipPgn(true);

        if (result == Result::UNKNOWN || !valid_game) return;

        if (!options.match_fen.empty() && std::regex_match(fen, options.regex_fen)) {
            fen_match = true;
            skipPgn(false);
        }

        const auto fixed_fen = fixFen(fen);

        occurance_map.lazy_emplace_l(
            fixed_fen,
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
                ctor(fixed_fen, Statistics{result == Result::WIN, result == Result::DRAW,
                                           result == Result::LOSS});
            });

        total_games++;
    }

    void move(std::string_view san, std::string_view comment) override {
        if (fen_match) moves.emplace_back(san, comment);
    }

    void endPgn() override {
        if (fen_match) {
            const std::lock_guard<std::mutex> lock(pgn_out_mutex);

            for (const auto &[k, v] : headers) {
                pgn_out_stream << "[" << k << " \"" << v << "\"]\n";
            }
            pgn_out_stream << "\n";

            std::istringstream iss(fen);
            std::string board, stm;
            iss >> board >> stm;
            bool wtm     = (stm == "w");
            int fm       = 1;
            int line_len = 0;

            for (const auto &[san, comment] : moves) {
                if (!san.empty()) {
                    if (wtm) {
                        const std::string s = std::to_string(fm) + ". ";
                        pgn_out_stream << s;
                        line_len += s.length();
                    } else if (fm == 1) {
                        pgn_out_stream << "1... ";
                        line_len += 5;
                    }

                    pgn_out_stream << san << " ";
                    line_len += san.length() + 1;
                }
                if (!comment.empty()) {
                    pgn_out_stream << "{" << comment << "} ";
                    line_len += comment.length() + 3;
                }

                if (line_len > 70) {
                    pgn_out_stream << "\n";
                    line_len = 0;
                }

                if (!wtm) fm++;
                wtm = !wtm;
            }

            if (result == Result::WIN) {
                pgn_out_stream << "1-0";
            } else if (result == Result::LOSS) {
                pgn_out_stream << "0-1";
            } else if (result == Result::DRAW) {
                pgn_out_stream << "1/2-1/2";
            } else pgn_out_stream << "*";

            pgn_out_stream << "\n\n";
        }
    }


   private:
    std::string fixFen(std::string_view fen_view) {
        std::regex p("^(.+) 0 1$");
        std::smatch match;
        std::string value_str(fen_view);

        // revert changes by cutechess-cli to move counters
        if (!options.fixfens.empty() && std::regex_search(value_str, match, p) &&
            match.size() > 1) {
            std::string fen = match[1];
            auto it         = options.fixfens.find(fen);

            if (it == options.fixfens.end()) {
                std::cerr << "Could not find FEN \"" << fen << "\" in fixFENsource." << std::endl;
                std::cerr << "The FEN is from the file " << file << std::endl;
                return value_str;
            }

            const auto &fix = it->second;
            std::string fixed_value =
                fen + " " + std::to_string(fix.first) + " " + std::to_string(fix.second);
            return fixed_value;
        }

        return value_str;
    }
    std::string_view file;
    Result result = Result::UNKNOWN;
    std::string fen;
    bool valid_game = true;
    bool fen_match  = false;
    std::vector<std::pair<std::string, std::string>> headers;
    std::vector<std::pair<std::string, std::string>> moves;
    const CLIOptions &options;
};

[[nodiscard]] map_meta get_metadata(const std::vector<std::string> &file_list,
                                    bool allow_duplicates) {
    map_meta meta_map;
    std::unordered_map<std::string, std::string> test_map;  // map to check for duplicate tests
    std::set<std::string> test_warned;
    for (const auto &pathname : file_list) {
        fs::path path(pathname);
        std::string filename      = path.filename().string();
        std::string test_id       = filename.substr(0, filename.find_first_of("-."));
        std::string test_filename = (path.parent_path() / test_id).string();

        if (test_map.find(test_id) == test_map.end()) {
            test_map[test_id] = test_filename;
        } else if (test_map[test_id] != test_filename) {
            if (test_warned.find(test_filename) == test_warned.end()) {
                std::cout << (allow_duplicates ? "Warning" : "Error")
                          << ": Detected a duplicate of test " << test_id << " in directory "
                          << path.parent_path().string() << std::endl;
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
        fs::path path(pathname);
        std::string filename      = path.filename().string();
        std::string test_id       = filename.substr(0, filename.find_first_of("-."));
        std::string test_filename = (path.parent_path() / test_id).string();
      
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
        fs::path path(pathname);
        std::string filename      = path.filename().string();
        std::string test_id       = filename.substr(0, filename.find_first_of("-."));
        std::string test_filename = (path.parent_path() / test_id).string();

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
            auto vis = std::make_unique<Analyzer>(file, options);

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

[[nodiscard]] map_fens get_fixfen(std::string file) {
    map_fens fixfen_map;
    if (file.empty()) {
        return fixfen_map;
    }

    const auto fen_iterator = [&](std::istream &iss) {
        std::string line;
        while (std::getline(iss, line)) {
            std::istringstream iss(line);
            std::string f1, f2, f3, ep;
            int halfmove, fullmove = 0;

            iss >> f1 >> f2 >> f3 >> ep >> halfmove >> fullmove;

            if (!fullmove) continue;

            auto key         = f1 + ' ' + f2 + ' ' + f3 + ' ' + ep;
            auto fixfen_data = std::pair<int, int>(halfmove, fullmove);

            if (fixfen_map.find(key) != fixfen_map.end()) {
                // for duplicate FENs, prefer the one with lower full move counter
                if (fullmove < fixfen_map[key].second) {
                    fixfen_map[key] = fixfen_data;
                }
            } else {
                fixfen_map[key] = fixfen_data;
            }
        }
    };

    if (file.size() >= 3 && file.substr(file.size() - 3) == ".gz") {
        igzstream input(file.c_str());
        fen_iterator(input);
    } else {
        std::ifstream input(file);
        fen_iterator(input);
    }

    return fixfen_map;
}

void process(const CLIOptions &options) {
    int target_chunks = 4 * options.concurrency;

    auto files_pgn = get_files(options.dir, true);

    const auto meta_map = get_metadata(files_pgn, options.allow_duplicates);

    if (!options.match_book.empty()) {
        std::regex regex(options.match_book);
        filter_files_book(files_pgn, meta_map, regex, options.matchBookInverted);
    }

    if (options.only_sprt) {
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

void write_results() {
    std::ofstream out("results.csv");

    out << "FEN, Wins, Draws, Losses\n";

    std::vector<std::pair<std::string, Statistics>> sorted_map(occurance_map.begin(),
                                                               occurance_map.end());

    std::sort(sorted_map.begin(), sorted_map.end(),
              [](const auto &a, const auto &b) { return a.second < b.second; });

    std::size_t wins   = 0;
    std::size_t draws  = 0;
    std::size_t losses = 0;

    for (const auto &[fen, stats] : sorted_map) {
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
/// [--allowDuplicates] [--SPRTonly] [--matchBookInvert] [--fixFENsource file]
/// [--matchFEN fen] [--pgnOutput file]
/// @param argc
/// @param argv
/// @return
int main(int argc, char const *argv[]) {
    CommandLine cmd(argc, argv);

    CLIOptions options;

    if (cmd.has("--dir")) {
        options.dir = cmd.get("--dir");
        std::cout << "Looking (recursively) for pgn files in " << options.dir << std::endl;
    }

    if (cmd.has("--concurrency")) {
        options.concurrency = std::stoi(cmd.get("--concurrency"));
    } else {
        options.concurrency = std::max(1, int(std::thread::hardware_concurrency()));
    }
    std::cout << "Files will be processed with concurrency " << options.concurrency << std::endl;

    if (cmd.has("--matchBook")) {
        options.match_book = cmd.get("--matchBook");

        if (options.match_book.empty()) {
            std::cerr << "Error: --matchBook cannot be empty" << std::endl;
            return 1;
        }

        if (cmd.has("--matchBookInvert")) {
            options.matchBookInverted = true;
        }
        std::cout << "Filtering pgn files " << (options.matchBookInverted ? "not " : "")
                  << "matching the book name " << options.match_book << std::endl;
    }

    if (cmd.has("--allowDuplicates")) {
        options.allow_duplicates = true;
        std::cout << "Allow duplicate tests during the analysis." << std::endl;
    }

    if (cmd.has("--SPRTonly")) {
        options.only_sprt = true;
        std::cout << "Only analyse games that are part of a SPRT test" << std::endl;
    }

    if (cmd.has("--fixFENsource")) {
        auto file       = cmd.get("--fixFENsource");
        options.fixfens = get_fixfen(file);
        std::cout << "Read in move counters to possibly fix FENs from " << file << std::endl;
    }

    if (cmd.has("--pgnOutput"))
        options.pgn_output = cmd.get("--pgnOutput");
    else
        options.pgn_output = "match_fen.pgn";

    if (cmd.has("--matchFEN")) {
        options.match_fen = cmd.get("--matchFEN");

        if (options.match_fen.empty()) {
            std::cerr << "Error: --matchFEN cannot be empty" << std::endl;
            return 1;
        }

        // For example:
        // --matchFEN "(r2qkb1r/1pp2ppp/p1np1n2/3Pp3/4P3/2NBBQ2/PPP2PPP/R3K2R b KQkq.*|r2q1rk1/ppp1bppp/2np1n2/4p3/2B1P1b1/BPN2NP1/P1PP1P1P/R2QK2R w KQ.*)"
        options.regex_fen = std::regex(options.match_fen);

        std::cout << "Save to " << options.pgn_output 
                  << " all the games with FEN tag matching " 
                  << options.match_fen << std::endl;

        pgn_out_stream.open(options.pgn_output);

        if (!pgn_out_stream.is_open()) {
            std::cerr << "Error: Failed to open output file: " << options.pgn_output
                      << std::endl;
            return 1;
        }
    }

    const auto t0 = std::chrono::high_resolution_clock::now();
    process(options);
    const auto t1 = std::chrono::high_resolution_clock::now();

    std::cout << "\nTime taken: "
              << std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count() / 1000.0
              << "s" << std::endl;

    write_results();

    if (pgn_out_stream.is_open()) {
        pgn_out_stream.close();
        std::cout << "Wrote matching games to " << options.pgn_output << std::endl;
    }

    return 0;
}
