//
// file.c
//
// Copyright (c) 2001 Michael Ringgaard. All rights reserved.
//
// File I/O
//

#include "msvcrt.h"

#define _NSTREAM_ 128

crtapi struct _iobuf _iob[_NSTREAM_];

struct critsect iob_lock;

#define stdin  (&_iob[0])
#define stdout (&_iob[1])
#define stderr (&_iob[2])

int vsprintf(char *buf, const char *fmt, va_list args);

static FILE *alloc_stream()
{
  FILE *stream;

  enter(&iob_lock);
  stream = _iob;
  while (stream < _iob + _NSTREAM_) 
  {
    if (stream->flag == 0)
    {
      stream->flag = -1;
      leave(&iob_lock);
      return stream;
    }

    stream++;
  }
  leave(&iob_lock);

  return NULL;
}

static void free_stream(FILE *stream)
{
  stream->flag = 0;
}

int _open(const char *filename, int oflag)
{
  TRACE("_open");
  syslog(LOG_DEBUG, "_open(%s,%p)\n", filename, oflag);
  return open(filename, oflag);
}

int _close(int handle)
{
  TRACE("_close");
  return close(handle);
}

int _read(int handle, void *buffer, unsigned int count)
{
  TRACE("_read");
  return read(handle, buffer, count);
}

int _write(int handle, const void *buffer, unsigned int count)
{
  unsigned int rc;

  TRACE("_write");
  //syslog(LOG_DEBUG, "_write %d bytes to %p\n", count, handle);
  
  rc = write(handle, buffer, count);
  if (rc != count) panic("error writing to file");
  return rc;
}

int _setmode(int handle, int mode)
{
  TRACE("_setmode");
  // TODO: check that mode is O_BINARY
  return 0;
}

int _stat(const char *path, struct _stat *buffer)
{
  int rc;
  struct stat fs;

  TRACE("_stat");
  //syslog(LOG_DEBUG, "stat on %s\n", path);

  rc = stat(path, &fs);
  if (rc < 0) 
  {
    errno = rc;
    return -1;
  }

  if (buffer)
  {
    memset(buffer, 0, sizeof(struct _stat));
    buffer->st_atime = fs.atime;
    buffer->st_ctime = fs.ctime;
    buffer->st_mtime = fs.mtime;
    buffer->st_size = fs.quad.size_low;
    buffer->st_mode = S_IREAD | S_IWRITE | S_IEXEC;
    if (fs.mode & FS_DIRECTORY)
      buffer->st_mode |= S_IFDIR;
    else
      buffer->st_mode |= S_IFREG;
  }

  //syslog(LOG_DEBUG, "%s: mode=%d size=%d\n", path, buffer->st_mode, buffer->st_size);
  return 0;
}

__int64 _stati64(const char *path, struct _stati64 *buffer)
{
  int rc;
  struct stat fs;

  TRACE("_stati64");
  //syslog(LOG_DEBUG, "stat on %s\n", path);

  rc = stat(path, &fs);
  if (rc < 0) return -1;

  if (buffer)
  {
    memset(buffer, 0, sizeof(struct _stati64));
    buffer->st_atime = fs.atime;
    buffer->st_ctime = fs.ctime;
    buffer->st_mtime = fs.mtime;
    buffer->st_size = fs.size;
    buffer->st_mode = S_IREAD | S_IWRITE | S_IEXEC;
    if (fs.mode & FS_DIRECTORY)
      buffer->st_mode |= S_IFDIR;
    else
      buffer->st_mode |= S_IFREG;
  }

  //syslog(LOG_DEBUG, "%s: mode=%d size=%d\n", path, buffer->st_mode, buffer->st_size);
  return 0;
}

__int64 _fstati64(int handle, struct _stati64 *buffer)
{
  int rc;
  struct stat fs;

  TRACE("_fstati64");
  rc = fstat(handle, &fs);
  if (rc < 0) return -1;

  if (buffer)
  {
    memset(buffer, 0, sizeof(struct _stati64));
    buffer->st_atime = fs.atime;
    buffer->st_ctime = fs.ctime;
    buffer->st_mtime = fs.mtime;
    buffer->st_size = fs.size;
    buffer->st_mode = S_IREAD | S_IWRITE | S_IEXEC;
    if (fs.mode & FS_DIRECTORY)
      buffer->st_mode |= S_IFDIR;
    else
      buffer->st_mode |= S_IFREG;
  }

  return 0;
}

__int64 _lseeki64(int handle, __int64 offset, int origin)
{
  TRACE("_lseeki64");
  return lseek(handle, (int) offset, origin);
}

