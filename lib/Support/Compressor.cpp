//===- lib/Support/Compressor.cpp -------------------------------*- C++ -*-===//
// 
//                     The LLVM Compiler Infrastructure
//
// This file was developed by Reid Spencer and is distributed under the 
// University of Illinois Open Source License. See LICENSE.TXT for details.
// 
//===----------------------------------------------------------------------===//
//
// This file implements the llvm::Compressor class, an abstraction for memory
// block compression.
//
//===----------------------------------------------------------------------===//

#include "llvm/Config/config.h"
#include "llvm/Support/Compressor.h"
#include "llvm/ADT/StringExtras.h"
#include <cassert>
#include <string>
#include "bzip2/bzlib.h"

namespace {

enum CompressionTypes {
  COMP_TYPE_NONE  = '0',
  COMP_TYPE_BZIP2 = '2',
};

inline int getdata(char*& buffer, unsigned& size, 
                   llvm::Compressor::OutputDataCallback* cb, void* context) {
  buffer = 0;
  size = 0;
  int result = (*cb)(buffer, size, context);
  assert(buffer != 0 && "Invalid result from Compressor callback");
  assert(size != 0 && "Invalid result from Compressor callback");
  return result;
}

//===----------------------------------------------------------------------===//
//=== NULLCOMP - a compression like set of routines that just copies data 
//===            without doing any compression. This is provided so that if the
//===            configured environment doesn't have a compression library the
//===            program can still work, albeit using more data/memory.
//===----------------------------------------------------------------------===//

struct NULLCOMP_stream {
  // User provided fields
  char* next_in;
  unsigned avail_in;
  char* next_out;
  unsigned avail_out;

  // Information fields
  uint64_t output_count; // Total count of output bytes
};

void NULLCOMP_init(NULLCOMP_stream* s) {
  s->output_count = 0;
}

bool NULLCOMP_compress(NULLCOMP_stream* s) {
  assert(s && "Invalid NULLCOMP_stream");
  assert(s->next_in != 0);
  assert(s->next_out != 0);
  assert(s->avail_in >= 1);
  assert(s->avail_out >= 1);

  if (s->avail_out >= s->avail_in) {
    ::memcpy(s->next_out, s->next_in, s->avail_in);
    s->output_count += s->avail_in;
    s->avail_out -= s->avail_in;
    s->next_in += s->avail_in;
    s->avail_in = 0;
    return true;
  } else {
    ::memcpy(s->next_out, s->next_in, s->avail_out);
    s->output_count += s->avail_out;
    s->avail_in -= s->avail_out;
    s->next_in += s->avail_out;
    s->avail_out = 0;
    return false;
  }
}

bool NULLCOMP_decompress(NULLCOMP_stream* s) {
  assert(s && "Invalid NULLCOMP_stream");
  assert(s->next_in != 0);
  assert(s->next_out != 0);
  assert(s->avail_in >= 1);
  assert(s->avail_out >= 1);

  if (s->avail_out >= s->avail_in) {
    ::memcpy(s->next_out, s->next_in, s->avail_in);
    s->output_count += s->avail_in;
    s->avail_out -= s->avail_in;
    s->next_in += s->avail_in;
    s->avail_in = 0;
    return true;
  } else {
    ::memcpy(s->next_out, s->next_in, s->avail_out);
    s->output_count += s->avail_out;
    s->avail_in -= s->avail_out;
    s->next_in += s->avail_out;
    s->avail_out = 0;
    return false;
  }
}

void NULLCOMP_end(NULLCOMP_stream* strm) {
}

/// This structure is only used when a bytecode file is compressed.
/// As bytecode is being decompressed, the memory buffer might need
/// to be reallocated. The buffer allocation is handled in a callback 
/// and this structure is needed to retain information across calls
/// to the callback.
/// @brief An internal buffer object used for handling decompression
struct BufferContext {
  char* buff;
  unsigned size;
  BufferContext(unsigned compressedSize ) { 
    // Null to indicate malloc of a new block
    buff = 0; 

    // Compute the initial length of the uncompression buffer. Note that this
    // is twice the length of the compressed buffer and will be doubled again
    // in the callback for an initial allocation of 4x compressedSize.  This 
    // calculation is based on the typical compression ratio of bzip2 on LLVM 
    // bytecode files which typically ranges in the 50%-75% range.   Since we 
    // tyipcally get at least 50%, doubling is insufficient. By using a 4x 
    // multiplier on the first allocation, we minimize the impact of having to
    // copy the buffer on reallocation.
    size = compressedSize*2; 
  }

