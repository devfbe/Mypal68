/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef DecryptThroughputLimit_h
#define DecryptThroughputLimit_h

#include "PlatformDecoderModule.h"
#include "MediaTimer.h"
#include <deque>

namespace mozilla {

// We throttle our decrypt so that we don't decrypt more than a certain
// duration of samples per second. This is to work around bugs in the
// Widevine CDM. See bug 1338924 and bug 1342822.
class DecryptThroughputLimit {
 public:
  explicit DecryptThroughputLimit(AbstractThread* aTargetThread)
      : mThrottleScheduler(aTargetThread) {}

  typedef MozPromise<RefPtr<MediaRawData>, MediaResult, true> ThrottlePromise;

  // Resolves promise after a delay if necessary in order to reduce the
  // throughput of samples sent through the CDM for decryption.
  RefPtr<ThrottlePromise> Throttle(MediaRawData* aSample) {
    // We should only have one decrypt request being processed at once.
    MOZ_RELEASE_ASSERT(!mThrottleScheduler.IsScheduled());

    const TimeDuration WindowSize = TimeDuration::FromSeconds(0.1);
    const TimeDuration MaxThroughput = TimeDuration::FromSeconds(0.2);

    // Forget decrypts that happened before the start of our window.
    const TimeStamp now = TimeStamp::Now();
    while (!mDecrypts.empty() &&
           mDecrypts.front().mTimestamp < now - WindowSize) {
      mDecrypts.pop_front();
    }

    // How much time duration of the media would we have decrypted inside the
    // time window if we did decrypt this block?
    TimeDuration sampleDuration = aSample->mDuration.ToTimeDuration();
    TimeDuration durationDecrypted = sampleDuration;
    for (const DecryptedJob& job : mDecrypts) {
      durationDecrypted += job.mSampleDuration;
    }

    if (durationDecrypted < MaxThroughput) {
      // If we decrypted a sample of this duration, we would *not* have
      // decrypted more than our threshold for max throughput, over the
      // preceding wall time window. So we're safe to proceed with this
      // decrypt.
      mDecrypts.push_back(DecryptedJob({now, sampleDuration}));
      return ThrottlePromise::CreateAndResolve(aSample, __func__);
    }

    // Otherwise, we need to delay until decrypting won't exceed our
    // throughput threshold.

    RefPtr<ThrottlePromise> p = mPromiseHolder.Ensure(__func__);

    TimeDuration delay = durationDecrypted - MaxThroughput;
    TimeStamp target = now + delay;
    RefPtr<MediaRawData> sample(aSample);
    mThrottleScheduler.Ensure(
        target,
        [this, sample, sampleDuration]() {
          mThrottleScheduler.CompleteRequest();
          mDecrypts.push_back(DecryptedJob({TimeStamp::Now(), sampleDuration}));
          mPromiseHolder.Resolve(sample, __func__);
        },
        []() { MOZ_DIAGNOSTIC_ASSERT(false); });

    return p;
  }

  void Flush() {
    mThrottleScheduler.Reset();
    mPromiseHolder.RejectIfExists(NS_ERROR_DOM_MEDIA_CANCELED, __func__);
  }

 private:
  DelayedScheduler mThrottleScheduler;
  MozPromiseHolder<ThrottlePromise> mPromiseHolder;

  struct DecryptedJob {
    TimeStamp mTimestamp;
    TimeDuration mSampleDuration;
  };
  std::deque<DecryptedJob> mDecrypts;
};

}  // namespace mozilla

#endif  // DecryptThroughputLimit_h