// Tencent is pleased to support the open source community by making RapidJSON available.
//
// Copyright (C) 2015 THL A29 Limited, a Tencent company, and Milo Yip.
//
// Licensed under the MIT License (the "License"); you may not use this file except
// in compliance with the License. You may obtain a copy of the License at
//
// http://opensource.org/licenses/MIT
//
// Unless required by applicable law or agreed to in writing, software distributed
// under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
// CONDITIONS OF ANY KIND, either express or implied. See the License for the
// specific language governing permissions and limitations under the License.

#ifndef LIBAI_RJ_LIBAI_RJ_H_
#define LIBAI_RJ_LIBAI_RJ_H_

/*!\file rapidjson.h
    \brief common definitions and configuration

    \see LIBAI_RJ_CONFIG
 */

/*! \defgroup LIBAI_RJ_CONFIG RapidJSON configuration
    \brief Configuration macros for library features

    Some RapidJSON features are configurable to adapt the library to a wide
    variety of platforms, environments and usage scenarios.  Most of the
    features can be configured in terms of overridden or predefined
    preprocessor macros at compile-time.

    Some additional customization is available in the \ref LIBAI_RJ_ERRORS APIs.

    \note These macros should be given on the compiler command-line
          (where applicable)  to avoid inconsistent values when compiling
          different translation units of a single application.
 */

#include <cstdlib>  // malloc(), realloc(), free(), size_t
#include <cstring>  // memset(), memcpy(), memmove(), memcmp()

///////////////////////////////////////////////////////////////////////////////
// LIBAI_RJ_VERSION_STRING
//
// ALWAYS synchronize the following 3 macros with corresponding variables in /CMakeLists.txt.
//

//!@cond LIBAI_RJ_HIDDEN_FROM_DOXYGEN
// token stringification
#define LIBAI_RJ_STRINGIFY(x) LIBAI_RJ_DO_STRINGIFY(x)
#define LIBAI_RJ_DO_STRINGIFY(x) #x

// token concatenation
#define LIBAI_RJ_JOIN(X, Y) LIBAI_RJ_DO_JOIN(X, Y)
#define LIBAI_RJ_DO_JOIN(X, Y) LIBAI_RJ_DO_JOIN2(X, Y)
#define LIBAI_RJ_DO_JOIN2(X, Y) X##Y
//!@endcond

/*! \def LIBAI_RJ_MAJOR_VERSION
    \ingroup LIBAI_RJ_CONFIG
    \brief Major version of RapidJSON in integer.
*/
/*! \def LIBAI_RJ_MINOR_VERSION
    \ingroup LIBAI_RJ_CONFIG
    \brief Minor version of RapidJSON in integer.
*/
/*! \def LIBAI_RJ_PATCH_VERSION
    \ingroup LIBAI_RJ_CONFIG
    \brief Patch version of RapidJSON in integer.
*/
/*! \def LIBAI_RJ_VERSION_STRING
    \ingroup LIBAI_RJ_CONFIG
    \brief Version of RapidJSON in "<major>.<minor>.<patch>" string format.
*/
#define LIBAI_RJ_MAJOR_VERSION 1
#define LIBAI_RJ_MINOR_VERSION 1
#define LIBAI_RJ_PATCH_VERSION 0
#define LIBAI_RJ_VERSION_STRING \
    LIBAI_RJ_STRINGIFY(LIBAI_RJ_MAJOR_VERSION.LIBAI_RJ_MINOR_VERSION.LIBAI_RJ_PATCH_VERSION)

///////////////////////////////////////////////////////////////////////////////
// LIBAI_RJ_NAMESPACE_(BEGIN|END)
/*! \def LIBAI_RJ_NAMESPACE
    \ingroup LIBAI_RJ_CONFIG
    \brief   provide custom rapidjson namespace

    In order to avoid symbol clashes and/or "One Definition Rule" errors
    between multiple inclusions of (different versions of) RapidJSON in
    a single binary, users can customize the name of the main RapidJSON
    namespace.

    In case of a single nesting level, defining \c LIBAI_RJ_NAMESPACE
    to a custom name (e.g. \c MyRapidJSON) is sufficient.  If multiple
    levels are needed, both \ref LIBAI_RJ_NAMESPACE_BEGIN and \ref
    LIBAI_RJ_NAMESPACE_END need to be defined as well:

    \code
    // in some .cpp file
    #define LIBAI_RJ_NAMESPACE my::rapidjson
    #define LIBAI_RJ_NAMESPACE_BEGIN namespace my { namespace rapidjson {
    #define LIBAI_RJ_NAMESPACE_END   } }
    #include "rapidjson/..."
    \endcode

    \see rapidjson
 */
