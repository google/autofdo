

#ifndef AUTOFDO_SYMBOLIZE_INDEX_HELPER_H__
#define AUTOFDO_SYMBOLIZE_INDEX_HELPER_H__

#include "base/common.h"


namespace devtools_crosstool_autofdo {


// string_buffer : as pointer to begining of .debug_str
// string_buffer_length : total of the string_buffer (.debug_str) 
// offset : offset of the string we want
// return a pointer pointing at the begining of that string.
const char* get_string_with_offsets(const char* string_buffer,
                                    uint64 string_buffer_length,
                                    uint64 offset);


//  str_index : index of the string we want
//  str_offset_buffer : as pointer to begining of .debug_str_offsets
//  str_offset_base : base address of the .debug_str_offsets section
//  offset_size : the size of offsets in architecture/version
//  version : version of header in .debug_str_offsets
// return a pointer to memory location, containing offset for the string with index str_index.
const char* get_offset_pointer_in_str_offsets_table(uint64 str_index,
                                                    const char* str_offset_buffer,
                                                    uint64 str_offset_base,
                                                    uint8 offset_size, 
                                                    uint16 version);

//  addr_buffer : pointer to .debug_addr buffer.
//  addr_base : base address of the .debug_addr section.
//  addr_index : the index of the address we want
//  addr_size : size of an andress in this architecture/version
// return a pointer to memory location, containing the address we want.
const char* get_address_pointer_from_address_table(const char* addr_buffer, 
                                                    uint64 addr_base,
                                                    uint64 addr_index,
                                                    uint8 addr_size);

}  // namespace devtools_crosstool_autofdo

#endif  // AUTOFDO_SYMBOLIZE_INDEX_HELPER_H__
