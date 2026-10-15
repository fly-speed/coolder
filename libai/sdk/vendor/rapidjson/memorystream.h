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

#ifndef LIBAI_RJ_MEMORYSTREAM_H_
#define LIBAI_RJ_MEMORYSTREAM_H_

#include "stream.h"

#ifdef __clang__
LIBAI_RJ_DIAG_PUSH
LIBAI_RJ_DIAG_OFF(unreachable-code)
LIBAI_RJ_DIAG_OFF(missing-noreturn)
#endif

LIBAI_RJ_NAMESPACE_BEGIN

//! Represents an in-memory input byte stream.
/*!
    This class is mainly for being wrapped by EncodedInputStream or AutoUTFInputStream.

    It is similar to FileReadBuffer but the source is an in-memory buffer instead of a file.

    Differences between MemoryStream and StringStream:
    1. StringStream has encoding but MemoryStream is a byte stream.
    2. MemoryStream needs size of the source buffer and the buffer don't need to be null terminated. StringStream assume null-terminated string as source.
    3. MemoryStream supports Peek4() for encoding detection. StringStream is specified with an encoding so it should not have Peek4().
    \note implements Stream concept
*/
struct MemoryStream {
    typedef char Ch; // byte

    MemoryStream(const Ch *src, size_t size) : src_(src), begin_(src), end_(src + size), size_(size) {}

    Ch Peek() const { return LIBAI_RJ_UNLIKELY(src_ == end_) ? '\0' : *src_; }
    Ch Take() { return LIBAI_RJ_UNLIKELY(src_ == end_) ? '\0' : *src_++; }
    size_t Tell() const { return static_cast<size_t>(src_ - begin_); }

    Ch* PutBegin() { LIBAI_RJ_ASSERT(false); return 0; }
    void Put(Ch) { LIBAI_RJ_ASSERT(false); }
    void Flush() { LIBAI_RJ_ASSERT(false); }
    size_t PutEnd(Ch*) { LIBAI_RJ_ASSERT(false); return 0; }

    // For encoding detection only.
    const Ch* Peek4() const {
        return Tell() + 4 <= size_ ? src_ : 0;
    }

    const Ch* src_;     //!< Current read position.
    const Ch* begin_;   //!< Original head of the string.
    const Ch* end_;     //!< End of stream.
    size_t size_;       //!< Size of the stream.
};

LIBAI_RJ_NAMESPACE_END

#ifdef __clang__
LIBAI_RJ_DIAG_POP
#endif

#endif // LIBAI_RJ_MEMORYBUFFER_H_