/*! \def LIBAI_RJ_NAMESPACE_BEGIN
    \ingroup LIBAI_RJ_CONFIG
    \brief   provide custom rapidjson namespace (opening expression)
    \see LIBAI_RJ_NAMESPACE
*/
/*! \def LIBAI_RJ_NAMESPACE_END
    \ingroup LIBAI_RJ_CONFIG
    \brief   provide custom rapidjson namespace (closing expression)
    \see LIBAI_RJ_NAMESPACE
*/
#ifndef LIBAI_RJ_NAMESPACE
#define LIBAI_RJ_NAMESPACE libai_schema_json
#endif
#ifndef LIBAI_RJ_NAMESPACE_BEGIN
#define LIBAI_RJ_NAMESPACE_BEGIN namespace LIBAI_RJ_NAMESPACE {
#endif
#ifndef LIBAI_RJ_NAMESPACE_END
#define LIBAI_RJ_NAMESPACE_END }
#endif

///////////////////////////////////////////////////////////////////////////////
// __cplusplus macro

//!@cond LIBAI_RJ_HIDDEN_FROM_DOXYGEN

#if defined(_MSC_VER)
#define LIBAI_RJ_CPLUSPLUS _MSVC_LANG
#else
#define LIBAI_RJ_CPLUSPLUS __cplusplus
#endif

//!@endcond

///////////////////////////////////////////////////////////////////////////////
// LIBAI_RJ_HAS_STDSTRING

#ifndef LIBAI_RJ_HAS_STDSTRING
#ifdef LIBAI_RJ_DOXYGEN_RUNNING
#define LIBAI_RJ_HAS_STDSTRING 1 // force generation of documentation
#else
#define LIBAI_RJ_HAS_STDSTRING 0 // no std::string support by default
#endif
/*! \def LIBAI_RJ_HAS_STDSTRING
    \ingroup LIBAI_RJ_CONFIG
    \brief Enable RapidJSON support for \c std::string

    By defining this preprocessor symbol to \c 1, several convenience functions for using
    \ref rapidjson::GenericValue with \c std::string are enabled, especially
    for construction and comparison.

    \hideinitializer
*/
#endif // !defined(LIBAI_RJ_HAS_STDSTRING)

#if LIBAI_RJ_HAS_STDSTRING
#include <string>
#endif // LIBAI_RJ_HAS_STDSTRING

///////////////////////////////////////////////////////////////////////////////
// LIBAI_RJ_USE_MEMBERSMAP

/*! \def LIBAI_RJ_USE_MEMBERSMAP
    \ingroup LIBAI_RJ_CONFIG
    \brief Enable RapidJSON support for object members handling in a \c std::multimap

    By defining this preprocessor symbol to \c 1, \ref rapidjson::GenericValue object
    members are stored in a \c std::multimap for faster lookup and deletion times, a
    trade off with a slightly slower insertion time and a small object allocat(or)ed
    memory overhead.

    \hideinitializer
*/
#ifndef LIBAI_RJ_USE_MEMBERSMAP
#define LIBAI_RJ_USE_MEMBERSMAP 0 // not by default
#endif

///////////////////////////////////////////////////////////////////////////////
// LIBAI_RJ_NO_INT64DEFINE

/*! \def LIBAI_RJ_NO_INT64DEFINE
    \ingroup LIBAI_RJ_CONFIG
    \brief Use external 64-bit integer types.

    RapidJSON requires the 64-bit integer types \c int64_t and  \c uint64_t types
    to be available at global scope.

    If users have their own definition, define LIBAI_RJ_NO_INT64DEFINE to
    prevent RapidJSON from defining its own types.
*/
#ifndef LIBAI_RJ_NO_INT64DEFINE
//!@cond LIBAI_RJ_HIDDEN_FROM_DOXYGEN
#if defined(_MSC_VER) && (_MSC_VER < 1800) // Visual Studio 2013
#include "msinttypes/stdint.h"
#include "msinttypes/inttypes.h"
#else
// Other compilers should have this.
#include <stdint.h>
#include <inttypes.h>
#endif
//!@endcond
#ifdef LIBAI_RJ_DOXYGEN_RUNNING
#define LIBAI_RJ_NO_INT64DEFINE
#endif
#endif // LIBAI_RJ_NO_INT64TYPEDEF

