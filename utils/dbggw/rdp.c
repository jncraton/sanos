//
// rdp.c
//
// Copyright (c) 2001 Michael Ringgaard. All rights reserved.
//
// Remote debugging protocol
//

#include <windows.h>
#include <stdio.h>
#include <process.h>

#include "rdp.h"

//
// add_event
//

static struct dbg_event *add_event(struct dbg_session *s)
{
  struct dbg_event *e = (struct dbg_event *) malloc(sizeof(struct dbg_event));
  memset(e, 0, sizeof(struct dbg_event));

  if (s->event_tail) s->event_tail->next = e;
  e->next = NULL;
  s->event_tail = e;
  if (!s->event_head) s->event_head = e;

  return e;
}

//
// get_next_event
//

static struct dbg_event *get_next_event(struct dbg_session *s)
{
  struct dbg_event *e = s->event_head;

  if (!e) return NULL;
  s->event_head = e->next;
  if (!s->event_head) s->event_tail = NULL;

  return e;
}

//
// dbg_send
//

static void dbg_send(struct dbg_session *s, void *buffer, int count)
{
  unsigned long bytes;

  WriteFile(s->target, buffer, count, &bytes, NULL);
}

//
// dbg_recv
//

static void dbg_recv(struct dbg_session *s, void *buffer, int count)
{
  unsigned long bytes;
  char *p = buffer;

  while (count > 0)
  {
    if (!ReadFile(s->target, p, count, &bytes, NULL)) 
    {
      printf("Error %d reading from com port\n", GetLastError());
      return;
    }

    count -= bytes;
    p += bytes;
  }
}

//
// dbg_send_packet
//

static void dbg_send_packet(struct dbg_session *s, int cmd, unsigned char id, void *data, unsigned int len)
{
  unsigned int n;
  struct dbg_hdr hdr;
  unsigned char checksum;
  unsigned char *p;

  hdr.signature = DBG_SIGNATURE;
  hdr.cmd = (unsigned char) cmd;
  hdr.len = len;
  hdr.id = id;
  hdr.checksum = 0;

  checksum = 0;
  p = (unsigned char *) &hdr;
  for (n = 0; n < sizeof(struct dbg_hdr); n++) checksum += *p++;
  p = (unsigned char *) data;
  for (n = 0; n < len; n++) checksum += *p++;
  hdr.checksum = -checksum;

  dbg_send(s, &hdr, sizeof(struct dbg_hdr));
  dbg_send(s, data, len);
}

//
// dbg_recv_packet
//

static int dbg_recv_packet(struct dbg_session *s, struct dbg_hdr *hdr, void *data)
{
  unsigned int n;
  unsigned char checksum;
  unsigned char *p;

  while (1)
  {
    dbg_recv(s, &hdr->signature, 1);
    if (hdr->signature == DBG_SIGNATURE) break;
  }

  dbg_recv(s, &hdr->cmd, 1);
  dbg_recv(s, &hdr->id, 1);
  dbg_recv(s, &hdr->checksum, 1);
  dbg_recv(s, (unsigned char *) &hdr->len, 4);
  if (hdr->len > MAX_DBG_PACKETLEN) return -1;
  dbg_recv(s, data, hdr->len);

  checksum = 0;
  p = (unsigned char *) hdr;
  for (n = 0; n < sizeof(struct dbg_hdr); n++) checksum += *p++;
  p = (unsigned char *) data;
  for (n = 0; n < hdr->len; n++) checksum += *p++;
  if (checksum != 0) return -2;

  return hdr->len;
}

//
// dbg_xact
//

