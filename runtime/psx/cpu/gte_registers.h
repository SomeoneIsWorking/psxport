// gte_registers.h — GTE (COP2) register numbers and the command words native bodies issue.
#pragma once

#include <cstdint>

namespace psx::gte {

// Data registers.
inline constexpr std::uint32_t kVxy0 = 0;
inline constexpr std::uint32_t kVz0 = 1;
inline constexpr std::uint32_t kVxy1 = 2;
inline constexpr std::uint32_t kVz1 = 3;
inline constexpr std::uint32_t kVxy2 = 4;
inline constexpr std::uint32_t kVz2 = 5;
inline constexpr std::uint32_t kRgbc = 6;
inline constexpr std::uint32_t kOtz = 7;
inline constexpr std::uint32_t kIr0 = 8;
inline constexpr std::uint32_t kIr1 = 9;
inline constexpr std::uint32_t kIr2 = 10;
inline constexpr std::uint32_t kIr3 = 11;
inline constexpr std::uint32_t kSxy0 = 12;
inline constexpr std::uint32_t kSxy1 = 13;
inline constexpr std::uint32_t kSxy2 = 14;
inline constexpr std::uint32_t kSz0 = 16;
inline constexpr std::uint32_t kSz1 = 17;
inline constexpr std::uint32_t kSz3 = 19;
inline constexpr std::uint32_t kRgb2 = 22;
inline constexpr std::uint32_t kMac0 = 24;
inline constexpr std::uint32_t kMac1 = 25;
inline constexpr std::uint32_t kMac2 = 26;
inline constexpr std::uint32_t kMac3 = 27;

// Control registers: 0..4 the rotation matrix, 5..7 the translation.
inline constexpr std::uint32_t kRotation = 0;
inline constexpr std::uint32_t kOfx = 24; // 16.16 screen offset
inline constexpr std::uint32_t kOfy = 25;
inline constexpr std::uint32_t kH = 26; // projection plane distance
inline constexpr std::uint32_t kDqa = 27;
inline constexpr std::uint32_t kDqb = 28;
inline constexpr std::uint32_t kFlag = 31; // bit 31 is the error summary

// Commands as the guests encode them.
inline constexpr std::uint32_t kRtps = 0x4A180001u; // sf=1 lm=0
inline constexpr std::uint32_t kRtpt = 0x4A280030u;
inline constexpr std::uint32_t kNclip = 0x4B400006u;
inline constexpr std::uint32_t kAvsz3 = 0x4B58002Du;
inline constexpr std::uint32_t kAvsz4 = 0x4B68002Eu;
inline constexpr std::uint32_t kDpcs = 0x4A780010u;
inline constexpr std::uint32_t kDcpl = 0x4A680029u;
inline constexpr std::uint32_t kCc = 0x4B38041Cu; // lm=1

} // namespace psx::gte
