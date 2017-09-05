/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "MozQuic.h"
#include "MozQuicInternal.h"
#include "Streams.h"

#include "assert.h"
#include "stdlib.h"
#include "unistd.h"

namespace mozquic  {

uint32_t
StreamState::StartNewStream(MozQuicStreamPair **outStream, const void *data,
                            uint32_t amount, bool fin)
{
  *outStream = new MozQuicStreamPair(mNextStreamId, this, mMozQuic);
  mStreams.insert( { mNextStreamId, *outStream } );
  mNextStreamId += 2;
  if ( amount || fin) {
    return (*outStream)->Write((const unsigned char *)data, amount, fin);
  }
  return MOZQUIC_OK;
}

uint32_t
StreamState::FindStream(uint32_t streamID, std::unique_ptr<MozQuicStreamChunk> &d)
{
  // Open a new stream and implicitly open all streams with ID smaller than
  // streamID that are not already opened.
  while (streamID >= mNextRecvStreamId) {
    fprintf(stderr, "Add new stream %d\n", mNextRecvStreamId);
    MozQuicStreamPair *stream = new MozQuicStreamPair(mNextRecvStreamId,
                                                      this, mMozQuic);
    mStreams.insert( { mNextRecvStreamId, stream } );
    mNextRecvStreamId += 2;
  }

  auto i = mStreams.find(streamID);
  if (i == mStreams.end()) {
    fprintf(stderr, "Stream %d already closed.\n", streamID);
    // this stream is already closed and deleted. Discharge frame.
    d.reset();
    return MOZQUIC_ERR_ALREADY_FINISHED;
  }
  (*i).second->Supply(d);
  if (!(*i).second->Empty() && mMozQuic->mConnEventCB) {
    mMozQuic->mConnEventCB(mMozQuic->mClosure, MOZQUIC_EVENT_NEW_STREAM_DATA, (*i).second);
  }
  return MOZQUIC_OK;
}

void
StreamState::DeleteStream(uint32_t streamID)
{
  fprintf(stderr, "Delete stream %lu\n", streamID);
  mStreams.erase(streamID);
}

uint32_t
StreamState::HandleStreamFrame(FrameHeaderData *result, bool fromCleartext,
                               const unsigned char *pkt, const unsigned char *endpkt,
                               uint32_t &_ptr)
{
  fprintf(stderr,"recv stream %d len=%d offset=%d fin=%d\n",
          result->u.mStream.mStreamID,
          result->u.mStream.mDataLen,
          result->u.mStream.mOffset,
          result->u.mStream.mFinBit);

  if (!result->u.mStream.mStreamID && result->u.mStream.mFinBit) {
    // todo need to respond with a connection error PROTOCOL_VIOLATION 12.2
    mMozQuic->RaiseError(MOZQUIC_ERR_GENERAL, (char *) "fin not allowed on stream 0\n");
    return MOZQUIC_ERR_GENERAL;
  }

  // todo, ultimately the stream chunk could hold references to
  // the packet buffer and _ptr into it for zero copy

  // parser checked for this, but jic
  assert(pkt + _ptr + result->u.mStream.mDataLen <= endpkt);
  std::unique_ptr<MozQuicStreamChunk>
    tmp(new MozQuicStreamChunk(result->u.mStream.mStreamID,
                               result->u.mStream.mOffset,
                               pkt + _ptr,
                               result->u.mStream.mDataLen,
                               result->u.mStream.mFinBit));
  if (!result->u.mStream.mStreamID) {
    mStream0->Supply(tmp);
  } else {
    if (fromCleartext) {
      mMozQuic->RaiseError(MOZQUIC_ERR_GENERAL, (char *) "cleartext non 0 stream id\n");
      return MOZQUIC_ERR_GENERAL;
    }
    uint32_t rv = FindStream(result->u.mStream.mStreamID, tmp);
    if (rv != MOZQUIC_OK) {
      return rv;
    }
  }
  _ptr += result->u.mStream.mDataLen;
  return MOZQUIC_OK;
}

uint32_t
StreamState::ScrubUnWritten(uint32_t streamID)
{
  auto iter = mUnWrittenData.begin();
  while (iter != mUnWrittenData.end()) {
    auto chunk = (*iter).get();
    if (chunk->mStreamID == streamID && !chunk->mRst) {
      iter = mUnWrittenData.erase(iter);
      fprintf(stderr,"scrubbing chunk %p of unwritten id %d\n",
              chunk, streamID);
    } else {
      iter++;
    }
  }

  auto iter2 = mUnAckedData.begin();
  while (iter2 != mUnAckedData.end()) {
    auto chunk = (*iter2).get();
    if (chunk->mStreamID == streamID && !chunk->mRst) {
      iter2 = mUnAckedData.erase(iter2);
      fprintf(stderr,"scrubbing chunk %p of unacked id %d\n",
              chunk, streamID);
    } else {
      iter2++;
    }
  }
  return MOZQUIC_OK;
}

static uint8_t varSize(uint64_t input)
{
  // returns 0->3 depending on magnitude of input
  return (input < 0x100) ? 0 : (input < 0x10000) ? 1 : (input < 0x100000000UL) ? 2 : 3;
}

uint32_t
StreamState::CreateStreamFrames(unsigned char *&framePtr, const unsigned char *endpkt, bool justZero)
{
  auto iter = mUnWrittenData.begin();
  while (iter != mUnWrittenData.end()) {
    if (justZero && (*iter)->mStreamID) {
      iter++;
      continue;
    }
    if ((*iter)->mRst) {
      if (mMozQuic->CreateStreamRst(framePtr, endpkt, (*iter).get()) != MOZQUIC_OK) {
        break;
      }
    } else {
      uint32_t room = endpkt - framePtr;
      if (room < 1) {
        break; // this is only for type, we will do a second check later.
      }

      // 11fssood -> 11000001 -> 0xC1. Fill in fin, offset-len and id-len below dynamically
      auto typeBytePtr = framePtr;
      framePtr[0] = 0xc1;

      // Determine streamId size without varSize becuase we use 24 bit value
      uint32_t tmp32 = (*iter)->mStreamID;
      tmp32 = htonl(tmp32);
      uint8_t idLen = 4;
      for (int i=0; (i < 3) && (((uint8_t*)(&tmp32))[i] == 0); i++) {
        idLen--;
      }

      // determine offset size
      uint64_t offsetValue = PR_htonll((*iter)->mOffset);
      uint8_t offsetSizeType = varSize((*iter)->mOffset);
      uint8_t offsetLen;
      if (offsetSizeType == 0) {
        // 0, 16, 32, 64 instead of usual 8, 16, 32, 64
        if ((*iter)->mOffset) {
          offsetSizeType = 1;
          offsetLen = 2;
        } else {
          offsetLen = 0;
        }
      } else {
        offsetLen = 1 << offsetSizeType;
      }

      // 1(type) + idLen + offsetLen + 2(len) + 1(data)
      if (room < (4 + idLen + offsetLen)) {
        break;
      }

      // adjust the frame type:
      framePtr[0] |= (idLen - 1) << 3;
      assert(!(offsetSizeType & ~0x3));
      framePtr[0] |= (offsetSizeType << 1);
      framePtr++;

      // Set streamId
      memcpy(framePtr, ((uint8_t*)(&tmp32)) + (4 - idLen), idLen);
      framePtr += idLen;

      // Set offset
      if (offsetLen) {
        memcpy(framePtr, ((uint8_t*)(&offsetValue)) + (8 - offsetLen), offsetLen);
        framePtr += offsetLen;
      }

      room -= (3 + idLen + offsetLen); //  1(type) + idLen + offsetLen + 2(len)
      if (room < (*iter)->mLen) {
        // we need to split this chunk. its too big
        // todo iterate on them all instead of doing this n^2
        // as there is a copy involved
        std::unique_ptr<MozQuicStreamChunk>
          tmp(new MozQuicStreamChunk((*iter)->mStreamID,
                                     (*iter)->mOffset + room,
                                     (*iter)->mData.get() + room,
                                     (*iter)->mLen - room,
                                     (*iter)->mFin));
        (*iter)->mLen = room;
        (*iter)->mFin = false;
        auto iterReg = iter++;
        mUnWrittenData.insert(iter, std::move(tmp));
        iter = iterReg;
      }
      assert(room >= (*iter)->mLen);

      // set the len and fin bits after any potential split
      uint16_t tmp16 = (*iter)->mLen;
      tmp16 = htons(tmp16);
      memcpy(framePtr, &tmp16, 2);
      framePtr += 2;

      if ((*iter)->mFin) {
        *typeBytePtr = *typeBytePtr | STREAM_FIN_BIT;
      }

      memcpy(framePtr, (*iter)->mData.get(), (*iter)->mLen);
      fprintf(stderr,"writing a stream %d frame %d @ offset %d [fin=%d] in packet %lX\n",
              (*iter)->mStreamID, (*iter)->mLen, (*iter)->mOffset, (*iter)->mFin,
              mMozQuic->mNextTransmitPacketNumber);
      framePtr += (*iter)->mLen;
    }

    (*iter)->mPacketNumber = mMozQuic->mNextTransmitPacketNumber;
    (*iter)->mTransmitTime = MozQuic::Timestamp();
    if ((mMozQuic->GetConnectionState() == CLIENT_STATE_CONNECTED) ||
        (mMozQuic->GetConnectionState() == SERVER_STATE_CONNECTED) ||
        (mMozQuic->GetConnectionState() == CLIENT_STATE_0RTT)) {
      (*iter)->mTransmitKeyPhase = keyPhase1Rtt;
    } else {
      (*iter)->mTransmitKeyPhase = keyPhaseUnprotected;
    }
    (*iter)->mRetransmitted = false;

    // move it to the unacked list
    std::unique_ptr<MozQuicStreamChunk> x(std::move(*iter));
    mUnAckedData.push_back(std::move(x));
    iter = mUnWrittenData.erase(iter);
  }
  return MOZQUIC_OK;
}

uint32_t
StreamState::Flush(bool forceAck)
{
  if (!mMozQuic->DecodedOK()) {
    mMozQuic->FlushStream0(forceAck);
  }

  if (mUnWrittenData.empty() && !forceAck) {
    return MOZQUIC_OK;
  }

  unsigned char plainPkt[kMaxMTU];
  uint32_t headerLen;
  uint32_t mtu = mMozQuic->mMTU;
  assert(mtu <= kMaxMTU);

  mMozQuic->CreateShortPacketHeader(plainPkt, mtu - kTagLen, headerLen);

  unsigned char *framePtr = plainPkt + headerLen;
  const unsigned char *endpkt = plainPkt + mtu - kTagLen; // reserve 16 for aead tag
  CreateStreamFrames(framePtr, endpkt, false);
  
  uint32_t rv = mMozQuic->ProtectedTransmit(plainPkt, headerLen,
                                            plainPkt + headerLen, framePtr - (plainPkt + headerLen),
                                            mtu - headerLen - kTagLen, true);
  if (rv != MOZQUIC_OK) {
    return rv;
  }

  if (!mUnWrittenData.empty()) {
    return Flush(false);
  }
  return MOZQUIC_OK;
}

uint32_t
StreamState::DoWriter(std::unique_ptr<MozQuicStreamChunk> &p)
{
  // this data gets queued to unwritten and framed and
  // transmitted after prioritization by flush()
  assert (mMozQuic->GetConnectionState() != STATE_UNINITIALIZED);
  
  mUnWrittenData.push_back(std::move(p));

  return MOZQUIC_OK;
}

uint32_t
StreamState::RetransmitTimer()
{
  if (mUnAckedData.empty()) {
    return MOZQUIC_OK;
  }

  // this is a crude stand in for reliability until we get a real loss
  // recovery system built
  uint64_t now = MozQuic::Timestamp();
  uint64_t discardEpoch = now - kForgetUnAckedThresh;

  for (auto i = mUnAckedData.begin(); i != mUnAckedData.end(); ) {
    // just a linear backoff for now
    uint64_t retransEpoch = now - (kRetransmitThresh * (*i)->mTransmitCount);
    if ((*i)->mTransmitTime > retransEpoch) {
      break;
    }
    if (((*i)->mTransmitTime <= discardEpoch) && (*i)->mRetransmitted) {
      // this is only on packets that we are keeping around for timestamp purposes
      fprintf(stderr,"old unacked packet forgotten %lX\n",
              (*i)->mPacketNumber);
      assert(!(*i)->mData);
      i = mUnAckedData.erase(i);
    } else if (!(*i)->mRetransmitted) {
      assert((*i)->mData);
      fprintf(stderr,"data associated with packet %lX retransmitted\n",
              (*i)->mPacketNumber);
      (*i)->mRetransmitted = true;

      // the ctor steals the data pointer
      std::unique_ptr<MozQuicStreamChunk> tmp(new MozQuicStreamChunk(*(*i)));
      assert(!(*i)->mData);
      DoWriter(tmp);
      i++;
    } else {
      i++;
    }
  }

  return MOZQUIC_OK;
}

StreamState::StreamState(MozQuic *q)
  : mMozQuic(q)
  , mNextStreamId(1)
  , mNextRecvStreamId(1)
  , mPeerMaxStreamData(kMaxStreamDataDefault)
  , mPeerMaxData(kMaxDataDefault)
  , mPeerMaxStreamID(kMaxStreamIDDefault)
{
}

}
