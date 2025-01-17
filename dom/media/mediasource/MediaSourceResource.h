/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef MOZILLA_MEDIASOURCERESOURCE_H_
#define MOZILLA_MEDIASOURCERESOURCE_H_

#include "MediaResource.h"
#include "mozilla/Monitor.h"
#include "mozilla/Logging.h"

extern mozilla::LogModule* GetMediaSourceLog();

#define MSE_DEBUG(arg, ...)                                                    \
  MOZ_LOG(                                                                     \
    GetMediaSourceLog(),                                                       \
    mozilla::LogLevel::Debug,                                                  \
    ("MediaSourceResource(%p)::%s: " arg, this, __func__, ##__VA_ARGS__))

#define UNIMPLEMENTED() MSE_DEBUG("UNIMPLEMENTED FUNCTION at %s:%d", __FILE__, __LINE__)

namespace mozilla {

class MediaSourceResource final : public MediaResource
{
public:
  MediaSourceResource()
    : mMonitor("MediaSourceResource")
    , mEnded(false)
  {}

  nsresult ReadAt(int64_t aOffset, char* aBuffer, uint32_t aCount, uint32_t* aBytes) override { UNIMPLEMENTED(); return NS_ERROR_FAILURE; }
  bool ShouldCacheReads() override { UNIMPLEMENTED(); return false; }
  int64_t Tell() override { UNIMPLEMENTED(); return -1; }
  void Pin() override { UNIMPLEMENTED(); }
  void Unpin() override { UNIMPLEMENTED(); }
  int64_t GetLength() override { UNIMPLEMENTED(); return -1; }
  int64_t GetNextCachedData(int64_t aOffset) override { UNIMPLEMENTED(); return -1; }
  int64_t GetCachedDataEnd(int64_t aOffset) override { UNIMPLEMENTED(); return -1; }
  bool IsDataCachedToEndOfResource(int64_t aOffset) override { UNIMPLEMENTED(); return false; }
  nsresult ReadFromCache(char* aBuffer, int64_t aOffset, uint32_t aCount) override { UNIMPLEMENTED(); return NS_ERROR_FAILURE; }

  nsresult GetCachedRanges(MediaByteRangeSet& aRanges) override
  {
    UNIMPLEMENTED();
    aRanges += MediaByteRange(0, GetLength());
    return NS_OK;
  }

  void SetEnded(bool aEnded)
  {
    MonitorAutoLock mon(mMonitor);
    mEnded = aEnded;
  }

private:
  size_t SizeOfExcludingThis(MallocSizeOf aMallocSizeOf) const
  {
    // TODO: track source buffers appended to MediaSource.
    return 0;
  }

  size_t SizeOfIncludingThis(MallocSizeOf aMallocSizeOf) const
  {
    return aMallocSizeOf(this) + SizeOfExcludingThis(aMallocSizeOf);
  }

  Monitor mMonitor;
  bool mEnded; // protected by mMonitor
};

} // namespace mozilla

#undef MSE_DEBUG
#undef UNIMPLEMENTED

#endif /* MOZILLA_MEDIASOURCERESOURCE_H_ */
