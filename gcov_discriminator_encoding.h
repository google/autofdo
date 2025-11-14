// Discriminator encoding utilities for AutoFDO
// Similar to LLVM's discriminator encoding scheme
//
// Discriminator format: [Base:8][Multiplicity:7][CopyID:11][Unused:6]
// - Base discriminator (bits 0-7): Distinguishes instructions at same line
// - Multiplicity (bits 8-14): Duplication factor for unrolling/vectorization  
// - CopyID (bits 15-25): Unique identifier for code copies
// - Unused (bits 26-31): Reserved

#ifndef AUTOFDO_DISCRIMINATOR_ENCODING_H_
#define AUTOFDO_DISCRIMINATOR_ENCODING_H_

#include <cstdint>

namespace devtools_crosstool_autofdo {

// Extract base discriminator (bits 0-7)
inline uint32_t GetBaseDiscriminator(uint32_t discriminator) {
  return discriminator & 0xFF;
}

// Extract multiplicity/duplication factor (bits 8-14)
// Returns 1 if multiplicity bits are 0 (no duplication)
inline uint32_t GetMultiplicity(uint32_t discriminator) {
  uint32_t mult = (discriminator >> 8) & 0x7F;
  return (mult == 0) ? 1 : mult;
}

// Extract copy ID (bits 15-25)
inline uint32_t GetCopyID(uint32_t discriminator) {
  return (discriminator >> 15) & 0x7FF;
}

// Encode discriminator from components
inline uint32_t EncodeDiscriminator(uint32_t base, uint32_t multiplicity,
                                     uint32_t copy_id) {
  // Validate ranges
  if (base > 0xFF || multiplicity > 127 || copy_id > 0x7FF) {
    return base;  // Fallback to just base if encoding fails
  }
  return base | (multiplicity << 8) | (copy_id << 15);
}

// Check if discriminator has multiplicity encoded
inline bool HasMultiplicity(uint32_t discriminator) {
  return GetMultiplicity(discriminator) > 1;
}

// Check if discriminator has copy ID encoded
inline bool HasCopyID(uint32_t discriminator) {
  return GetCopyID(discriminator) != 0;
}

}  // namespace devtools_crosstool_autofdo

#endif  // AUTOFDO_DISCRIMINATOR_ENCODING_H_

