#pragma once

#include <cstdint>
#include <string>

namespace rocketdb {

class Slice;
class WritbaleFile;

void AppendNumberTo(std::string* str, uint64_t num);

// Append human-readable numbers to strings    number --> string
std::string NumberToString(uint64_t num);

// Extract human-readable numbers from the slice   string --> number 
bool ConsumeDecimalNumber(Slice* in, uint64_t* val);

// Return a human-readable version of value
// Escapes any non-printbale characters found in value
void AppendEscapedStringTo(std::string* str, const Slice& value);

std::string EscapeString(const Slice& value);

}