///////////////////////////////////////////////////////////////////////////////
// LIBAI_RJ_FORCEINLINE

#ifndef LIBAI_RJ_FORCEINLINE
//!@cond LIBAI_RJ_HIDDEN_FROM_DOXYGEN
#if defined(_MSC_VER) && defined(NDEBUG)
#define LIBAI_RJ_FORCEINLINE __forceinline
#elif defined(__GNUC__) && __GNUC__ >= 4 && defined(NDEBUG)
#define LIBAI_RJ_FORCEINLINE __attribute__((always_inline))
#else
#define LIBAI_RJ_FORCEINLINE
#endif
//!@endcond
#endif // LIBAI_RJ_FORCEINLINE

///////////////////////////////////////////////////////////////////////////////
// LIBAI_RJ_ENDIAN
#define LIBAI_RJ_LITTLEENDIAN  0   //!< Little endian machine
#define LIBAI_RJ_BIGENDIAN     1   //!< Big endian machine

//! Endianness of the machine.
/*!
    \def LIBAI_RJ_ENDIAN
    \ingroup LIBAI_RJ_CONFIG

    GCC 4.6 provided macro for detecting endianness of the target machine. But other
    compilers may not have this. User can define LIBAI_RJ_ENDIAN to either
    \ref LIBAI_RJ_LITTLEENDIAN or \ref LIBAI_RJ_BIGENDIAN.

    Default detection implemented with reference to
    \li https://gcc.gnu.org/onlinedocs/gcc-4.6.0/cpp/Common-Predefined-Macros.html
    \li http://www.boost.org/doc/libs/1_42_0/boost/detail/endian.hpp
*/
#ifndef LIBAI_RJ_ENDIAN
// Detect with GCC 4.6's macro
#  ifdef __BYTE_ORDER__
#    if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#      define LIBAI_RJ_ENDIAN LIBAI_RJ_LITTLEENDIAN
#    elif __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#      define LIBAI_RJ_ENDIAN LIBAI_RJ_BIGENDIAN
#    else
#      error Unknown machine endianness detected. User needs to define LIBAI_RJ_ENDIAN.
#    endif // __BYTE_ORDER__
// Detect with GLIBC's endian.h
#  elif defined(__GLIBC__)
#    include <endian.h>
#    if (__BYTE_ORDER == __LITTLE_ENDIAN)
#      define LIBAI_RJ_ENDIAN LIBAI_RJ_LITTLEENDIAN
#    elif (__BYTE_ORDER == __BIG_ENDIAN)
#      define LIBAI_RJ_ENDIAN LIBAI_RJ_BIGENDIAN
#    else
#      error Unknown machine endianness detected. User needs to define LIBAI_RJ_ENDIAN.
#   endif // __GLIBC__
// Detect with _LITTLE_ENDIAN and _BIG_ENDIAN macro
#  elif defined(_LITTLE_ENDIAN) && !defined(_BIG_ENDIAN)
#    define LIBAI_RJ_ENDIAN LIBAI_RJ_LITTLEENDIAN
#  elif defined(_BIG_ENDIAN) && !defined(_LITTLE_ENDIAN)
#    define LIBAI_RJ_ENDIAN LIBAI_RJ_BIGENDIAN
// Detect with architecture macros
#  elif defined(__sparc) || defined(__sparc__) || defined(_POWER) || defined(__powerpc__) || defined(__ppc__) || defined(__ppc64__) || defined(__hpux) || defined(__hppa) || defined(_MIPSEB) || defined(_POWER) || defined(__s390__)
#    define LIBAI_RJ_ENDIAN LIBAI_RJ_BIGENDIAN
#  elif defined(__i386__) || defined(__alpha__) || defined(__ia64) || defined(__ia64__) || defined(_M_IX86) || defined(_M_IA64) || defined(_M_ALPHA) || defined(__amd64) || defined(__amd64__) || defined(_M_AMD64) || defined(__x86_64) || defined(__x86_64__) || defined(_M_X64) || defined(__bfin__)
#    define LIBAI_RJ_ENDIAN LIBAI_RJ_LITTLEENDIAN
#  elif defined(_MSC_VER) && (defined(_M_ARM) || defined(_M_ARM64))
#    define LIBAI_RJ_ENDIAN LIBAI_RJ_LITTLEENDIAN
#  elif defined(LIBAI_RJ_DOXYGEN_RUNNING)
#    define LIBAI_RJ_ENDIAN
#  else
#    error Unknown machine endianness detected. User needs to define LIBAI_RJ_ENDIAN.
#  endif
#endif // LIBAI_RJ_ENDIAN

