//
// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE.md file in the project root for full license information.
//
// push_audio_input_stream.cpp: Implementation definitions for CSpxPushAudioInputStream C++ class
//

#include "stdafx.h"
#include "push_audio_input_stream.h"
#include <property_id_2_name_map.h>
#include <algorithm>
#include <cstring>
#include <chrono>

// change it to 1 to debug.
#define TURN_ON_VERBOSE_AUDIO_DEBUGGING 0

namespace Microsoft {
namespace CognitiveServices {
namespace Speech {
namespace Impl {

CSpxPushAudioInputStream::CSpxPushAudioInputStream() :
    m_bytesInBuffer(0),
    m_ptrIntoBuffer(nullptr),
    m_bytesLeftInBuffer(0),
    m_endOfStream(false)
{
}

void CSpxPushAudioInputStream::SetFormat(SPXWAVEFORMATEX* format)
{
    SPX_IFTRUE_THROW_HR(m_format != nullptr, SPXERR_ALREADY_INITIALIZED);

    // Allocate the buffer for the format
    auto formatSize = sizeof(SPXWAVEFORMATEX) + format->cbSize;
    m_format = SpxAllocWAVEFORMATEX(formatSize);
    SPX_DBG_TRACE_VERBOSE_IF(TURN_ON_VERBOSE_AUDIO_DEBUGGING, "CSpxPushAudioInputStream::SetFormat is called with format 0x%p", (void*)format);
    // Copy the format
    memcpy(m_format.get(), format, formatSize);
}

void CSpxPushAudioInputStream::Write(uint8_t* buffer, uint32_t size)
{
    SPX_DBG_TRACE_VERBOSE_IF(TURN_ON_VERBOSE_AUDIO_DEBUGGING, "CSpxPushAudioInputStream::Write buffer %p size=%d", (void*)buffer, size);
    if (buffer != nullptr && size > 0)
    {
        WriteBuffer(buffer, size);
    }
    else
    {
        SignalEndOfStream();
    }
}

void CSpxPushAudioInputStream::SetProperty(PropertyId propertyId, const SPXSTRING& value)
{
    if (propertyId == PropertyId::DataBuffer_TimeStamp)
    {
        m_dataInfo[DATA_INFO_TIME_STAMP_KEY] = value;
    }
    else if (propertyId == PropertyId::DataBuffer_UserId)
    {
        m_dataInfo[DATA_INFO_SPEAKER_ID_KEY] = value;
    }
    else
    {
        std::string str = "Error: PropertyId " + std::to_string(static_cast<int>(propertyId)) + " is not supported";
        ThrowInvalidArgumentException(str);
    }
}

void CSpxPushAudioInputStream::SetProperty(const SPXSTRING& name, const SPXSTRING& value)
{
    m_dataInfo[name] = value;
}

uint16_t CSpxPushAudioInputStream::GetFormat(SPXWAVEFORMATEX* formatBuffer, uint16_t formatSize)
{
    uint16_t formatSizeRequired = sizeof(SPXWAVEFORMATEX) + m_format->cbSize;
    SPX_DBG_TRACE_VERBOSE_IF(TURN_ON_VERBOSE_AUDIO_DEBUGGING, "CSpxPushAudioInputStream::GetFormat is called formatBuffer is %s formatSize=%d", formatBuffer? "not null":"null", formatSize);

    if (formatBuffer != nullptr)
    {
        size_t size = std::min(formatSize, formatSizeRequired);
        std::memcpy(formatBuffer, m_format.get(), size);
    }

    return formatSizeRequired;
}

uint32_t CSpxPushAudioInputStream::Read(uint8_t* buffer, uint32_t bytesToRead)
{
    SPX_DBG_TRACE_VERBOSE("CSpxPushAudioInputStream::Read: bytesToRead=%d", bytesToRead);

    uint32_t totalBytesRead = 0;
    while (bytesToRead > 0)
    {
        // If we don't have any bytes in our buffer, let's get a new buffer from the queue
        if (m_bytesLeftInBuffer == 0 && !m_audioQueue.empty())
        {
            std::unique_lock<std::mutex> lock(m_mutex);

            auto item = m_audioQueue.front();
            m_audioQueue.pop();

            m_buffer = std::get<0>(item);
            m_bytesInBuffer = std::get<1>(item);
            m_dataInfoInRead = std::get<2>(item);

            m_ptrIntoBuffer = m_buffer.get();
            m_bytesLeftInBuffer = m_bytesInBuffer;
        }

        // If we still don't have a buffer to work with...
        if (m_bytesLeftInBuffer == 0)
        {
             SPX_DBG_TRACE_VERBOSE_IF(TURN_ON_VERBOSE_AUDIO_DEBUGGING, "CSpxPushAudioInputStream::Read: endOfStream is %s", m_endOfStream ? "true" : "false");

            if (m_endOfStream)
            {
                // Caller told us we're done; we're outta here!
                break;
            }
            else if (WaitForMoreData())
            {
                // Caller provided more data; back to the top of the loop!
                continue;
            }
            else // We waited for more data, but instead...
            {
                SPX_DBG_TRACE_VERBOSE("%s: End of stream detected...", __FUNCTION__);
                break; // We're outta here!
            }
        }

        // Now that we know we have a buffer with data in it ... let's copy some bytes
        uint32_t bytesThisLoop = std::min(bytesToRead, m_bytesLeftInBuffer);
        std::memcpy(buffer, m_ptrIntoBuffer, bytesThisLoop);
        buffer += bytesThisLoop;

        // And update our buffering pointers/members
        m_ptrIntoBuffer += bytesThisLoop;
        m_bytesLeftInBuffer -= bytesThisLoop;
        bytesToRead -= bytesThisLoop;
        totalBytesRead += bytesThisLoop;
    }

    SPX_DBG_TRACE_VERBOSE("CSpxPushAudioInputStream::Read: totalBytesRead=%d", totalBytesRead);
    return totalBytesRead;
}

SPXSTRING CSpxPushAudioInputStream::GetProperty(PropertyId propertyId)
{
    if (propertyId == PropertyId::DataBuffer_TimeStamp)
    {
        if (m_dataInfoInRead.find(DATA_INFO_TIME_STAMP_KEY) != m_dataInfoInRead.end())
        {
            return m_dataInfoInRead[DATA_INFO_TIME_STAMP_KEY];
        }
    }
    else if (propertyId == PropertyId::DataBuffer_UserId)
    {
        if (m_dataInfoInRead.find(DATA_INFO_SPEAKER_ID_KEY) != m_dataInfoInRead.end())
        {
            return m_dataInfoInRead[DATA_INFO_SPEAKER_ID_KEY];
        }
    }
    return "";
}

void CSpxPushAudioInputStream::WriteBuffer(uint8_t* buffer, uint32_t size)
{
    SPX_DBG_TRACE_VERBOSE("%s: size=%d", __FUNCTION__, size);

    // Allocate the buffer for the audio, and make a copy of the data
    auto newBuffer = SpxAllocSharedAudioBuffer(size);
    memcpy(newBuffer.get(), buffer, size);

    // Store the buffer in our queue
    std::unique_lock<std::mutex> lock(m_mutex);
    m_audioQueue.emplace(newBuffer, size, std::move(m_dataInfo));
    m_cv.notify_all();
}

bool CSpxPushAudioInputStream::WaitForMoreData()
{
    std::unique_lock<std::mutex> lock(m_mutex);
    if (m_audioQueue.empty())
    {
        #ifdef _DEBUG
            while (!m_cv.wait_for(lock, std::chrono::milliseconds(100), [&] { return !m_audioQueue.empty() || m_endOfStream; }))
            {
                SPX_DBG_TRACE_VERBOSE("%s: waiting ...", __FUNCTION__);
            }
        #else
            m_cv.wait(lock, [&] { return !m_audioQueue.empty() || m_endOfStream; });
        #endif
    }
    return !m_audioQueue.empty();
}

void CSpxPushAudioInputStream::SignalEndOfStream()
{
    std::unique_lock<std::mutex> lock(m_mutex);
    SPX_DBG_TRACE_VERBOSE_IF(TURN_ON_VERBOSE_AUDIO_DEBUGGING, "Signal End of Stream is called");
    m_endOfStream = true;
    m_cv.notify_all();
}
} } } } // Microsoft::CognitiveServices::Speech::Impl