int dbg_xact(struct dbg_session *s, unsigned char cmd, void *reqdata, unsigned int len, void *rspdata)
{
  struct dbg_hdr hdr;
  int rc;
  unsigned char reqid = s->next_reqid++;

  dbg_send_packet(s, cmd, reqid, reqdata, len);
  rc = dbg_recv_packet(s, &hdr, rspdata);
  if (rc < 0)
  {
    printf("Error %d receiving response for command 0x%x\n", rc);
    return rc;
  }

  if (hdr.id != reqid)
  {
    printf("Error: reqid %d expected, got %d\n", reqid, hdr.id);
    return -4;
  }

  if (hdr.cmd >= 0x80)
  {
    printf("Error: debuggee returned error %d %s\n", hdr.cmd, hdr.len > 0 ? (char *) rspdata : "");
    return -hdr.cmd;
  }

  return hdr.cmd;
}

//
// dbg_read_mem
//

int dbg_read_mem(struct dbg_session *s, void *addr, void *buffer, unsigned long size)
{
  struct dbg_memory mem;

  mem.addr = addr;
  mem.size = size;
  return dbg_xact(s, DBGCMD_READ_MEMORY, &mem, sizeof(struct dbg_memory), buffer);
}

//
// dbg_create_session
//

struct dbg_session *dbg_create_session(char *port)
{
  DCB dcb;
  HANDLE target;
  struct dbg_session *s;
  struct dbg_event *e;
  int n;