///////////////////////////////////////////////////////////////////////////////
// LIBAI_RJ_64BIT

//! Whether using 64-bit architecture
#ifndef LIBAI_RJ_64BIT
#if defined(__LP64__) || (defined(__x86_64__) && defined(__ILP32__)) || defined(_WIN64) || defined(__EMSCRIPTEN__)
#define LIBAI_RJ_64BIT 1
#else
#define LIBAI_RJ_64BIT 0
#endif
#endif // LIBAI_RJ_64BIT

///////////////////////////////////////////////////////////////////////////////
// LIBAI_RJ_ALIGN

//! Data alignment of the machine.
/*! \ingroup LIBAI_RJ_CONFIG
    \param x pointer to align

    Some machines require strict data alignment. The default is 8 bytes.
    User can customize by defining the LIBAI_RJ_ALIGN function macro.
*/
#ifndef LIBAI_RJ_ALIGN
#define LIBAI_RJ_ALIGN(x) (((x) + static_cast<size_t>(7u)) & ~static_cast<size_t>(7u))
#endif

///////////////////////////////////////////////////////////////////////////////
// LIBAI_RJ_UINT64_C2

//! Construct a 64-bit literal by a pair of 32-bit integer.
/*!
    64-bit literal with or without ULL suffix is prone to compiler warnings.
    UINT64_C() is C macro which cause compilation problems.
    Use this macro to define 64-bit constants by a pair of 32-bit integer.
*/
#ifndef LIBAI_RJ_UINT64_C2
#define LIBAI_RJ_UINT64_C2(high32, low32) ((static_cast<uint64_t>(high32) << 32) | static_cast<uint64_t>(low32))
#endif

///////////////////////////////////////////////////////////////////////////////
// LIBAI_RJ_48BITPOINTER_OPTIMIZATION

//! Use only lower 48-bit address for some pointers.
/*!
    \ingroup LIBAI_RJ_CONFIG

    This optimization uses the fact that current X86-64 architecture only implement lower 48-bit virtual address.
    The higher 16-bit can be used for storing other data.
    \c GenericValue uses this optimization to reduce its size form 24 bytes to 16 bytes in 64-bit architecture.
*/
#ifndef LIBAI_RJ_48BITPOINTER_OPTIMIZATION
#if defined(__amd64__) || defined(__amd64) || defined(__x86_64__) || defined(__x86_64) || defined(_M_X64) || defined(_M_AMD64)
#define LIBAI_RJ_48BITPOINTER_OPTIMIZATION 1
#else
#define LIBAI_RJ_48BITPOINTER_OPTIMIZATION 0
#endif
#endif // LIBAI_RJ_48BITPOINTER_OPTIMIZATION

#if LIBAI_RJ_48BITPOINTER_OPTIMIZATION == 1
#if LIBAI_RJ_64BIT != 1
#error LIBAI_RJ_48BITPOINTER_OPTIMIZATION can only be set to 1 when LIBAI_RJ_64BIT=1
#endif
#define LIBAI_RJ_SETPOINTER(type, p, x) (p = reinterpret_cast<type *>((reinterpret_cast<uintptr_t>(p) & static_cast<uintptr_t>(LIBAI_RJ_UINT64_C2(0xFFFF0000, 0x00000000))) | reinterpret_cast<uintptr_t>(reinterpret_cast<const void*>(x))))
#define LIBAI_RJ_GETPOINTER(type, p) (reinterpret_cast<type *>(reinterpret_cast<uintptr_t>(p) & static_cast<uintptr_t>(LIBAI_RJ_UINT64_C2(0x0000FFFF, 0xFFFFFFFF))))
#else
#define LIBAI_RJ_SETPOINTER(type, p, x) (p = (x))
#define LIBAI_RJ_GETPOINTER(type, p) (p)
#endif

///////////////////////////////////////////////////////////////////////////////
// LIBAI_RJ_SSE2/LIBAI_RJ_SSE42/LIBAI_RJ_NEON/LIBAI_RJ_SIMD

