//===-- Support/ELF.h - ELF constants and data structures -------*- C++ -*-===//
// 
//                     The LLVM Compiler Infrastructure
//
// This file was developed by the LLVM research group and is distributed under
// the University of Illinois Open Source License. See LICENSE.TXT for details.
// 
//===----------------------------------------------------------------------===//
//
// This header contains common, non-processor-specific data structures and
// constants for the ELF file format.
// 
// The details of this file are largely based on the Tool Interface Standard
// (TIS) Executable and Linking Format (ELF) Specification Version 1.2,
// May 1995.
//
//===----------------------------------------------------------------------===//

#include "Support/DataTypes.h"
#include <cstring>
#include <cstdlib>

namespace llvm {

namespace ELF {

typedef uint32_t Elf32_Addr; // Program address
typedef uint16_t Elf32_Half;
typedef uint32_t Elf32_Off;  // File offset
typedef int32_t  Elf32_Sword;
typedef uint32_t Elf32_Word;

// Object file magic string.
static const char *ElfMagic = "\x7fELF";

struct Elf32_Ehdr {
  unsigned char e_ident[16]; // ELF Identification bytes
  Elf32_Half    e_type;      // Type of file (see ET_* below)
  Elf32_Half    e_machine;   // Required architecture for this file (see EM_*)
  Elf32_Word    e_version;   // Must be equal to 1
  Elf32_Addr    e_entry;     // Address to jump to in order to start program
  Elf32_Off     e_phoff;     // Program header table's file offset, in bytes
  Elf32_Off     e_shoff;     // Section header table's file offset, in bytes
  Elf32_Word    e_flags;     // Processor-specific flags
  Elf32_Half    e_ehsize;    // Size of ELF header, in bytes
  Elf32_Half    e_phentsize; // Size of an entry in the program header table
  Elf32_Half    e_phnum;     // Number of entries in the program header table
  Elf32_Half    e_shentsize; // Size of an entry in the section header table
  Elf32_Half    e_shnum;     // Number of entries in the section header table
  Elf32_Half    e_shstrndx;  // Sect hdr table index of sect name string table
  bool checkMagic () const {
    return (memcmp (e_ident, ElfMagic, strlen (ElfMagic)) == 0;
  }
  unsigned char getFileClass () const { return e_ident[4]; }
  unsigned char getDataEncoding () { return e_ident[5]; }
};

// File types
enum {
  ET_NONE   = 0,      // No file type
  ET_REL    = 1,      // Relocatable file
  ET_EXEC   = 2,      // Executable file
  ET_DYN    = 3,      // Shared object file
  ET_CORE   = 4,      // Core file
  ET_LOPROC = 0xff00, // Beginning of processor-specific codes
  ET_HIPROC = 0xffff  // Processor-specific
};

// Machine architectures
enum {
  EM_NONE = 0,  // No machine
  EM_M32 = 1,   // AT&T WE 32100
  EM_SPARC = 2, // SPARC
  EM_386 = 3,   // Intel 386
  EM_68K = 4,   // Motorola 68000
  EM_88K = 5,   // Motorola 88000
  EM_860 = 7,   // Intel 80860
  EM_MIPS = 8   // MIPS R3000
};

// Object file classes.
enum {
  ELFCLASS32 = 1, // 32-bit object file
  ELFCLASS64 = 2  // 64-bit object file
};

// Object file byte orderings.
enum {
  ELFDATA2LSB = 1, // Little-endian object file
  ELFDATA2MSB = 2  // Big-endian object file
};

// Section header.
struct Elf32_Shdr {
  Elf32_Word sh_name;      // Section name (index into string table)
  Elf32_Word sh_type;      // Section type (SHT_*)
  Elf32_Word sh_flags;     // Section flags (SHF_*)
  Elf32_Addr sh_addr;      // Address where section is to be loaded
  Elf32_Off  sh_offset;    // File offset of section data, in bytes
  Elf32_Word sh_size;      // Size of section, in bytes
  Elf32_Word sh_link;      // Section type-specific header table index link 
  Elf32_Word sh_info;      // Section type-specific extra information
  Elf32_Word sh_addralign; // Section address alignment
  Elf32_Word sh_entsize;   // Size of records contained within the section
};

// Special section indices.
enum {
  SHN_UNDEF     = 0,      // Undefined, missing, irrelevant, or meaningless
  SHN_LORESERVE = 0xff00, // Lowest reserved index
  SHN_LOPROC    = 0xff00, // Lowest processor-specific index
  SHN_HIPROC    = 0xff1f, // Highest processor-specific index
  SHN_ABS       = 0xfff1, // Symbol has absolute value; does not need relocation
  SHN_COMMON    = 0xfff2, // FORTRAN COMMON or C external global variables
  SHN_HIRESERVE = 0xffff  // Highest reserved index
};

// Section types.
enum {
  SHT_NULL     = 0,  // No associated section (inactive entry).
  SHT_PROGBITS = 1,  // Program-defined contents.
  SHT_SYMTAB   = 2,  // Symbol table.
  SHT_STRTAB   = 3,  // String table.
  SHT_RELA     = 4,  // Relocation entries; explicit addends.
  SHT_HASH     = 5,  // Symbol hash table.
  SHT_DYNAMIC  = 6,  // Information for dynamic linking.
  SHT_NOTE     = 7,  // Information about the file.
  SHT_NOBITS   = 8,  // Data occupies no space in the file.
  SHT_REL      = 9,  // Relocation entries; no explicit addends.
  SHT_SHLIB    = 10, // Reserved.
  SHT_DYNSYM   = 11, // Symbol table.
  SHT_LOPROC   = 0x70000000, // Lowest processor architecture-specific type.
  SHT_HIPROC   = 0x7fffffff, // Highest processor architecture-specific type.
  SHT_LOUSER   = 0x80000000, // Lowest type reserved for applications.
  SHT_HIUSER   = 0xffffffff  // Highest type reserved for applications.
};

// Section flags.
enum {
  SHF_WRITE     = 0x1, // Section data should be writable during execution.
  SHF_ALLOC     = 0x2, // Section occupies memory during program execution.
  SHF_EXECINSTR = 0x4, // Section contains executable machine instructions.
  SHF_MASKPROC  = 0xf0000000 // Bits indicating processor-specific flags.
};

// Symbol table entries.
struct Elf32_Sym {
  Elf32_Word    st_name;  // Symbol name (index into string table)
  Elf32_Addr    st_value; // Value or address associated with the symbol
  Elf32_Word    st_size;  // Size of the symbol
  unsigned char st_info;  // Symbol's type and binding attributes
  unsigned char st_other; // Must be zero; reserved
  Elf32_Half    st_shndx; // Which section (header table index) it's defined in
  