  // Open and configure port
  target = CreateFile(port, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
  if (target == INVALID_HANDLE_VALUE)
  {
    printf("rdp: error %d opening com port\n", GetLastError());
    return NULL;
  }

  if (!GetCommState(target, &dcb)) return NULL;
  dcb.BaudRate = 115200;
  dcb.ByteSize = DATABITS_8;
  dcb.StopBits = ONESTOPBIT;
  dcb.Parity = NOPARITY;
  if (!SetCommState(target, &dcb)) return NULL;

  // Allocate and initialize new session
  s = (struct dbg_session *) malloc(sizeof(struct dbg_session));
  if (!s) return NULL;
  memset(s, 0, sizeof(struct dbg_session));
  s->target = target;
  s->body = (union dbg_body *) s->dbg_data;

  // Send connect to debug target
  printf("rdp: connecting to debugger...\n");

  s->body->conn.version = DRPC_VERSION;
  dbg_send_packet(s, DBGCMD_CONNECT, s->next_reqid++, s->body, 4);

  if (dbg_recv_packet(s, &s->hdr, s->body) < 0)
  {
    printf("rdp: unable to connect to debugger\n");
    return NULL;
  }

  if (s->hdr.cmd != (DBGCMD_CONNECT | DBGCMD_REPLY))
  {
    printf("rdp: unexpected reply on connect from debugee\n");
    CloseHandle(s->target);
    free(s);
    return NULL;
  }
  s->conn = s->body->conn;

  // Get module list
  if (dbg_xact(s, DBGCMD_GET_MODULES, NULL, 0, s->body) < 0) return NULL;

  for (n = 0; n < s->body->mod.count; n++)
  {
    if (s->body->mod.mods[n].hmod != s->conn.mod.hmod)
    {
      e = add_event(s);
      e->tid = s->conn.trap.tid;
      e->type = DBGEVT_LOAD_MODULE;
      e->evt.load.hmod = s->body->mod.mods[n].hmod;
      e->evt.load.name = s->body->mod.mods[n].name;
    }
  }

  // Get thread list
  if (dbg_xact(s, DBGCMD_GET_THREADS, NULL, 0,s-> body) < 0) return NULL;

  for (n = 0; n < s->body->thl.count; n++)
  {
    if (s->body->thl.threads[n].tid != s->conn.thr.tid)
    {
      e = add_event(s);
      e->tid = s->conn.trap.tid;
      e->type = DBGEVT_CREATE_THREAD;
      e->evt.create.tid = s->body->thl.threads[n].tid;
      e->evt.create.tib = s->body->thl.threads[n].tib;
      e->evt.create.startaddr = s->body->thl.threads[n].startaddr;
    }
  }

  // Add break trap
  e = add_event(s);
  e->type = DBGEVT_TRAP;
  e->tid = s->conn.trap.tid;
  e->evt.trap = s->conn.trap;

  return s;
}

//
// dbg_close_session
//

void dbg_close_session(struct dbg_session *s)
{
  struct dbg_event *e;
  struct dbg_event *next;

  if (s->target != INVALID_HANDLE_VALUE) CloseHandle(s->target);
  
  e = s->event_head;
  while (e)
  {
    next = e->next;
    free(e);
    e = next;
  }

  free(s);
}

//
// dbg_close_session
//

struct dbg_event *dbg_next_event(struct dbg_session *s)
{
  return get_next_event(s);
}

//
// dbg_release_event
//

void dbg_release_event(struct dbg_event *e)
{
  if (e) free(e);
}

//
// dbg_read_memory
//

int dbg_read_memory(struct dbg_session *s, void *addr, int size, void *buffer)
{
  struct dbg_memory mem;
  int rc;

  if (size <= 0 || size >= 4096) 
  {
    printf("rdp: read memory size is %d bytes, truncated\n", size);
    size = 4096;
  }
  mem.addr = addr;
  mem.size = size;

  rc = dbg_xact(s, DBGCMD_READ_MEMORY, &mem, sizeof(struct dbg_memory), buffer);
  return rc;
}

//
// dbg_write_memory
//

int dbg_write_memory(struct dbg_session *s, void *addr, int size, void *buffer)
{
  int rc;

  if (size <= 0 || size >= 4096) 
  {
    printf("rdp: write memory size is %d bytes, truncated\n", size);
    size = 4096;
  }
  s->body->mem.addr = addr;
  s->body->mem.size = size;
  memcpy(s->body->mem.data, buffer, size);

  rc = dbg_xact(s, DBGCMD_WRITE_MEMORY, s->body, sizeof(struct dbg_memory) + size, s->body);
  return rc;
}

//
// dbg_get_context
//

int dbg_get_context(struct dbg_session *s, tid_t tid, struct context *ctxt)
{
  int rc;

  s->body->ctx.tid = tid;
  rc = dbg_xact(s, DBGCMD_GET_THREAD_CONTEXT, s->body, 4, s->body);
  memcpy(ctxt, &s->body->ctx.ctxt, sizeof(struct context));
  return rc;
}

//
// dbg_set_context
//

int dbg_set_context(struct dbg_session *s, tid_t tid, struct context *ctxt)
{
  int rc;

  s->body->ctx.tid = tid;
  memcpy(&s->body->ctx.ctxt, ctxt, sizeof(struct context));
  rc = dbg_xact(s, DBGCMD_SET_THREAD_CONTEXT, s->body, sizeof(struct dbg_context), s->body);
  return rc;
}

//
// dbg_suspend_threads
//

int dbg_suspend_threads(struct dbg_session *s, tid_t *threadids, int count)
{
  int rc;
  int n;

  s->body->thr.count = count;
  memcpy(s->body->thr.threadids, threadids, sizeof(tid_t) * count);

  rc = dbg_xact(s, DBGCMD_SUSPEND_THREAD, s->body, sizeof(struct dbg_thread) + sizeof(tid_t) * count, s->body);
  if (rc < 0) return rc;

  for (n = 0; n < count; n++)
  {
    if (s->body->thr.threadids[n] & 0x80000000)
      threadids[n] = 0;
    else
      threadids[n] = s->body->thr.threadids[n] + 1;
  }

  return rc;
}

//
// dbg_resume_threads
//

int dbg_resume_threads(struct dbg_session *s, tid_t *threadids, int count)
{
  int rc;
  int n;

  s->body->thr.count = count;
  memcpy(s->body->thr.threadids, threadids, sizeof(tid_t) * count);

  rc = dbg_xact(s, DBGCMD_RESUME_THREAD, s->body, sizeof(struct dbg_thread) + sizeof(tid_t) * count, s->body);
  if (rc < 0) return rc;

  for (n = 0; n < count; n++)
  {
    if (s->body->thr.threadids[n] & 0x80000000)
      threadids[n] = 0;
    else
      threadids[n] = s->body->thr.threadids[n] - 1;
  }

  return rc;
}
