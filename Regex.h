#ifndef REGEX_H
#define REGEX_H

#include <chrono>
#include "mregexp.h"
#include <random>
#include <string>
#include <vector>

long long BenchmarkRegex(const std::string& regex, int length, int milliseconds);
long long BenchmarkStringComparisons(int length, int milliseconds);
double RegexDifficultyAdjustment(const std::string& regex, int length, int milliseconds);
bool Match(const std::string& text, MRegexp* re);
#endif /* REGEX_H */