  /// This function handles allocation of the buffer used for decompression of
  /// compressed bytecode files. It is called by Compressor::decompress which is
  /// called by BytecodeReader::ParseBytecode. 
  static unsigned callback(char*&buff, unsigned& sz, void* ctxt){
    // Case the context variable to our BufferContext
    BufferContext* bc = reinterpret_cast<BufferContext*>(ctxt);

    // Compute the new, doubled, size of the block
    unsigned new_size = bc->size * 2;

    // Extend or allocate the block (realloc(0,n) == malloc(n))
    char* new_buff = (char*) ::realloc(bc->buff, new_size);

    // Figure out what to return to the Compressor. If this is the first call,
    // then bc->buff will be null. In this case we want to return the entire
    // buffer because there was no previous allocation.  Otherwise, when the
    // buffer is reallocated, we save the new base pointer in the 
    // BufferContext.buff field but return the address of only the extension, 
    // mid-way through the buffer (since its size was doubled). Furthermore, 
    // the sz result must be 1/2 the total size of the buffer.
    if (bc->buff == 0 ) {
      buff = bc->buff = new_buff;
      sz = new_size;
    } else {
      bc->buff = new_buff;
      buff = new_buff + bc->size;
      sz = bc->size;
    }

    // Retain the size of the allocated block
    bc->size = new_size;

    // Make sure we fail (return 1) if we didn't get any memory.
    return (bc->buff == 0 ? 1 : 0);
  }
};

// This structure retains the context when compressing the bytecode file. The
// WriteCompressedData function below uses it to keep track of the previously
// filled chunk of memory (which it writes) and how many bytes have been 
// written.
struct WriterContext {
  // Initialize the context
  WriterContext(std::ostream*OS, unsigned CS) 
    : chunk(0), sz(0), written(0), compSize(CS), Out(OS) {}

  // Make sure we clean up memory
  ~WriterContext() {
    if (chunk)
      delete [] chunk;
  }

  // Write the chunk
  void write(unsigned size = 0) {
    unsigned write_size = (size == 0 ? sz : size);
    Out->write(chunk,write_size);
    written += write_size;
    delete [] chunk;
    chunk = 0;
    sz = 0;
  }

  // This function is a callback used by the Compressor::compress function to 
  // allocate memory for the compression buffer. This function fulfills that
  // responsibility but also writes the previous (now filled) buffer out to the
  // stream. 
  static unsigned callback(char*& buffer, unsigned& size, void* context) {
    // Cast the context to the structure it must point to.
    WriterContext* ctxt = 
      reinterpret_cast<WriterContext*>(context);

    // If there's a previously allocated chunk, it must now be filled with
    // compressed data, so we write it out and deallocate it.
    if (ctxt->chunk != 0 && ctxt->sz > 0 ) {
      ctxt->write();
    }

    // Compute the size of the next chunk to allocate. We attempt to allocate
    // enough memory to handle the compression in a single memory allocation. In
    // general, the worst we do on compression of bytecode is about 50% so we
    // conservatively estimate compSize / 2 as the size needed for the
    // compression buffer. compSize is the size of the compressed data, provided
    // by WriteBytecodeToFile.
    size = ctxt->sz = ctxt->compSize / 2;

    // Allocate the chunks
    buffer = ctxt->chunk = new char [size];

    // We must return 1 if the allocation failed so that the Compressor knows
    // not to use the buffer pointer.
    return (ctxt->chunk == 0 ? 1 : 0);
  }

