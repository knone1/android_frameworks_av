/*
 * Copyright (C) 2009 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

//#define LOG_NDEBUG 0
#define LOG_TAG "AudioPlayer"
#include <utils/Log.h>

#include <binder/IPCThreadState.h>
#include <media/AudioTrack.h>
#ifdef INTEL_MUSIC_OFFLOAD_FEATURE
#include <media/AudioTrackOffload.h>
#endif
#include <media/stagefright/foundation/ADebug.h>
#include <media/stagefright/foundation/ALooper.h>
#include <media/stagefright/AudioPlayer.h>
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/MediaErrors.h>
#include <media/stagefright/MediaSource.h>
#include <media/stagefright/MetaData.h>
#include <media/stagefright/Utils.h>

#include "include/AwesomePlayer.h"

namespace android {

AudioPlayer::AudioPlayer(
        const sp<MediaPlayerBase::AudioSink> &audioSink,
        bool allowDeepBuffering,
        AwesomePlayer *observer)
    : mAudioTrack(NULL),
      mInputBuffer(NULL),
      mSampleRate(0),
      mLatencyUs(0),
      mFrameSize(0),
      mNumFramesPlayed(0),
      mNumFramesPlayedSysTimeUs(ALooper::GetNowUs()),
      mPositionTimeMediaUs(-1),
      mPositionTimeRealUs(-1),
      mSeeking(false),
      mReachedEOS(false),
      mFinalStatus(OK),
      mStarted(false),
      mIsFirstBuffer(false),
      mFirstBufferResult(OK),
      mFirstBuffer(NULL),
      mAudioSink(audioSink),
      mAllowDeepBuffering(allowDeepBuffering),
      mObserver(observer),
      mPinnedTimeUs(-1ll) {
#ifdef INTEL_MUSIC_OFFLOAD_FEATURE
      mOffload = false;
      mOffloadFormat = AUDIO_FORMAT_PCM_16_BIT;
      mStartPos = 0;
      mOffloadPostEOSPending = false;
#endif
}

AudioPlayer::AudioPlayer(
        audio_format_t audioFormat,
        const sp<MediaPlayerBase::AudioSink> &audioSink,
        uint32_t flags,
        AwesomePlayer *observer)
    : mAudioTrack(NULL),
      mInputBuffer(NULL),
      mSampleRate(0),
      mLatencyUs(0),
      mFrameSize(0),
      mNumFramesPlayed(0),
      mNumFramesPlayedSysTimeUs(ALooper::GetNowUs()),
      mPositionTimeMediaUs(-1),
      mPositionTimeRealUs(-1),
      mSeeking(false),
      mReachedEOS(false),
      mFinalStatus(OK),
      mStarted(false),
      mIsFirstBuffer(false),
      mFirstBufferResult(OK),
      mFirstBuffer(NULL),
      mAudioSink(audioSink),
      mAllowDeepBuffering(flags & ALLOW_DEEP_BUFFERING),
      mObserver(observer),
      mPinnedTimeUs(-1ll)
{
#ifdef INTEL_MUSIC_OFFLOAD_FEATURE
      mOffload = flags & USE_OFFLOAD;
      mOffloadFormat = audioFormat;
      mStartPos = 0;
      mOffloadPostEOSPending = false;
#endif
}

AudioPlayer::~AudioPlayer() {
    if (mStarted) {
        reset();
    }
}

void AudioPlayer::setSource(const sp<MediaSource> &source) {
    CHECK(mSource == NULL);
    mSource = source;
}

status_t AudioPlayer::start(bool sourceAlreadyStarted) {
    CHECK(!mStarted);
    CHECK(mSource != NULL);

    status_t err;
    if (!sourceAlreadyStarted) {
        err = mSource->start();

        if (err != OK) {
            return err;
        }
    }

    // We allow an optional INFO_FORMAT_CHANGED at the very beginning
    // of playback, if there is one, getFormat below will retrieve the
    // updated format, if there isn't, we'll stash away the valid buffer
    // of data to be used on the first audio callback.

    CHECK(mFirstBuffer == NULL);

    MediaSource::ReadOptions options;
    if (mSeeking) {
        options.setSeekTo(mSeekTimeUs);
        mSeeking = false;
    }

    mFirstBufferResult = mSource->read(&mFirstBuffer, &options);
    if (mFirstBufferResult == INFO_FORMAT_CHANGED) {
        ALOGV("INFO_FORMAT_CHANGED!!!");

        CHECK(mFirstBuffer == NULL);
        mFirstBufferResult = OK;
        mIsFirstBuffer = false;
    } else {
        mIsFirstBuffer = true;
    }

    sp<MetaData> format = mSource->getFormat();
    const char *mime;
    bool success = format->findCString(kKeyMIMEType, &mime);
    CHECK(success);

#ifdef INTEL_MUSIC_OFFLOAD_FEATURE
    CHECK( mOffload || (!strcasecmp(mime, MEDIA_MIMETYPE_AUDIO_RAW)) );
#else
    CHECK(!strcasecmp(mime, MEDIA_MIMETYPE_AUDIO_RAW));
#endif

    success = format->findInt32(kKeySampleRate, &mSampleRate);
    CHECK(success);

    int32_t numChannels, channelMask;
    success = format->findInt32(kKeyChannelCount, &numChannels);
    CHECK(success);

    if(!format->findInt32(kKeyChannelMask, &channelMask)) {
        // log only when there's a risk of ambiguity of channel mask selection
        ALOGI_IF(numChannels > 2,
                "source format didn't specify channel mask, using (%d) channel order", numChannels);
        channelMask = CHANNEL_MASK_USE_CHANNEL_ORDER;
    }

#ifdef BGM_ENABLED
    String8 reply;
    char* bgmKVpair;

    reply =  AudioSystem::getParameters(0,String8(AudioParameter::keyBGMState));
    bgmKVpair = strpbrk((char *)reply.string(), "=");
    ALOGV("%s [BGMUSIC] bgmKVpair = %s",__func__,bgmKVpair);
    ++bgmKVpair;
    mAllowBackgroundPlayback = strcmp(bgmKVpair,"true") ? false : true;
    ALOGD("%s [BGMUSIC] mAllowBackgroundPlayback = %d",__func__,mAllowBackgroundPlayback);
#endif

#ifdef INTEL_MUSIC_OFFLOAD_FEATURE
    int avgBitRate = -1;
    success = format->findInt32(kKeyBitRate, &avgBitRate);
    if (mAudioSink.get() != NULL) {
        if (mOffload) {
            ALOGV("Opening compress offload sink");
            err = mAudioSink->open(
                mSampleRate, numChannels, channelMask,
                avgBitRate,
                mOffloadFormat,
                DEFAULT_AUDIOSINK_BUFFERCOUNT,
                &AudioPlayer::AudioSinkCallback,
                this,
                AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD);
        } else {
#ifdef BGM_ENABLED
           if((mAllowBackgroundPlayback) &&(!mAllowDeepBuffering)) {
              err = mAudioSink->open(
                  mSampleRate, numChannels, channelMask, AUDIO_FORMAT_PCM_16_BIT,
                  DEFAULT_AUDIOSINK_BUFFERCOUNT,
                  &AudioPlayer::AudioSinkCallback,
                  this,
                  (mAllowBackgroundPlayback ?
                            AUDIO_OUTPUT_FLAG_REMOTE_BGM :
                            AUDIO_OUTPUT_FLAG_NONE));
           } else {
#endif
              err = mAudioSink->open(
                  mSampleRate, numChannels, channelMask, AUDIO_FORMAT_PCM_16_BIT,
                  DEFAULT_AUDIOSINK_BUFFERCOUNT,
                  &AudioPlayer::AudioSinkCallback,
                  this,
                  (mAllowDeepBuffering ?
                            AUDIO_OUTPUT_FLAG_DEEP_BUFFER :
                            AUDIO_OUTPUT_FLAG_NONE));
#ifdef BGM_ENABLED
          }
#endif
        }
#else
    if (mAudioSink.get() != NULL) {
#ifdef BGM_ENABLED
       if((mAllowBackgroundPlayback) &&(!mAllowDeepBuffering)) {
          status_t  err = mAudioSink->open(
                        mSampleRate, numChannels, channelMask, AUDIO_FORMAT_PCM_16_BIT,
                        DEFAULT_AUDIOSINK_BUFFERCOUNT,
                        &AudioPlayer::AudioSinkCallback,
                        this,
                        (mAllowBackgroundPlayback ?
                            AUDIO_OUTPUT_FLAG_REMOTE_BGM :
                            AUDIO_OUTPUT_FLAG_NONE));
       } else {
#endif
           status_t err = mAudioSink->open(
                        mSampleRate, numChannels, channelMask, AUDIO_FORMAT_PCM_16_BIT,
                        DEFAULT_AUDIOSINK_BUFFERCOUNT,
                        &AudioPlayer::AudioSinkCallback,
                        this,
                        (mAllowDeepBuffering ?
                            AUDIO_OUTPUT_FLAG_DEEP_BUFFER :
                            AUDIO_OUTPUT_FLAG_NONE));
#ifdef BGM_ENABLED
       }
#endif

#endif
        if (err != OK) {
            if (mFirstBuffer != NULL) {
                mFirstBuffer->release();
                mFirstBuffer = NULL;
            }

            if (!sourceAlreadyStarted) {
                mSource->stop();
            }

            return err;
        }

        mLatencyUs = (int64_t)mAudioSink->latency() * 1000;
        mFrameSize = mAudioSink->frameSize();

#ifdef INTEL_MUSIC_OFFLOAD_FEATURE
        if (mOffload) {
            sendMetaDataToHal(mAudioSink, format);
        }
#endif

        mAudioSink->start();
    } else {
        // playing to an AudioTrack, set up mask if necessary
        audio_channel_mask_t audioMask = channelMask == CHANNEL_MASK_USE_CHANNEL_ORDER ?
                audio_channel_out_mask_from_count(numChannels) : channelMask;
        if (0 == audioMask) {
            return BAD_VALUE;
        }

#ifdef INTEL_MUSIC_OFFLOAD_FEATURE
        if (mOffload) {
            mAudioTrack = new AudioTrackOffload(
                AUDIO_STREAM_MUSIC, avgBitRate, mSampleRate, mOffloadFormat, audioMask,
                0, AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD, &AudioCallback, this, 0, 0);
        } else {
            mAudioTrack = new AudioTrack(
                AUDIO_STREAM_MUSIC, mSampleRate, AUDIO_FORMAT_PCM_16_BIT, audioMask,
                0, AUDIO_OUTPUT_FLAG_NONE, &AudioCallback, this, 0);

        }
#else
        mAudioTrack = new AudioTrack(
                AUDIO_STREAM_MUSIC, mSampleRate, AUDIO_FORMAT_PCM_16_BIT, audioMask,
                0, AUDIO_OUTPUT_FLAG_NONE, &AudioCallback, this, 0);
#endif
        if ((err = mAudioTrack->initCheck()) != OK) {
            delete mAudioTrack;
            mAudioTrack = NULL;

            if (mFirstBuffer != NULL) {
                mFirstBuffer->release();
                mFirstBuffer = NULL;
            }

            if (!sourceAlreadyStarted) {
                mSource->stop();
            }

            return err;
        }

        mLatencyUs = (int64_t)mAudioTrack->latency() * 1000;
        mFrameSize = mAudioTrack->frameSize();

        mAudioTrack->start();
    }

    mStarted = true;
    mPinnedTimeUs = -1ll;

    mOffloadPostEOSPending = false;
    return OK;
}

void AudioPlayer::pause(bool playPendingSamples) {
    CHECK(mStarted);

    if (playPendingSamples) {
        if (mAudioSink.get() != NULL) {
            mAudioSink->stop();
        } else {
            mAudioTrack->stop();
        }

        mNumFramesPlayed = 0;
        mNumFramesPlayedSysTimeUs = ALooper::GetNowUs();
    } else {
        if (mAudioSink.get() != NULL) {
            mAudioSink->pause();
        } else {
            mAudioTrack->pause();
        }

        mPinnedTimeUs = ALooper::GetNowUs();
    }
}

void AudioPlayer::resume() {
    CHECK(mStarted);

    if (mAudioSink.get() != NULL) {
        mAudioSink->start();
    } else {
        mAudioTrack->start();
    }
}

void AudioPlayer::reset() {
    CHECK(mStarted);

    if (mAudioSink.get() != NULL) {
        mAudioSink->stop();
        mAudioSink->close();
    } else {
        mAudioTrack->stop();

        delete mAudioTrack;
        mAudioTrack = NULL;
    }

    // Make sure to release any buffer we hold onto so that the
    // source is able to stop().

    if (mFirstBuffer != NULL) {
        mFirstBuffer->release();
        mFirstBuffer = NULL;
    }

    if (mInputBuffer != NULL) {
        ALOGV("AudioPlayer releasing input buffer.");

        mInputBuffer->release();
        mInputBuffer = NULL;
    }

    mSource->stop();

    // The following hack is necessary to ensure that the OMX
    // component is completely released by the time we may try
    // to instantiate it again.
    wp<MediaSource> tmp = mSource;
    mSource.clear();
    while (tmp.promote() != NULL) {
        usleep(1000);
    }
    IPCThreadState::self()->flushCommands();

    mNumFramesPlayed = 0;
    mNumFramesPlayedSysTimeUs = ALooper::GetNowUs();
    mPositionTimeMediaUs = -1;
    mPositionTimeRealUs = -1;
    mSeeking = false;
    mReachedEOS = false;
    mFinalStatus = OK;
    mStarted = false;
#ifdef INTEL_MUSIC_OFFLOAD_FEATURE
    mStartPos = 0;
    mOffload = false;
    mOffloadPostEOSPending = false;
#endif
}

// static
void AudioPlayer::AudioCallback(int event, void *user, void *info) {
    static_cast<AudioPlayer *>(user)->AudioCallback(event, info);
}

bool AudioPlayer::isSeeking() {
    Mutex::Autolock autoLock(mLock);
    return mSeeking;
}

bool AudioPlayer::reachedEOS(status_t *finalStatus) {
    *finalStatus = OK;

    Mutex::Autolock autoLock(mLock);
    *finalStatus = mFinalStatus;
    return mReachedEOS;
}

status_t AudioPlayer::setPlaybackRatePermille(int32_t ratePermille) {
    if (mAudioSink.get() != NULL) {
        return mAudioSink->setPlaybackRatePermille(ratePermille);
    } else if (mAudioTrack != NULL){
        return mAudioTrack->setSampleRate(ratePermille * mSampleRate / 1000);
    } else {
        return NO_INIT;
    }
}

void AudioPlayer::notifyAudioEOS() {
#ifdef INTEL_MUSIC_OFFLOAD_FEATURE
    if (mObserver != NULL) {
        mObserver->postAudioEOS(NULL);
        ALOGV("Notified observer about end of stream! ");
    }
#endif
}

// static
size_t AudioPlayer::AudioSinkCallback(
        MediaPlayerBase::AudioSink *audioSink,
        void *buffer, size_t size, void *cookie) {
    AudioPlayer *me = (AudioPlayer *)cookie;
    return me->fillBuffer(buffer, size);
}

// static
size_t AudioPlayer::AudioSinkCallback(
        MediaPlayerBase::AudioSink *audioSink,
        void *buffer, size_t size, void *cookie,
        MediaPlayerBase::AudioSink::cb_event_t event) {
#ifdef INTEL_MUSIC_OFFLOAD_FEATURE
     AudioPlayer *me = (AudioPlayer *)cookie;

    if(event == MediaPlayerBase::AudioSink::CB_EVENT_FILL_BUFFER) {
        return me->fillBuffer(buffer, size);
    } else if( event == MediaPlayerBase::AudioSink::CB_EVENT_STREAM_END ) {
        ALOGV("AudioSinkCallback: stream end");
        me->mReachedEOS = true;
        me->mOffloadPostEOSPending = false;
        me->notifyAudioEOS();
    } else if( event ==  MediaPlayerBase::AudioSink::CB_EVENT_TEAR_DOWN ) {
        ALOGV("AudioSinkCallback: Tear down event");
        me->mObserver->postAudioOffloadTearDown();
    }
    return 0;
#endif
    return 0;
}

void AudioPlayer::AudioCallback(int event, void *info) {
#ifdef INTEL_MUSIC_OFFLOAD_FEATURE
    switch (event) {
    case AudioTrack::EVENT_MORE_DATA:
    {
        AudioTrack::Buffer *buffer = (AudioTrack::Buffer *)info;
        size_t numBytesWritten = fillBuffer(buffer->raw, buffer->size);

        buffer->size = numBytesWritten;
    } break;
    case AudioTrackOffload::EVENT_STREAM_END:
    {
        if (mOffload) {
            mReachedEOS = true;
            mOffloadPostEOSPending = false;
            notifyAudioEOS();
        }
    }   break;
    default:
        ALOGE("received unknown event type: %d inside CallbackWrapper !", event);
        break;
    }
#else
    if (event != AudioTrack::EVENT_MORE_DATA) {
        return;
    }

    AudioTrack::Buffer *buffer = (AudioTrack::Buffer *)info;
    size_t numBytesWritten = fillBuffer(buffer->raw, buffer->size);

    buffer->size = numBytesWritten;

#endif
}

uint32_t AudioPlayer::getNumFramesPendingPlayout() const {
    uint32_t numFramesPlayedOut;
    status_t err;

    if (mAudioSink != NULL) {
        err = mAudioSink->getPosition(&numFramesPlayedOut);
    } else {
#ifdef INTEL_MUSIC_OFFLOAD_FEATURE
        if (mOffload) {
            err = static_cast<AudioTrackOffload*>(mAudioTrack)->getPosition(&numFramesPlayedOut);
        } else {
            err = mAudioTrack->getPosition(&numFramesPlayedOut);
        }
#else
        err = mAudioTrack->getPosition(&numFramesPlayedOut);
#endif
    }

    if (err != OK || mNumFramesPlayed < numFramesPlayedOut) {
        return 0;
    }

    // mNumFramesPlayed is the number of frames submitted
    // to the audio sink for playback, but not all of them
    // may have played out by now.
    return mNumFramesPlayed - numFramesPlayedOut;
}

size_t AudioPlayer::fillBuffer(void *data, size_t size) {
    if (mNumFramesPlayed == 0) {
        ALOGV("AudioCallback");
    }

    if (mReachedEOS) {
        return 0;
    }

    bool postSeekComplete = false;
    bool postEOS = false;
    int64_t postEOSDelayUs = 0;

    size_t size_done = 0;
    size_t size_remaining = size;
    while (size_remaining > 0) {
        MediaSource::ReadOptions options;

        {
            Mutex::Autolock autoLock(mLock);

            if (mSeeking) {
                if (mIsFirstBuffer) {
                    if (mFirstBuffer != NULL) {
                        mFirstBuffer->release();
                        mFirstBuffer = NULL;
                    }
                    mIsFirstBuffer = false;
                }

                options.setSeekTo(mSeekTimeUs);

                if (mInputBuffer != NULL) {
                    mInputBuffer->release();
                    mInputBuffer = NULL;
                }

                mSeeking = false;
                if (mObserver) {
                    postSeekComplete = true;
                }
            }
        }

        if (mInputBuffer == NULL) {
            status_t err;

            if (mIsFirstBuffer) {
                mInputBuffer = mFirstBuffer;
                mFirstBuffer = NULL;
                err = mFirstBufferResult;

                mIsFirstBuffer = false;
            } else {
                err = mSource->read(&mInputBuffer, &options);
            }

            CHECK((err == OK && mInputBuffer != NULL)
                   || (err != OK && mInputBuffer == NULL));

            Mutex::Autolock autoLock(mLock);

            if (err != OK) {
                if (mObserver && !mReachedEOS) {
#ifdef INTEL_MUSIC_OFFLOAD_FEATURE
                    if (mOffload) {
                        ALOGV("fillBuffer: mOffload ReachedEOS");
                        mOffloadPostEOSPending = true;
                        if (mAudioSink != NULL) {
                            mAudioSink->setOffloadEOSReached(mOffloadPostEOSPending);
                        } else {
                            static_cast<AudioTrackOffload*>(mAudioTrack)->setOffloadEOSReached(mOffloadPostEOSPending);
                        }
                    } else {
#endif

                        // We don't want to post EOS right away but only
                        // after all frames have actually been played out.
                        // These are the number of frames submitted to the
                        // AudioTrack that you haven't heard yet.
                        uint32_t numFramesPendingPlayout =
                            getNumFramesPendingPlayout();

                        // These are the number of frames we're going to
                        // submit to the AudioTrack by returning from this
                        // callback.
                        uint32_t numAdditionalFrames = size_done / mFrameSize;

                        numFramesPendingPlayout += numAdditionalFrames;

                        int64_t timeToCompletionUs =
                            (1000000ll * numFramesPendingPlayout) / mSampleRate;

                        ALOGV("total number of frames played: %lld (%lld us)",
                            (mNumFramesPlayed + numAdditionalFrames),
                            1000000ll * (mNumFramesPlayed + numAdditionalFrames)
                                / mSampleRate);

                        ALOGV("%d frames left to play, %lld us (%.2f secs)",
                             numFramesPendingPlayout,
                             timeToCompletionUs, timeToCompletionUs / 1E6);

                        postEOS = true;
                        if (mAudioSink->needsTrailingPadding()) {
                            postEOSDelayUs = timeToCompletionUs + mLatencyUs;
                        } else {
                            postEOSDelayUs = 0;
                        }
#ifdef INTEL_MUSIC_OFFLOAD_FEATURE
                   }
#endif
              }

                mReachedEOS = true;
                mFinalStatus = err;
                break;
            }

            if (mAudioSink != NULL) {
                mLatencyUs = (int64_t)mAudioSink->latency() * 1000;
            } else {
                mLatencyUs = (int64_t)mAudioTrack->latency() * 1000;
            }

            if(mInputBuffer->range_length() != 0) {
               //check for non zero buffer
               CHECK(mInputBuffer->meta_data()->findInt64(
                           kKeyTime, &mPositionTimeMediaUs));

            }
            mPositionTimeRealUs =
                ((mNumFramesPlayed + size_done / mFrameSize) * 1000000)
                    / mSampleRate;

            ALOGV("buffer->size() = %d, "
                 "mPositionTimeMediaUs=%.2f mPositionTimeRealUs=%.2f",
                 mInputBuffer->range_length(),
                 mPositionTimeMediaUs / 1E6, mPositionTimeRealUs / 1E6);
#ifdef INTEL_MUSIC_OFFLOAD_FEATURE
             // need to adjust the mStartPos for offload decoding since parser
            // might not be able to get the exact seek time requested.
            if (mSeeking && mOffload) {
                mSeeking = false;
                if (mObserver) {
                    ALOGV("fillBuffer is going to post SEEK_COMPLETE");
                    mObserver->postAudioSeekComplete();
                }

                mStartPos = mPositionTimeMediaUs;
                ALOGV("adjust seek time to: %.2f", mStartPos/ 1E6);
            }
#endif
        }

        if (mInputBuffer->range_length() == 0) {
            mInputBuffer->release();
            mInputBuffer = NULL;

            continue;
        }

        size_t copy = size_remaining;
        if (copy > mInputBuffer->range_length()) {
            copy = mInputBuffer->range_length();
        }

        memcpy((char *)data + size_done,
               (const char *)mInputBuffer->data() + mInputBuffer->range_offset(),
               copy);

        mInputBuffer->set_range(mInputBuffer->range_offset() + copy,
                                mInputBuffer->range_length() - copy);

        size_done += copy;
        size_remaining -= copy;
    }

    {
        Mutex::Autolock autoLock(mLock);
        mNumFramesPlayed += size_done / mFrameSize;
        mNumFramesPlayedSysTimeUs = ALooper::GetNowUs();

        if (mReachedEOS) {
            mPinnedTimeUs = mNumFramesPlayedSysTimeUs;
        } else {
            mPinnedTimeUs = -1ll;
        }
    }

    if (postEOS) {
        mObserver->postAudioEOS(postEOSDelayUs);
    }

    if (postSeekComplete) {
        mObserver->postAudioSeekComplete();
    }

    return size_done;
}

int64_t AudioPlayer::getRealTimeUs() {
    Mutex::Autolock autoLock(mLock);
    return getRealTimeUsLocked();
}

int64_t AudioPlayer::getRealTimeUsLocked() const {
    CHECK(mStarted);
    CHECK_NE(mSampleRate, 0);
    int64_t result = -mLatencyUs + (mNumFramesPlayed * 1000000) / mSampleRate;

    // Compensate for large audio buffers, updates of mNumFramesPlayed
    // are less frequent, therefore to get a "smoother" notion of time we
    // compensate using system time.
    int64_t diffUs;
    if (mPinnedTimeUs >= 0ll) {
        diffUs = mPinnedTimeUs;
    } else {
        diffUs = ALooper::GetNowUs();
    }

    diffUs -= mNumFramesPlayedSysTimeUs;

    return result + diffUs;
}

int64_t AudioPlayer::getMediaTimeUs() {
    Mutex::Autolock autoLock(mLock);

    if (mPositionTimeMediaUs < 0 || mPositionTimeRealUs < 0) {
        if (mSeeking) {
            return mSeekTimeUs;
        }

        return 0;
    }
#ifdef INTEL_MUSIC_OFFLOAD_FEATURE
    if (mOffload) {
        // For Offload as the bufferSizes are huge, query the timestamp
        // from HAL. This is used for updating the progress bar.
        int64_t  renderedDuration = 0;
        uint32_t mediaPosition = 0;
        if (mAudioSink != NULL) {
            mAudioSink->getPosition(&mediaPosition);
        } else {
            static_cast<AudioTrackOffload*>(mAudioTrack)->getPosition(&mediaPosition);
        }
        renderedDuration = 1000 * (int64_t) mediaPosition;
        renderedDuration += mStartPos;
        return renderedDuration;
    }
#endif
    int64_t realTimeOffset = getRealTimeUsLocked() - mPositionTimeRealUs;
    if (realTimeOffset < 0) {
        realTimeOffset = 0;
    }

    return mPositionTimeMediaUs + realTimeOffset;
}

bool AudioPlayer::getMediaTimeMapping(
        int64_t *realtime_us, int64_t *mediatime_us) {
    Mutex::Autolock autoLock(mLock);

    *realtime_us = mPositionTimeRealUs;
    *mediatime_us = mPositionTimeMediaUs;

    return mPositionTimeRealUs != -1 && mPositionTimeMediaUs != -1;
}

status_t AudioPlayer::seekTo(int64_t time_us) {
    Mutex::Autolock autoLock(mLock);

    mSeeking = true;
    mPositionTimeRealUs = mPositionTimeMediaUs = -1;
    mReachedEOS = false;
    mSeekTimeUs = time_us;
#ifdef INTEL_MUSIC_OFFLOAD_FEATURE
    if (mOffload)
        mStartPos = time_us;
#endif

    // Flush resets the number of played frames
    mNumFramesPlayed = 0;
    mNumFramesPlayedSysTimeUs = ALooper::GetNowUs();

    if (mAudioSink != NULL) {
        mAudioSink->flush();
    } else {
        mAudioTrack->flush();
    }

    return OK;
}

}
