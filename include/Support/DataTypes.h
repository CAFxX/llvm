//===-- include/Support/DataTypes.h - Define fixed size types ----*- C++ -*--=//
//
// This file contains definitions to figure out the size of _HOST_ data types.
// This file is important because different host OS's define different macros,
// which makes portability tough.  This file exports the following definitions:
//
//   ENDIAN_LITTLE : is #define'd if the host is little endian
//   int64_t       : is a typedef for the signed 64 bit system type
//   uint64_t      : is a typedef for the unsigned 64 bit system type
//   INT64_MAX     : is a #define specifying the max value for int64_t's
//
// No library is required when using these functinons.
//
//===----------------------------------------------------------------------===//

#ifndef SUPPORT_DATATYPES_H
#define SUPPORT_DATATYPES_H

#include "Config/config.h"

#define __STDC_LIMIT_MACROS 1

#ifdef HAVE_INTTYPES_H
#include <inttypes.h>
#endif

#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif

#if (defined(ENDIAN_LITTLE) && defined(ENDIAN_BIG))
#error "Cannot define both ENDIAN_LITTLE and ENDIAN_BIG!"
#endif

#if (!defined(ENDIAN_LITTLE) && !defined(ENDIAN_BIG)) || !defined(INT64_MAX)
#error "include/Support/DataTypes.h could not determine endianness!"
#endif

#endif  /* SUPPORT_DATATYPES_H */