/*! \def LIBAI_RJ_SIMD
    \ingroup LIBAI_RJ_CONFIG
    \brief Enable SSE2/SSE4.2/Neon optimization.

    RapidJSON supports optimized implementations for some parsing operations
    based on the SSE2, SSE4.2 or NEon SIMD extensions on modern Intel
    or ARM compatible processors.

    To enable these optimizations, three different symbols can be defined;
    \code
    // Enable SSE2 optimization.
    #define LIBAI_RJ_SSE2

    // Enable SSE4.2 optimization.
    #define LIBAI_RJ_SSE42
    \endcode

    // Enable ARM Neon optimization.
    #define LIBAI_RJ_NEON
    \endcode

    \c LIBAI_RJ_SSE42 takes precedence over SSE2, if both are defined.

    If any of these symbols is defined, RapidJSON defines the macro
    \c LIBAI_RJ_SIMD to indicate the availability of the optimized code.
*/
#if defined(LIBAI_RJ_SSE2) || defined(LIBAI_RJ_SSE42) \
    || defined(LIBAI_RJ_NEON) || defined(LIBAI_RJ_DOXYGEN_RUNNING)
#define LIBAI_RJ_SIMD
#endif

///////////////////////////////////////////////////////////////////////////////
// LIBAI_RJ_NO_SIZETYPEDEFINE

#ifndef LIBAI_RJ_NO_SIZETYPEDEFINE
/*! \def LIBAI_RJ_NO_SIZETYPEDEFINE
    \ingroup LIBAI_RJ_CONFIG
    \brief User-provided \c SizeType definition.

    In order to avoid using 32-bit size types for indexing strings and arrays,
    define this preprocessor symbol and provide the type rapidjson::SizeType
    before including RapidJSON:
    \code
    #define LIBAI_RJ_NO_SIZETYPEDEFINE
    namespace rapidjson { typedef ::std::size_t SizeType; }
    #include "rapidjson/..."
    \endcode

    \see rapidjson::SizeType
*/
#ifdef LIBAI_RJ_DOXYGEN_RUNNING
#define LIBAI_RJ_NO_SIZETYPEDEFINE
#endif
LIBAI_RJ_NAMESPACE_BEGIN
//! Size type (for string lengths, array sizes, etc.)
/*! RapidJSON uses 32-bit array/string indices even on 64-bit platforms,
    instead of using \c size_t. Users may override the SizeType by defining
    \ref LIBAI_RJ_NO_SIZETYPEDEFINE.
*/
typedef unsigned SizeType;
LIBAI_RJ_NAMESPACE_END
#endif

// always import std::size_t to rapidjson namespace
LIBAI_RJ_NAMESPACE_BEGIN
using std::size_t;
LIBAI_RJ_NAMESPACE_END

///////////////////////////////////////////////////////////////////////////////
// LIBAI_RJ_ASSERT

//! Assertion.
/*! \ingroup LIBAI_RJ_CONFIG
    By default, rapidjson uses C \c assert() for internal assertions.
    User can override it by defining LIBAI_RJ_ASSERT(x) macro.

    \note Parsing errors are handled and can be customized by the
          \ref LIBAI_RJ_ERRORS APIs.
*/
#ifndef LIBAI_RJ_ASSERT
#include <cassert>
#define LIBAI_RJ_ASSERT(x) assert(x)
#endif // LIBAI_RJ_ASSERT

///////////////////////////////////////////////////////////////////////////////
// LIBAI_RJ_STATIC_ASSERT

// Prefer C++11 static_assert, if available
#ifndef LIBAI_RJ_STATIC_ASSERT
#if LIBAI_RJ_CPLUSPLUS >= 201103L || ( defined(_MSC_VER) && _MSC_VER >= 1800 )
#define LIBAI_RJ_STATIC_ASSERT(x) \
   static_assert(x, LIBAI_RJ_STRINGIFY(x))
#endif // C++11
#endif // LIBAI_RJ_STATIC_ASSERT

// Adopt C++03 implementation from boost
#ifndef LIBAI_RJ_STATIC_ASSERT
#ifndef __clang__
//!@cond LIBAI_RJ_HIDDEN_FROM_DOXYGEN
#endif
LIBAI_RJ_NAMESPACE_BEGIN
template <bool x> struct STATIC_ASSERTION_FAILURE;
template <> struct STATIC_ASSERTION_FAILURE<true> { enum { value = 1 }; };
template <size_t x> struct StaticAssertTest {};
LIBAI_RJ_NAMESPACE_END

#if defined(__GNUC__) || defined(__clang__)
#define LIBAI_RJ_STATIC_ASSERT_UNUSED_ATTRIBUTE __attribute__((unused))
#else
#define LIBAI_RJ_STATIC_ASSERT_UNUSED_ATTRIBUTE 
#endif
#ifndef __clang__
//!@endcond
#endif

