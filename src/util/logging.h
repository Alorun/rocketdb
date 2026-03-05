#pragma once

#include <cstdint>
#include <string>

namespace rocketdb {

class Slice;
class WritbaleFile;

// Convert numbers to strings
void AppendNumberTo(std::string* str, uint64_t num);

// Convert byte stream to string 
void AppendEscapedStringTo(std::string* str, const Slice& value);

std::string NumberToString(uint64_t num);

std::string EscapeString(const Slice& value);

// Extract the digits from the slice
bool ConsumeDecimalNumber(Slice* in, uint64_t* val);

}