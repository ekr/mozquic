/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include "../MozQuic.h"
#include "assert.h"

#define SERVER_NAME "foo.example.com"
#define SERVER_PORT 4433

#if 0

Basic server, does a handshake and waits forever.. it can only handle 1
  session at a time right now.. it will ignore stream data it recvs
  except if it contains a msg of FIN, in which case it will respond
  with a single message and close the stream

  -send-close option will send a close before exiting at 1.5sec

  all connected sessions will be be ping at 30 sec interval.. no response after
  2 seconds closes connection

  About Certificate Verifcation::
The sample/nss-config directory is a sample that can be passed
to mozquic_nss_config(). It contains a NSS database with a cert
and key for foo.example.com that is signed by a CA defined by CA.cert.der.

#endif

#define SEND_CLOSE_TIMEOUT_MS 1500
#define TIMEOUT_CLIENT_MS 30000

int send_close = 0;
int connected = 0;

static int accept_new_connection(mozquic_connection_t *nc);

int close_connection(mozquic_connection_t *c)
{
  connected--;
  assert(connected >= 0);
  return mozquic_destroy_connection(c);
}
  
static int connEventCB(void *closure, uint32_t event, void *param)
{
  switch (event) {
  case MOZQUIC_EVENT_NEW_STREAM_DATA:
  {
    mozquic_stream_t *stream = param;
    char buf[100];
    int finStream = 0;
    uint32_t read = 0;
    int fin = 0;
    int line = 0;
    do {
      uint32_t code = mozquic_recv(stream, buf, 100, &read, &fin);
      if (code != MOZQUIC_OK) {
        fprintf(stderr,"Read stream error %d\n", code);
        return MOZQUIC_OK;
      } else if (read > 0) {
        if (!line) {
          fprintf(stderr,"Data:\n");
        }
        line++;
        buf[read] = '\0';
        if (strcmp(buf, "FIN") == 0) {
          finStream = 1;
        }
        fprintf(stderr,"[%s] fin=%d\n", buf, fin);
      } else if (fin) {
        fprintf(stderr,"fin=%d\n", fin);
      }
    } while (read > 0);
    if (finStream) {
      char msg[] = "Server sending data.";
      mozquic_send(stream, msg, strlen(msg), 1);
    }
    return MOZQUIC_OK;
  }
  case MOZQUIC_EVENT_STREAM_RESET:
  {
    // todo not implemented yet.
    mozquic_stream_t *stream = param;
    fprintf(stderr,"Stream was reset\n");
    return MOZQUIC_OK;
  }

  case MOZQUIC_EVENT_ACCEPT_NEW_CONNECTION:
    return accept_new_connection(param);

  case MOZQUIC_EVENT_CLOSE_CONNECTION:
  case MOZQUIC_EVENT_ERROR:
    // todo this leaks the 64bit int allocation
    return close_connection(param);

  case MOZQUIC_EVENT_IO:
    if (!closure) 
      return MOZQUIC_OK;
    {
      uint32_t *i = closure;
      mozquic_connection_t *conn = param;
      *i += 1;
      if (send_close && (*i == SEND_CLOSE_TIMEOUT_MS)) {
        fprintf(stderr,"server terminating connection\n");
        close_connection(param);
        free(i);
      } else if (!(*i % TIMEOUT_CLIENT_MS)) {
        fprintf(stderr,"server testing conn\n");
        mozquic_check_peer(param, 2000);
      }
      return MOZQUIC_OK;
    }

  default:
    fprintf(stderr,"unhandled event %X\n", event);
  }
  return MOZQUIC_OK;
}

static int accept_new_connection(mozquic_connection_t *nc)
{
  uint32_t *ctr = malloc (sizeof (uint32_t));
  *ctr = 0;
  mozquic_set_event_callback(nc, connEventCB);
  mozquic_set_event_callback_closure(nc, ctr);
  connected++;
  return MOZQUIC_OK;
}

int
has_arg(int argc, char **argv, char *test)
{
  int i;
  for (i=0; i < argc; i++) {
    if (!strcasecmp(argv[i], test)) {
      return 1;
    }
  }
  return 0;
}

int main(int argc, char **argv)
{
  uint32_t i = 0;
  uint32_t delay = 1000;
  struct mozquic_config_t config;
  mozquic_connection_t *c;

  send_close = has_arg(argc, argv, "-send-close");
  
  char *cdir = getenv ("MOZQUIC_NSS_CONFIG");
  if (mozquic_nss_config(cdir) != MOZQUIC_OK) {
    fprintf(stderr,"MOZQUIC_NSS_CONFIG FAILURE [%s]\n", cdir ? cdir : "");
    exit (-1);
  }
  
  memset(&config, 0, sizeof(config));
  config.originName = SERVER_NAME;
  config.originPort = SERVER_PORT;
  config.tolerateBadALPN = 1;
  config.handleIO = 0; // todo mvp

  mozquic_new_connection(&c, &config);
  mozquic_set_event_callback(c, connEventCB);
  mozquic_start_server(c);

  do {
    usleep (delay); // this is for handleio todo
    if (!(i++ & 0xf)) {
      char p;
      assert(connected >= 0);
      if (!connected) {
        p = '.';
        delay = 5000;
      } else if (connected < 10) {
        p = '0' + connected;
        delay = 1000;
      } else {
        p = '*';
        delay = 1000;
      }
      fprintf(stderr,"%c",p);
      fflush(stderr);
    }
    mozquic_IO(c);
  } while (1);
  
}