/*! \def LIBAI_RJ_STATIC_ASSERT
    \brief (Internal) macro to check for conditions at compile-time
    \param x compile-time condition
    \hideinitializer
 */
#define LIBAI_RJ_STATIC_ASSERT(x) \
    typedef ::LIBAI_RJ_NAMESPACE::StaticAssertTest< \
      sizeof(::LIBAI_RJ_NAMESPACE::STATIC_ASSERTION_FAILURE<bool(x) >)> \
    LIBAI_RJ_JOIN(StaticAssertTypedef, __LINE__) LIBAI_RJ_STATIC_ASSERT_UNUSED_ATTRIBUTE
#endif // LIBAI_RJ_STATIC_ASSERT

///////////////////////////////////////////////////////////////////////////////
// LIBAI_RJ_LIKELY, LIBAI_RJ_UNLIKELY

//! Compiler branching hint for expression with high probability to be true.
/*!
    \ingroup LIBAI_RJ_CONFIG
    \param x Boolean expression likely to be true.
*/
#ifndef LIBAI_RJ_LIKELY
#if defined(__GNUC__) || defined(__clang__)
#define LIBAI_RJ_LIKELY(x) __builtin_expect(!!(x), 1)
#else
#define LIBAI_RJ_LIKELY(x) (x)
#endif
#endif

//! Compiler branching hint for expression with low probability to be true.
/*!
    \ingroup LIBAI_RJ_CONFIG
    \param x Boolean expression unlikely to be true.
*/
#ifndef LIBAI_RJ_UNLIKELY
#if defined(__GNUC__) || defined(__clang__)
#define LIBAI_RJ_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define LIBAI_RJ_UNLIKELY(x) (x)
#endif
#endif

///////////////////////////////////////////////////////////////////////////////
// Helpers

//!@cond LIBAI_RJ_HIDDEN_FROM_DOXYGEN

#define LIBAI_RJ_MULTILINEMACRO_BEGIN do {
#define LIBAI_RJ_MULTILINEMACRO_END \
} while((void)0, 0)

// adopted from Boost
#define LIBAI_RJ_VERSION_CODE(x,y,z) \
  (((x)*100000) + ((y)*100) + (z))

#if defined(__has_builtin)
#define LIBAI_RJ_HAS_BUILTIN(x) __has_builtin(x)
#else
#define LIBAI_RJ_HAS_BUILTIN(x) 0
#endif

///////////////////////////////////////////////////////////////////////////////
// LIBAI_RJ_DIAG_PUSH/POP, LIBAI_RJ_DIAG_OFF

#if defined(__GNUC__)
#define LIBAI_RJ_GNUC \
    LIBAI_RJ_VERSION_CODE(__GNUC__,__GNUC_MINOR__,__GNUC_PATCHLEVEL__)
#endif

#if defined(__clang__) || (defined(LIBAI_RJ_GNUC) && LIBAI_RJ_GNUC >= LIBAI_RJ_VERSION_CODE(4,2,0))

#define LIBAI_RJ_PRAGMA(x) _Pragma(LIBAI_RJ_STRINGIFY(x))
#define LIBAI_RJ_DIAG_PRAGMA(x) LIBAI_RJ_PRAGMA(GCC diagnostic x)
#define LIBAI_RJ_DIAG_OFF(x) \
    LIBAI_RJ_DIAG_PRAGMA(ignored LIBAI_RJ_STRINGIFY(LIBAI_RJ_JOIN(-W,x)))

// push/pop support in Clang and GCC>=4.6
#if defined(__clang__) || (defined(LIBAI_RJ_GNUC) && LIBAI_RJ_GNUC >= LIBAI_RJ_VERSION_CODE(4,6,0))
#define LIBAI_RJ_DIAG_PUSH LIBAI_RJ_DIAG_PRAGMA(push)
#define LIBAI_RJ_DIAG_POP  LIBAI_RJ_DIAG_PRAGMA(pop)
#else // GCC >= 4.2, < 4.6
#define LIBAI_RJ_DIAG_PUSH /* ignored */
#define LIBAI_RJ_DIAG_POP /* ignored */
#endif

#elif defined(_MSC_VER)

// pragma (MSVC specific)
#define LIBAI_RJ_PRAGMA(x) __pragma(x)
#define LIBAI_RJ_DIAG_PRAGMA(x) LIBAI_RJ_PRAGMA(warning(x))

