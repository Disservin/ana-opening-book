#pragma once

#include <regex>
#include <string>
#include <unordered_map>
#include <utility>

using map_fens = std::unordered_map<std::string, std::pair<int, int>>;

struct CLIOptions {
    map_fens fixfens;
    std::string match_book;
    std::string match_fen;
    std::regex regex_fen;
    std::string pgn_output;
    std::string dir        = "./pgns";
    int concurrency        = 1;
    bool conclusive        = false;
    bool only_sprt         = false;
    bool allow_duplicates  = false;
    bool matchBookInverted = false;
};
