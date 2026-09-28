// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Summon Software Labs.

#include <cstring>

#include "energy_ledger/digest.hpp"

namespace energy_ledger {
namespace {

constexpr std::uint32_t kRoundConstants[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
    0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
    0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
    0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
    0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
    0xc67178f2u};

inline std::uint32_t rotr(std::uint32_t value, int count) noexcept {
  return (value >> count) | (value << (32 - count));
}

void compress_block(std::uint32_t state[8], const std::uint8_t* block) noexcept {
  std::uint32_t schedule[64];
  for (int index = 0; index < 16; ++index) {
    const auto* chunk = block + (index * 4);
    schedule[index] = (static_cast<std::uint32_t>(chunk[0]) << 24) |
                      (static_cast<std::uint32_t>(chunk[1]) << 16) |
                      (static_cast<std::uint32_t>(chunk[2]) << 8) |
                      static_cast<std::uint32_t>(chunk[3]);
  }
  for (int index = 16; index < 64; ++index) {
    const std::uint32_t s0 = rotr(schedule[index - 15], 7) ^ rotr(schedule[index - 15], 18) ^
                             (schedule[index - 15] >> 3);
    const std::uint32_t s1 =
        rotr(schedule[index - 2], 17) ^ rotr(schedule[index - 2], 19) ^ (schedule[index - 2] >> 10);
    schedule[index] = schedule[index - 16] + s0 + schedule[index - 7] + s1;
  }

  std::uint32_t a = state[0];
  std::uint32_t b = state[1];
  std::uint32_t c = state[2];
  std::uint32_t d = state[3];
  std::uint32_t e = state[4];
  std::uint32_t f = state[5];
  std::uint32_t g = state[6];
  std::uint32_t h = state[7];

  for (int index = 0; index < 64; ++index) {
    const std::uint32_t big_s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
    const std::uint32_t choose = (e & f) ^ (~e & g);
    const std::uint32_t temp1 = h + big_s1 + choose + kRoundConstants[index] + schedule[index];
    const std::uint32_t big_s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
    const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
    const std::uint32_t temp2 = big_s0 + majority;
    h = g;
    g = f;
    f = e;
    e = d + temp1;
    d = c;
    c = b;
    b = a;
    a = temp1 + temp2;
  }

  state[0] += a;
  state[1] += b;
  state[2] += c;
  state[3] += d;
  state[4] += e;
  state[5] += f;
  state[6] += g;
  state[7] += h;
}

}  // namespace

Sha256::Sha256() noexcept
    : state_{0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au, 0x510e527fu, 0x9b05688cu,
             0x1f83d9abu, 0x5be0cd19u},
      buffer_{},
      total_bytes_(0),
      buffer_used_(0),
      finalized_(false),
      digest_() {}

void Sha256::update(const void* data, std::size_t length) noexcept {
  if (finalized_ || length == 0) {
    return;
  }
  const auto* bytes = static_cast<const std::uint8_t*>(data);
  total_bytes_ += static_cast<std::uint64_t>(length);
  std::size_t offset = 0;
  if (buffer_used_ > 0) {
    while (buffer_used_ < 64 && offset < length) {
      buffer_[buffer_used_++] = bytes[offset++];
    }
    if (buffer_used_ == 64) {
      compress_block(state_, buffer_);
      buffer_used_ = 0;
    }
  }
  while (length - offset >= 64) {
    compress_block(state_, bytes + offset);
    offset += 64;
  }
  while (offset < length) {
    buffer_[buffer_used_++] = bytes[offset++];
  }
}

Digest256 Sha256::finish() noexcept {
  if (finalized_) {
    return digest_;
  }

  std::uint32_t state[8];
  std::memcpy(state, state_, sizeof(state));

  std::uint8_t block[64];
  std::memcpy(block, buffer_, buffer_used_);
  std::size_t used = buffer_used_;
  const std::uint64_t total = total_bytes_;

  block[used++] = 0x80u;
  if (used > 56) {
    while (used < 64) {
      block[used++] = 0u;
    }
    compress_block(state, block);
    used = 0;
  }
  while (used < 56) {
    block[used++] = 0u;
  }
  const std::uint64_t bits = total * 8u;
  for (int index = 0; index < 8; ++index) {
    block[56 + index] = static_cast<std::uint8_t>((bits >> (56 - (8 * index))) & 0xFFu);
  }
  compress_block(state, block);

  Digest256::Storage storage{};
  for (int index = 0; index < 8; ++index) {
    storage[static_cast<std::size_t>(index) * 4 + 0] =
        static_cast<std::uint8_t>((state[index] >> 24) & 0xFFu);
    storage[static_cast<std::size_t>(index) * 4 + 1] =
        static_cast<std::uint8_t>((state[index] >> 16) & 0xFFu);
    storage[static_cast<std::size_t>(index) * 4 + 2] =
        static_cast<std::uint8_t>((state[index] >> 8) & 0xFFu);
    storage[static_cast<std::size_t>(index) * 4 + 3] =
        static_cast<std::uint8_t>(state[index] & 0xFFu);
  }

  digest_ = Digest256(storage);
  finalized_ = true;
  return digest_;
}

Digest256 Sha256::hash(const void* data, std::size_t length) noexcept {
  Sha256 hasher;
  hasher.update(data, length);
  return hasher.finish();
}

Digest256 Sha256::hash_domain(std::string_view domain, const void* data,
                              std::size_t length) noexcept {
  Sha256 hasher;
  hasher.update(domain.data(), domain.size());
  const std::uint8_t separator = 0u;
  hasher.update(&separator, 1);
  hasher.update(data, length);
  return hasher.finish();
}

}  // namespace energy_ledger