#define LIBAI_RJ_DIAG_OFF(x) LIBAI_RJ_DIAG_PRAGMA(disable: x)
#define LIBAI_RJ_DIAG_PUSH LIBAI_RJ_DIAG_PRAGMA(push)
#define LIBAI_RJ_DIAG_POP  LIBAI_RJ_DIAG_PRAGMA(pop)

#else

#define LIBAI_RJ_DIAG_OFF(x) /* ignored */
#define LIBAI_RJ_DIAG_PUSH   /* ignored */
#define LIBAI_RJ_DIAG_POP    /* ignored */

#endif // LIBAI_RJ_DIAG_*

///////////////////////////////////////////////////////////////////////////////
// C++11 features

#ifndef LIBAI_RJ_HAS_CXX11
#define LIBAI_RJ_HAS_CXX11 (LIBAI_RJ_CPLUSPLUS >= 201103L)
#endif

#ifndef LIBAI_RJ_HAS_CXX11_RVALUE_REFS
#if LIBAI_RJ_HAS_CXX11
#define LIBAI_RJ_HAS_CXX11_RVALUE_REFS 1
#elif defined(__clang__)
#if __has_feature(cxx_rvalue_references) && \
    (defined(_MSC_VER) || defined(_LIBCPP_VERSION) || defined(__GLIBCXX__) && __GLIBCXX__ >= 20080306)
#define LIBAI_RJ_HAS_CXX11_RVALUE_REFS 1
#else
#define LIBAI_RJ_HAS_CXX11_RVALUE_REFS 0
#endif
#elif (defined(LIBAI_RJ_GNUC) && (LIBAI_RJ_GNUC >= LIBAI_RJ_VERSION_CODE(4,3,0)) && defined(__GXX_EXPERIMENTAL_CXX0X__)) || \
      (defined(_MSC_VER) && _MSC_VER >= 1600) || \
      (defined(__SUNPRO_CC) && __SUNPRO_CC >= 0x5140 && defined(__GXX_EXPERIMENTAL_CXX0X__))

#define LIBAI_RJ_HAS_CXX11_RVALUE_REFS 1
#else
#define LIBAI_RJ_HAS_CXX11_RVALUE_REFS 0
#endif
#endif // LIBAI_RJ_HAS_CXX11_RVALUE_REFS

#if LIBAI_RJ_HAS_CXX11_RVALUE_REFS
#include <utility> // std::move
#endif

#ifndef LIBAI_RJ_HAS_CXX11_NOEXCEPT
#if LIBAI_RJ_HAS_CXX11
#define LIBAI_RJ_HAS_CXX11_NOEXCEPT 1
#elif defined(__clang__)
#define LIBAI_RJ_HAS_CXX11_NOEXCEPT __has_feature(cxx_noexcept)
#elif (defined(LIBAI_RJ_GNUC) && (LIBAI_RJ_GNUC >= LIBAI_RJ_VERSION_CODE(4,6,0)) && defined(__GXX_EXPERIMENTAL_CXX0X__)) || \
    (defined(_MSC_VER) && _MSC_VER >= 1900) || \
    (defined(__SUNPRO_CC) && __SUNPRO_CC >= 0x5140 && defined(__GXX_EXPERIMENTAL_CXX0X__))
#define LIBAI_RJ_HAS_CXX11_NOEXCEPT 1
#else
#define LIBAI_RJ_HAS_CXX11_NOEXCEPT 0
#endif
#endif
#ifndef LIBAI_RJ_NOEXCEPT
#if LIBAI_RJ_HAS_CXX11_NOEXCEPT
#define LIBAI_RJ_NOEXCEPT noexcept
#else
#define LIBAI_RJ_NOEXCEPT throw()
#endif // LIBAI_RJ_HAS_CXX11_NOEXCEPT
#endif

// no automatic detection, yet
#ifndef LIBAI_RJ_HAS_CXX11_TYPETRAITS
#if (defined(_MSC_VER) && _MSC_VER >= 1700)
#define LIBAI_RJ_HAS_CXX11_TYPETRAITS 1
#else
#define LIBAI_RJ_HAS_CXX11_TYPETRAITS 0
#endif
#endif

