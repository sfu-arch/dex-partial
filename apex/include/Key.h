#if !defined(_APEX_KEY_H_)
#define _APEX_KEY_H_

#include "Common.h"
#include <string>
#include <cstring>

// ─── Key Conversion Utilities ──────────────────────────────────────
// APEX uses Key = uint64_t (8 bytes), simplifying all key operations
// compared to CHIME's std::array<uint8_t, 8> representation.

inline Key int2key(uint64_t v) {
  return v;
}

inline Key str2key(const std::string& s) {
  Key k = 0;
  // Pack up to 8 bytes of the string into the key
  size_t len = std::min(s.size(), sizeof(Key));
  memcpy(&k, s.data(), len);
  return k;
}

inline uint64_t key2int(const Key& k) {
  return k;
}

inline std::string key2str(const Key& k) {
  char buf[9] = {};
  memcpy(buf, &k, sizeof(Key));
  return std::string(buf, sizeof(Key));
}

// ─── Prefix / Partial Helpers ──────────────────────────────────────

// Get the byte at a given depth (0 = MSB, 7 = LSB)
inline uint8_t get_partial(const Key& key, int depth) {
  if (depth == 0) return 0;
  return (uint8_t)((key >> (64 - depth * 8)) & 0xFF);
}

// Extract the suffix (lower bytes after prefix consumption)
inline uint32_t extract_suffix(const Key& key, int prefix_depth) {
  return (uint32_t)(key & 0xFFFFFFFF);  // lower 4 bytes as suffix
}

// Get byte at position i (big-endian: byte 0 = MSB)
inline uint8_t key_byte_at(const Key& key, int i) {
  return (uint8_t)((key >> (56 - i * 8)) & 0xFF);
}

// Convert key to a byte array (big-endian)
inline void key_to_bytes(const Key& key, uint8_t* out) {
  for (int i = 0; i < 8; i++) {
    out[i] = key_byte_at(key, i);
  }
}

// Convert byte array back to key (big-endian)
inline Key bytes_to_key(const uint8_t* bytes) {
  Key k = 0;
  for (int i = 0; i < 8; i++) {
    k |= ((uint64_t)bytes[i]) << (56 - i * 8);
  }
  return k;
}

#endif // _APEX_KEY_H_