  char* chunk;       // pointer to the chunk of memory filled by compression
  unsigned sz;       // size of chunk
  unsigned written;  // aggregate total of bytes written in all chunks
  unsigned compSize; // size of the uncompressed buffer
  std::ostream* Out; // The stream we write the data to.
};

}

namespace llvm {

// Compress in one of three ways
uint64_t Compressor::compress(const char* in, unsigned size, 
    OutputDataCallback* cb, void* context ) {
  assert(in && "Can't compress null buffer");
  assert(size && "Can't compress empty buffer");
  assert(cb && "Can't compress without a callback function");

  uint64_t result = 0;

  // For small files, we just don't bother compressing. bzip2 isn't very good
  // with tiny files and can actually make the file larger, so we just avoid
  // it altogether.
  if (size > 64*1024) {
    // Set up the bz_stream
    bz_stream bzdata;
    bzdata.bzalloc = 0;
    bzdata.bzfree = 0;
    bzdata.opaque = 0;
    bzdata.next_in = (char*)in;
    bzdata.avail_in = size;
    bzdata.next_out = 0;
    bzdata.avail_out = 0;
    switch ( BZ2_bzCompressInit(&bzdata, 5, 0, 100) ) {
      case BZ_CONFIG_ERROR: throw std::string("bzip2 library mis-compiled");
      case BZ_PARAM_ERROR:  throw std::string("Compressor internal error");
      case BZ_MEM_ERROR:    throw std::string("Out of memory");
      case BZ_OK:
      default:
        break;
    }

    // Get a block of memory
    if (0 != getdata(bzdata.next_out, bzdata.avail_out,cb,context)) {
      BZ2_bzCompressEnd(&bzdata);
      throw std::string("Can't allocate output buffer");
    }

    // Put compression code in first byte
    (*bzdata.next_out++) = COMP_TYPE_BZIP2;
    bzdata.avail_out--;

    // Compress it
    int bzerr = BZ_FINISH_OK;
    while (BZ_FINISH_OK == (bzerr = BZ2_bzCompress(&bzdata, BZ_FINISH))) {
      if (0 != getdata(bzdata.next_out, bzdata.avail_out,cb,context)) {
        BZ2_bzCompressEnd(&bzdata);
        throw std::string("Can't allocate output buffer");
      }
    }
    switch (bzerr) {
      case BZ_SEQUENCE_ERROR:
      case BZ_PARAM_ERROR: throw std::string("Param/Sequence error");
      case BZ_FINISH_OK:
      case BZ_STREAM_END: break;
      default: throw std::string("Oops: ") + utostr(unsigned(bzerr));
    }

    // Finish
    result = (static_cast<uint64_t>(bzdata.total_out_hi32) << 32) |
        bzdata.total_out_lo32 + 1;

    BZ2_bzCompressEnd(&bzdata);
  } else {
    // Do null compression, for small files
    NULLCOMP_stream sdata;
    sdata.next_in = (char*)in;
    sdata.avail_in = size;
    NULLCOMP_init(&sdata);

    if (0 != getdata(sdata.next_out, sdata.avail_out,cb,context)) {
      throw std::string("Can't allocate output buffer");
    }

    *(sdata.next_out++) = COMP_TYPE_NONE;
    sdata.avail_out--;

    while (!NULLCOMP_compress(&sdata)) {
      if (0 != getdata(sdata.next_out, sdata.avail_out,cb,context)) {
        throw std::string("Can't allocate output buffer");
      }
    }

    result = sdata.output_count + 1;
    NULLCOMP_end(&sdata);
  }
  return result;
}

uint64_t 
Compressor::compressToNewBuffer(const char* in, unsigned size, char*&out) {
  BufferContext bc(size);
  unsigned result = compress(in,size,BufferContext::callback,(void*)&bc);
  out = bc.buff;
  return result;
}

uint64_t 
Compressor::compressToStream(const char*in, unsigned size, std::ostream& out) {
  // Set up the context and writer
  WriterContext ctxt(&out,size / 2);

  // Compress everything after the magic number (which we'll alter)
  uint64_t zipSize = Compressor::compress(in,size,
    WriterContext::callback, (void*)&ctxt);

  if (ctxt.chunk) {
    ctxt.write(zipSize - ctxt.written);
  }
  return zipSize;
}

// Decompress in one of three ways
uint64_t Compressor::decompress(const char *in, unsigned size,
                                OutputDataCallback* cb, void* context) {
  assert(in && "Can't decompress null buffer");
  assert(size > 1 && "Can't decompress empty buffer");
  assert(cb && "Can't decompress without a callback function");

  uint64_t result = 0;

  switch (*in++) {
    case COMP_TYPE_BZIP2: {
      // Set up the bz_stream
      bz_stream bzdata;
      bzdata.bzalloc = 0;
      bzdata.bzfree = 0;
      bzdata.opaque = 0;
      bzdata.next_in = (char*)in;
      bzdata.avail_in = size - 1;
      bzdata.next_out = 0;
      bzdata.avail_out = 0;
      switch ( BZ2_bzDecompressInit(&bzdata, 0, 0) ) {
        case BZ_CONFIG_ERROR: throw std::string("bzip2 library mis-compiled");
        case BZ_PARAM_ERROR:  throw std::string("Compressor internal error");
        case BZ_MEM_ERROR:    throw std::string("Out of memory");
        case BZ_OK:
        default:
          break;
      }

      // Get a block of memory
      if (0 != getdata(bzdata.next_out, bzdata.avail_out,cb,context)) {
        BZ2_bzDecompressEnd(&bzdata);
        throw std::string("Can't allocate output buffer");
      }

      // Decompress it
      int bzerr = BZ_OK;
      while (BZ_OK == (bzerr = BZ2_bzDecompress(&bzdata))) {
        if (0 != getdata(bzdata.next_out, bzdata.avail_out,cb,context)) {
          BZ2_bzDecompressEnd(&bzdata);
          throw std::string("Can't allocate output buffer");
        }
      }

      switch (bzerr) {
        case BZ_PARAM_ERROR:  throw std::string("Compressor internal error");
        case BZ_MEM_ERROR:    throw std::string("Out of memory");
        case BZ_DATA_ERROR:   throw std::string("Data integrity error");
        case BZ_DATA_ERROR_MAGIC:throw std::string("Data is not BZIP2");
        default: throw("Ooops");
        case BZ_STREAM_END:
          break;
      }

      // Finish
      result = (static_cast<uint64_t>(bzdata.total_out_hi32) << 32) |
        bzdata.total_out_lo32;
      BZ2_bzDecompressEnd(&bzdata);
      break;
    }

    case COMP_TYPE_NONE: {
      NULLCOMP_stream sdata;
      sdata.next_in = (char*)in;
      sdata.avail_in = size - 1;
      NULLCOMP_init(&sdata);

      if (0 != getdata(sdata.next_out, sdata.avail_out,cb,context)) {
        throw std::string("Can't allocate output buffer");
      }

      while (!NULLCOMP_decompress(&sdata)) {
        if (0 != getdata(sdata.next_out, sdata.avail_out,cb,context)) {
          throw std::string("Can't allocate output buffer");
        }
      }

      result = sdata.output_count;
      NULLCOMP_end(&sdata);
      break;
    }

    default:
      throw std::string("Unknown type of compressed data");
  }

  return result;
}

uint64_t 
Compressor::decompressToNewBuffer(const char* in, unsigned size, char*&out) {
  BufferContext bc(size);
  unsigned result = decompress(in,size,BufferContext::callback,(void*)&bc);
  out = bc.buff;
  return result;
}
                                                                                                                                            
uint64_t 
Compressor::decompressToStream(const char*in, unsigned size, std::ostream& out){
  // Set up the context and writer
  WriterContext ctxt(&out,size / 2);

  // Compress everything after the magic number (which we'll alter)
  uint64_t zipSize = Compressor::decompress(in,size,
    WriterContext::callback, (void*)&ctxt);

  if (ctxt.chunk) {
    ctxt.write(zipSize - ctxt.written);
  }
  return zipSize;
}

}

// vim: sw=2 ai