#ifndef LIBAI_RJ_HAS_CXX11_RANGE_FOR
#if defined(__clang__)
#define LIBAI_RJ_HAS_CXX11_RANGE_FOR __has_feature(cxx_range_for)
#elif (defined(LIBAI_RJ_GNUC) && (LIBAI_RJ_GNUC >= LIBAI_RJ_VERSION_CODE(4,6,0)) && defined(__GXX_EXPERIMENTAL_CXX0X__)) || \
      (defined(_MSC_VER) && _MSC_VER >= 1700) || \
      (defined(__SUNPRO_CC) && __SUNPRO_CC >= 0x5140 && defined(__GXX_EXPERIMENTAL_CXX0X__))
#define LIBAI_RJ_HAS_CXX11_RANGE_FOR 1
#else
#define LIBAI_RJ_HAS_CXX11_RANGE_FOR 0
#endif
#endif // LIBAI_RJ_HAS_CXX11_RANGE_FOR

///////////////////////////////////////////////////////////////////////////////
// C++17 features

#ifndef LIBAI_RJ_HAS_CXX17
#define LIBAI_RJ_HAS_CXX17 (LIBAI_RJ_CPLUSPLUS >= 201703L)
#endif

#if LIBAI_RJ_HAS_CXX17
# define LIBAI_RJ_DELIBERATE_FALLTHROUGH [[fallthrough]]
#elif defined(__has_cpp_attribute)
# if __has_cpp_attribute(clang::fallthrough)
#  define LIBAI_RJ_DELIBERATE_FALLTHROUGH [[clang::fallthrough]]
# elif __has_cpp_attribute(fallthrough)
#  define LIBAI_RJ_DELIBERATE_FALLTHROUGH __attribute__((fallthrough))
# else
#  define LIBAI_RJ_DELIBERATE_FALLTHROUGH
# endif
#else
# define LIBAI_RJ_DELIBERATE_FALLTHROUGH
#endif

//!@endcond

//! Assertion (in non-throwing contexts).
 /*! \ingroup LIBAI_RJ_CONFIG
    Some functions provide a \c noexcept guarantee, if the compiler supports it.
    In these cases, the \ref LIBAI_RJ_ASSERT macro cannot be overridden to
    throw an exception.  This macro adds a separate customization point for
    such cases.

    Defaults to C \c assert() (as \ref LIBAI_RJ_ASSERT), if \c noexcept is
    supported, and to \ref LIBAI_RJ_ASSERT otherwise.
 */

///////////////////////////////////////////////////////////////////////////////
// LIBAI_RJ_NOEXCEPT_ASSERT

#ifndef LIBAI_RJ_NOEXCEPT_ASSERT
#ifdef LIBAI_RJ_ASSERT_THROWS
#include <cassert>
#define LIBAI_RJ_NOEXCEPT_ASSERT(x) assert(x)
#else
#define LIBAI_RJ_NOEXCEPT_ASSERT(x) LIBAI_RJ_ASSERT(x)
#endif // LIBAI_RJ_ASSERT_THROWS
#endif // LIBAI_RJ_NOEXCEPT_ASSERT

///////////////////////////////////////////////////////////////////////////////
// malloc/realloc/free

#ifndef LIBAI_RJ_MALLOC
///! customization point for global \c malloc
#define LIBAI_RJ_MALLOC(size) std::malloc(size)
#endif
#ifndef LIBAI_RJ_REALLOC
///! customization point for global \c realloc
#define LIBAI_RJ_REALLOC(ptr, new_size) std::realloc(ptr, new_size)
#endif
#ifndef LIBAI_RJ_FREE
///! customization point for global \c free
#define LIBAI_RJ_FREE(ptr) std::free(ptr)
#endif

///////////////////////////////////////////////////////////////////////////////
// new/delete

#ifndef LIBAI_RJ_NEW
///! customization point for global \c new
#define LIBAI_RJ_NEW(TypeName) new TypeName
#endif
#ifndef LIBAI_RJ_DELETE
///! customization point for global \c delete
#define LIBAI_RJ_DELETE(x) delete x
#endif

///////////////////////////////////////////////////////////////////////////////
// Type

/*! \namespace rapidjson
    \brief main RapidJSON namespace
    \see LIBAI_RJ_NAMESPACE
*/
LIBAI_RJ_NAMESPACE_BEGIN

//! Type of JSON value
enum Type {
    kNullType = 0,      //!< null
    kFalseType = 1,     //!< false
    kTrueType = 2,      //!< true
    kObjectType = 3,    //!< object
    kArrayType = 4,     //!< array
    kStringType = 5,    //!< string
    kNumberType = 6     //!< number
};

LIBAI_RJ_NAMESPACE_END

#endif // LIBAI_RJ_LIBAI_RJ_H_
