#pragma once

#include <string>

struct CLIOptions {
  std::string path = "./pgns";
  std::string match_book;
  int concurrency = 1;
  bool conclusive = false;
  bool allow_duplicates = false;
  bool matchBookInverted = true;
};
