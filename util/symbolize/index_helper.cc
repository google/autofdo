
#include "index_helper.h"

namespace devtools_crosstool_autofdo {


const char* get_string_with_offsets(const char* string_buffer,
                                    uint64 string_buffer_length,
                                    uint64 offset) {
  if (offset >= string_buffer_length) {
    LOG(WARNING) << "offset is out of range.  offset=" << offset
                  << " string_buffer_length_=" << string_buffer_length;
    return NULL;
  }
  return string_buffer + offset;
}

const char* get_offset_pointer_in_str_offsets_table(uint64 str_index,
                                              const char* str_offset_buffer,
                                              uint64 str_offset_base,
                                              uint8 offset_size, 
                                              uint16 version){
  const char* offset_ptr =
      str_offset_buffer + str_offset_base + (str_index * offset_size);

  // In DWARF5 we have to include base_address to the calculation. 
  if (version == 5)
    offset_ptr =
      str_offset_buffer + str_offset_base + (str_index * offset_size);
  else if (version < 5)
    offset_ptr = 
      str_offset_buffer + str_index * offset_size;

  return offset_ptr;
}

const char* get_address_pointer_from_address_table(const char* addr_buffer, 
                                                    uint64 addr_base,
                                                    uint64 addr_index,
                                                    uint8 addr_size){
  return addr_buffer + addr_base + addr_index * addr_size;
}

}  // namespace devtools_crosstool_autofdo