int _open_osfhandle(long osfhandle, int flags)
{
  TRACE("_open_osfhandle");
  return dup(osfhandle);
}

long _get_osfhandle(int filehandle)
{
  TRACE("_get_osfhandle");
  return filehandle;
}

int _getdrive()
{
  TRACE("_getdrive");
  // Drive C is current drive
  return 3;
}

char *_getdcwd(int drive, char *buffer, int maxlen)
{
  TRACE("_getdcwd");
  strcpy(buffer, "c:\\");
  return buffer;
}

char *_fullpath(char *abspath, const char *relpath, size_t maxlen)
{
  TRACE("_fullpath");
  canonicalize(relpath, abspath, maxlen);
  //syslog(LOG_DEBUG, "fullpath: abspath: %s relpath: %s\n", abspath, relpath);
  return abspath;
}

int _rename(const char *oldname, const char *newname)
{
  TRACE("_rename");
  if (rename(oldname, newname) < 0) return -1;
  return 0;
}

int _access(const char *path, int mode)
{
  struct stat fs;
  int rc;

  TRACE("_access");
  //syslog(LOG_DEBUG, "access check on %s\n", path);

  rc = stat(path, &fs);
  if (rc < 0) return -1;
  return 0;
}

int _mkdir(const char *dirname)
{
  TRACE("_mkdir");
  return mkdir(dirname);
}

FILE *fopen(const char *filename, const char *mode)
{
  FILE *stream; 
  int oflag;
  handle_t handle;

  TRACE("fopen");
  syslog(LOG_DEBUG, "fopen(%s,%s)\n", filename, mode);

  switch (mode[0])
  {
    case 'r':
      oflag = O_RDONLY;
      break;

    case 'w':
      oflag = O_WRONLY | O_CREAT | O_TRUNC;
      break;

    case 'a':
      oflag = O_WRONLY | O_CREAT | O_APPEND;
      break;
  
    default:
      return NULL;
  }

  if (mode[1] == '+')
  {
    oflag |= O_RDWR;
    oflag &= ~(O_RDONLY | O_WRONLY);
  }

  handle = open(filename, oflag);
  if (handle < 0) return NULL;

  stream = alloc_stream();
  if (stream == NULL) panic("too many files open");

  stream->file = handle;
  return stream;
}

int fclose(FILE *stream)
{
  TRACE("fclose");
  close(stream->file);
  free_stream(stream);
  return 0;
}

int fflush(FILE *stream)
{
  TRACE("fflush");
  //syslog(LOG_DEBUG, "fflush(%d)\n", stream->file);
  if (flush(stream->file) < 0) return -1;
  return 0;
}

int getc(FILE *stream)
{
  char ch;

  TRACE("getc");
  //syslog(LOG_DEBUG, "getc on %p\n", stream - _iob);
  if (read(stream->file, &ch, 1) < 0) return EOF;
  return 0;
}

int fputc(int c, FILE *stream)
{
  char ch;

  TRACE("fputc");
  ch = c;
  if (write(stream->file, &ch, 1) < 0) return -1;
  return 0;
}

char *fgets(char *string, int n, FILE *stream)
{
  TRACE("fgets");
  panic("fgets not implemented");
  return NULL;
}

int fprintf(FILE *stream, const char *fmt, ...)
{
  va_list args;
  int n;
  char buffer[1024];

  TRACEX("fprintf");
  va_start(args, fmt);
  n = vsprintf(buffer, fmt, args);
  va_end(args);
  return write(stream->file, buffer, n);
}

int vfprintf(FILE *stream, const char *fmt, va_list args)
{
  int n;
  char buffer[1024];

  TRACEX("vfprintf");
  n = vsprintf(buffer, fmt, args);
  
  //syslog(LOG_DEBUG, "vfprintf called (fileno = %d) (%s)\n", stream - _iob, buffer);

  return write(stream->file, buffer, n);
}

int putchar(int c)
{
  char ch;

  TRACEX("putchar");
  ch = c;
  write(1, &ch, 1);
  return c;
}

void init_fileio()
{
  int i;

  mkcs(&iob_lock);

  memset(_iob, 0, sizeof(struct _iobuf) * _NSTREAM_);
  for (i = 0; i < _NSTREAM_; i++) _iob[i].file = NOHANDLE;

  stdin->file = 0;
  stdin->flag = -1;
  stdout->file = 1;
  stdout->flag = -1;
  stderr->file = 2;
  stderr->flag = -1;
}