  // These accessors and mutators correspond to the ELF32_ST_BIND,
  // ELF32_ST_TYPE, and ELF32_ST_INFO macros defined in the ELF specification:
  unsigned char getBinding () const { return st_info >> 4; }
  unsigned char getType () const { return st_info & 0x0f; }
  void setBinding (unsigned char b) { setBindingAndType (b, getType ()); }
  void setType (unsigned char t) { setBindingAndType (getBinding (), t); }
  void setBindingAndType (unsigned char b, unsigned char t) {
    st_info = (b << 4) + (t & 0x0f);
  }
};

// Symbol bindings.
enum {
  STB_LOCAL = 0,   // Local symbol, not visible outside obj file containing def
  STB_GLOBAL = 1,  // Global symbol, visible to all object files being combined
  STB_WEAK = 2,    // Weak symbol, like global but lower-precedence
  STB_LOPROC = 13, // Lowest processor-specific binding type
  STB_HIPROC = 15  // Highest processor-specific binding type
};

// Symbol types.
enum {
  STT_NOTYPE  = 0,   // Symbol's type is not specified
  STT_OBJECT  = 1,   // Symbol is a data object (variable, array, etc.)
  STT_FUNC    = 2,   // Symbol is executable code (function, etc.)
  STT_SECTION = 3,   // Symbol refers to a section
  STT_FILE    = 4,   // Local, absolute symbol that refers to a file
  STT_LOPROC  = 13,  // Lowest processor-specific symbol type
  STT_HIPROC  = 15   // Highest processor-specific symbol type
};

// Relocation entry, without explicit addend.
struct Elf32_Rel {
  Elf32_Addr r_offset; // Location (file byte offset, or program virtual addr) 
  Elf32_Word r_info;   // Symbol table index and type of relocation to apply
  
  // These accessors and mutators correspond to the ELF32_R_SYM, ELF32_R_TYPE,
  // and ELF32_R_INFO macros defined in the ELF specification:
  Elf32_Word getSymbol () const { return (r_info >> 8); }
  unsigned char getType () const { return (unsigned char) (r_info & 0x0ff); }
  void setSymbol (Elf32_Word s) const { setSymbolAndType (s, getType ()); }
  void setType (unsigned char t) const { setSymbolAndType (getSymbol(), t); }
  void setSymbolAndType (Elf32_Word s, unsigned char t) {
    r_info = (s << 8) + t;
  };
};

// Relocation entry with explicit addend.
struct Elf32_Rela {
  Elf32_Addr  r_offset; // Location (file byte offset, or program virtual addr)     
  Elf32_Word  r_info;   // Symbol table index and type of relocation to apply
  Elf32_Sword r_addend; // Compute value for relocatable field by adding this
  
  // These accessors and mutators correspond to the ELF32_R_SYM, ELF32_R_TYPE,
  // and ELF32_R_INFO macros defined in the ELF specification:
  Elf32_Word getSymbol () const { return (r_info >> 8); }
  unsigned char getType () const { return (unsigned char) (r_info & 0x0ff); }
  void setSymbol (Elf32_Word s) const { setSymbolAndType (s, getType ()); }
  void setType (unsigned char t) const { setSymbolAndType (getSymbol(), t); }
  void setSymbolAndType (Elf32_Word s, unsigned char t) {
    r_info = (s << 8) + t;
  };
};

// Program header.
struct Elf32_Phdr {
  Elf32_Word p_type;   // Type of segment
  Elf32_Off  p_offset; // File offset where segment is located, in bytes
  Elf32_Addr p_vaddr;  // Virtual address of beginning of segment
  Elf32_Addr p_paddr;  // Physical address of beginning of segment (OS-specific)
  Elf32_Word p_filesz; // Num. of bytes in file image of segment (may be zero)
  Elf32_Word p_memsz;  // Num. of bytes in mem image of segment (may be zero)
  Elf32_Word p_flags;  // Segment flags
  Elf32_Word p_align;  // Segment alignment constraint
};

enum {
  PT_NULL    = 0, // Unused segment.
  PT_LOAD    = 1, // Loadable segment.
  PT_DYNAMIC = 2, // Dynamic linking information.
  PT_INTERP  = 3, // Interpreter pathname.
  PT_NOTE    = 4, // Auxiliary information.
  PT_SHLIB   = 5, // Reserved.
  PT_PHDR    = 6, // The program header table itself.
  PT_LOPROC  = 0x70000000, // Lowest processor-specific program hdr entry type.
  PT_HIPROC  = 0x7fffffff  // Highest processor-specific program hdr entry type.
};

} // end namespace ELF

} // end namespace llvm
