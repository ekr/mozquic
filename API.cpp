/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

static int mozQuicInit = 0;

#include "Logging.h"
#include "MozQuic.h"
#include "MozQuicInternal.h"
#include "NSSHelper.h"
#include "Streams.h"

#include <assert.h>
#include <strings.h>

#ifdef __cplusplus
extern "C" {
#endif

struct mozquic_internal_config_t 
{
  unsigned int greaseVersionNegotiation; // flag
  unsigned int ignorePKI; // flag
  unsigned int tolerateBadALPN; // flag
  unsigned int tolerateNoTransportParams; // flag
  unsigned int sabotageVN; // flag
  unsigned int forceAddressValidation; // flag
  uint64_t streamWindow;
  uint64_t connWindowKB;
};
  
uint32_t mozquic_unstable_api1(struct mozquic_config_t *c, const char *name, uint64_t arg1, uint64_t arg2)
{
  assert(sizeof(mozquic_internal_config_t) <= sizeof(c->reservedInternally));
  mozquic_internal_config_t *internal = (mozquic_internal_config_t *) c->reservedInternally + 0;

  if (!strcasecmp(name, "greaseVersionNegotiation")) {
    internal->greaseVersionNegotiation = arg1;
  } else if (!strcasecmp(name, "ignorePKI")) {
    internal->ignorePKI = arg1;
  } else if (!strcasecmp(name, "tolerateBadALPN")) {
    internal->tolerateBadALPN = arg1;
  } else if (!strcasecmp(name, "tolerateNoTransportParams")) {
    internal->tolerateNoTransportParams = arg1;
  } else if (!strcasecmp(name, "sabotageVN")) {
    internal->sabotageVN = arg1;
  } else if (!strcasecmp(name, "forceAddressValidation")) {
    internal->forceAddressValidation = arg1;
  } else if (!strcasecmp(name, "streamWindow")) {
    internal->streamWindow = arg1;
  } else if (!strcasecmp(name, "connWindowKB")) {
    internal->connWindowKB = arg1;
  } else {
    return MOZQUIC_ERR_GENERAL;
  }

  return MOZQUIC_OK;
}

uint32_t mozquic_unstable_api2(mozquic_connection_t *c, const char *name, uint64_t, uint64_t)
{
  return MOZQUIC_ERR_GENERAL;
}
  
int mozquic_new_connection(mozquic_connection_t **outConnection,
                           struct mozquic_config_t *inConfig)
{
  assert(sizeof(mozquic_internal_config_t) <= sizeof(inConfig->reservedInternally));
  mozquic_internal_config_t *internal = (mozquic_internal_config_t *) inConfig->reservedInternally + 0;
  if (!mozQuicInit) {
    int rv = mozquic::NSSHelper::Init(nullptr);
    if (rv != MOZQUIC_OK) {
      return rv;
    }
  }

  if (!outConnection || !inConfig) {
    return MOZQUIC_ERR_INVALID;
  }

  if (!inConfig->originName) {
    return MOZQUIC_ERR_INVALID;
  }

  mozquic::MozQuic *q = new mozquic::MozQuic(inConfig->handleIO);
  if (!q) {
    return MOZQUIC_ERR_GENERAL;
  }
  *outConnection = (void *)q;

  q->SetClosure(inConfig->closure);
  q->SetConnEventCB(inConfig->connection_event_callback);
  q->SetOriginPort(inConfig->originPort);
  q->SetOriginName(inConfig->originName);
  if (internal->greaseVersionNegotiation) {
    q->GreaseVersionNegotiation();
  }
  if (internal->tolerateBadALPN) {
    q->SetTolerateBadALPN();
  }
  if (internal->tolerateNoTransportParams) {
    q->SetTolerateNoTransportParams();
  }
  if (internal->sabotageVN) {
    q->SetSabotageVN();
  }
  if (internal->forceAddressValidation) {
    q->SetForceAddressValidation();
  }
  if (inConfig->appHandlesSendRecv) {
    q->SetAppHandlesSendRecv();
  }
  if (inConfig->appHandlesLogging) {
    q->SetAppHandlesLogging();
  }
  if (internal->ignorePKI) {
    q->SetIgnorePKI();
  }
  if (internal->streamWindow) {
    q->SetStreamWindow(internal->streamWindow);
  }
    if (internal->connWindowKB) {
    q->SetConnWindowKB(internal->connWindowKB);
  }
  
  unsigned char empty[128];
  memset(empty, 0, 128);
  assert(sizeof(empty) == sizeof(inConfig->statelessResetKey));
  if (memcmp(empty, inConfig->statelessResetKey, 128)) {
    q->SetStatelessResetKey(inConfig->statelessResetKey);
  }
  return MOZQUIC_OK;
}

int mozquic_destroy_connection(mozquic_connection_t *conn)
{
  mozquic::MozQuic *self(reinterpret_cast<mozquic::MozQuic *>(conn));
  self->Destroy(0, "");
  return MOZQUIC_OK;
}

int mozquic_start_client(mozquic_connection_t *conn)
{
  mozquic::MozQuic *self(reinterpret_cast<mozquic::MozQuic *>(conn));
  return self->StartClient();
}

int mozquic_start_server(mozquic_connection_t *conn)
{
  mozquic::MozQuic *self(reinterpret_cast<mozquic::MozQuic *>(conn));
  return self->StartServer();
}

int mozquic_start_backpressure(mozquic_connection_t *conn)
{
  mozquic::MozQuic *self(reinterpret_cast<mozquic::MozQuic *>(conn));
  self->StartBackPressure();
  return MOZQUIC_OK;
}

int mozquic_release_backpressure(mozquic_connection_t *conn)
{
  mozquic::MozQuic *self(reinterpret_cast<mozquic::MozQuic *>(conn));
  self->ReleaseBackPressure();
  return MOZQUIC_OK;
}
  
int mozquic_start_new_stream(mozquic_stream_t **outStream,
                             mozquic_connection_t *conn, void *data,
                             uint32_t amount,
                             int fin)
{
  mozquic::MozQuic *self(reinterpret_cast<mozquic::MozQuic *>(conn));
  mozquic::StreamPair *stream;
  int rv = self->StartNewStream(&stream, data, amount, fin);
  if (!rv) {
    *outStream = (void *)stream;
  }
  return rv;
}

int mozquic_send(mozquic_stream_t *stream, void *data, uint32_t amount,
                 int fin)
{
  mozquic::StreamPair *self(reinterpret_cast<mozquic::StreamPair *>(stream));
  int rv = self->Write((const unsigned char *)data, amount, fin);
  if (fin) {
    self->mMozQuic->MaybeDeleteStream(self);
  }
  return rv;
}

int mozquic_end_stream(mozquic_stream_t *stream)
{
  mozquic::StreamPair *self(reinterpret_cast<mozquic::StreamPair *>(stream));
  int rv = self->EndStream();
  self->mMozQuic->MaybeDeleteStream(self);
  return rv;
}

int mozquic_reset_stream(mozquic_stream_t *stream)
{
  mozquic::StreamPair *self(reinterpret_cast<mozquic::StreamPair *>(stream));
  int rv = self->RstStream(mozquic::ERROR_CANCELLED);
  self->mMozQuic->MaybeDeleteStream(self);
  return rv;
}

int mozquic_stop_sending(mozquic_stream_t *stream)
{
  mozquic::StreamPair *self(reinterpret_cast<mozquic::StreamPair *>(stream));
  int rv = self->StopSending(mozquic::ERROR_CANCELLED);
  self->mMozQuic->MaybeDeleteStream(self);
  return rv;
}

int mozquic_recv(mozquic_stream_t *stream, void *data, uint32_t avail,
                 uint32_t *amount, int *fin)
{
  mozquic::StreamPair *self(reinterpret_cast<mozquic::StreamPair *>(stream));
  bool f;
  uint32_t a;
  int rv = self->Read((unsigned char *)data, avail, a, f);
  *fin = f;
  *amount = a;
  if (f) {
    self->mMozQuic->MaybeDeleteStream(self);
  }
  return rv;
}

int mozquic_set_event_callback(mozquic_connection_t *conn, int (*fx)(mozquic_connection_t *, uint32_t event, void * param))
{
  mozquic::MozQuic *self(reinterpret_cast<mozquic::MozQuic *>(conn));
  self->SetConnEventCB(fx);
  return MOZQUIC_OK;
}

int mozquic_set_event_callback_closure(mozquic_connection_t *conn,
                                       void *closure)
{
  mozquic::MozQuic *self(reinterpret_cast<mozquic::MozQuic *>(conn));
  self->SetClosure(closure);
  return MOZQUIC_OK;
}

int mozquic_IO(mozquic_connection_t *conn)
{
  mozquic::MozQuic *self(reinterpret_cast<mozquic::MozQuic *>(conn));
  return self->IO();
}

mozquic_socket_t mozquic_osfd(mozquic_connection_t *conn)
{
  mozquic::MozQuic *self(reinterpret_cast<mozquic::MozQuic *>(conn));
  return self->GetFD();
}

void mozquic_setosfd(mozquic_connection_t *conn, mozquic_socket_t fd)
{
  mozquic::MozQuic *self(reinterpret_cast<mozquic::MozQuic *>(conn));
  self->SetFD(fd);
}

void mozquic_handshake_output(mozquic_connection_t *conn,
                              unsigned char *data, uint32_t data_len)
{
  mozquic::MozQuic *self(reinterpret_cast<mozquic::MozQuic *>(conn));
  self->HandshakeOutput(data, data_len);
}

void mozquic_handshake_complete(mozquic_connection_t *conn, uint32_t errCode,
                                struct mozquic_handshake_info *keyInfo)
{
  assert(false);
  mozquic::MozQuic *self(reinterpret_cast<mozquic::MozQuic *>(conn));
  self->HandshakeComplete(errCode, keyInfo);
}

int mozquic_nss_config(char *dir)
{
  if (mozQuicInit) {
    return MOZQUIC_ERR_GENERAL;
  }
  mozQuicInit = 1;
  if (!dir) {
    return MOZQUIC_ERR_INVALID;
  }

  return mozquic::NSSHelper::Init(dir);
}

int mozquic_check_peer(mozquic_connection_t *conn, uint32_t deadlineMs)
{
  mozquic::MozQuic *self(reinterpret_cast<mozquic::MozQuic *>(conn));
  return self->CheckPeer(deadlineMs);
}

int mozquic_get_streamid(mozquic_stream_t *stream)
{
  return (reinterpret_cast<mozquic::StreamPair *>(stream))->mStreamID;
}

namespace mozquic  {

static const uint32_t kMozQuicVersionGreaseC = 0xfa1a7a3a;

void
MozQuic::GreaseVersionNegotiation()
{
  assert(mConnectionState == STATE_UNINITIALIZED);
  ConnectionLog5("applying version grease\n");
  mVersion = kMozQuicVersionGreaseC;
}

bool
MozQuic::IgnorePKI()
{
  return mIgnorePKI || mIsLoopback;
}

void
MozQuic::SetOriginName(const char *name)
{
  mOriginName.reset(new char[strlen(name) + 1]);
  strcpy (mOriginName.get(), name);
}

}


#ifdef __cplusplus
}
#endif
