#pragma once

#include <string>

struct CLIOptions {
    std::string match_book;
    std::string dir        = "./pgns";
    int concurrency        = 1;
    bool conclusive        = false;
    bool only_sprt         = false;
    bool allow_duplicates  = false;
    bool matchBookInverted = true;
};
