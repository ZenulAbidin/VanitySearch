#include "Regex.h"

const int num_strings = 10;

bool Match(const std::string& text, MRegexp* re) {
  MRegexpMatch m;
  return mregexp_match(re, text.c_str(), &m);
}

long long BenchmarkRegex(const std::string& regex, int length, int milliseconds) {
  // Compile the regular expression
  MRegexpMatch m;
  MRegexp* re = mregexp_compile(regex.c_str());
  if (!re) {
    printf("Fatal: Regex failed to compile.\n");
    exit(1);
  }

  // Generate a vector of random strings
  std::vector<std::string> strings;
  std::random_device rd;
  std::mt19937 generator(rd());
  std::uniform_int_distribution<> distribution(0, 9);
  for (int i = 0; i < num_strings; ++i) {
    std::string str;
    str.reserve(length);
    for (int j = 0; j < length; ++j) {
      str.push_back(distribution(generator) + '0');
    }
    strings.emplace_back(std::move(str));
  }

  // Start the timer
  auto start_time = std::chrono::system_clock::now();

  // Try to match the regular expression to each of the strings
  long long num_matches = 0;
  while (true) {
      for (const auto& str : strings) {
        mregexp_match(re, str.c_str(), &m);
        ++num_matches;
        if (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now() - start_time).count() > milliseconds) {
            // Stop the timer if the specified number of milliseconds has elapsed
            // Free the resources and return the number of matches
            mregexp_free(re);
            return num_matches;
        }
      }
  }

}

long long BenchmarkStringComparisons(int length, int milliseconds) {
  // Generate a vector of random strings
  std::vector<std::string> strings;
  std::random_device rd;
  std::mt19937 generator(rd());
  std::uniform_int_distribution<> distribution(0, 9);
  for (int i = 0; i < num_strings; ++i) {
    std::string str;
    str.reserve(length);
    for (int j = 0; j < length; ++j) {
      str.push_back(distribution(generator) + '0');
    }
    strings.emplace_back(std::move(str));
  }

  // Start the timer
  auto start_time = std::chrono::system_clock::now();

  // Compare each pair of strings
  long long num_comparisons = 0;
  while (true) {
      for (int i = 0; i < num_strings; ++i) {
        for (int j = i + 1; j < num_strings; ++j) {
          strings[i] == strings[j];
          ++num_comparisons;
          if (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now() - start_time).count() > milliseconds) {
            // Stop the timer if the specified number of milliseconds has elapsed
            // Return the number of comparisons
            return num_comparisons;
          }
        }
      }
  }

}


double RegexDifficultyAdjustment(const std::string& regex, int length, int milliseconds) {
    return static_cast<double>(BenchmarkRegex(regex, length, milliseconds)) /
        static_cast<double>(BenchmarkStringComparisons(length, milliseconds));
}
