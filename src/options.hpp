#pragma once

#include <string>

using map_fens = std::unordered_map<std::string, std::pair<int, int>>;

struct CLIOptions {
    map_fens fixfens;
    std::string match_book;
    std::string dir        = "./pgns";
    int concurrency        = 1;
    bool conclusive        = false;
    bool only_sprt         = false;
    bool allow_duplicates  = false;
    bool matchBookInverted = false;
};
