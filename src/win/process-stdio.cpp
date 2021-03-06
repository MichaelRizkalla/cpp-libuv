/* Copyright Joyent, Inc. and other Node contributors. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include <assert.h>
#include <io.h>
#include <stdio.h>
#include <stdlib.h>

#include "uv.h"
#include "internal.h"
#include "handle-inl.h"


/*
 * The `child_stdio_buffer` buffer has the following layout:
 *   int number_of_fds
 *   unsigned char crt_flags[number_of_fds]
 *   HANDLE os_handle[number_of_fds]
 */
#define CHILD_STDIO_SIZE(count)                     \
    (sizeof(int) +                                  \
     sizeof(unsigned char) * (count) +              \
     sizeof(uintptr_t) * (count))

#define CHILD_STDIO_COUNT(buffer)                   \
    *(reinterpret_cast<unsigned int*>(buffer))

#define CHILD_STDIO_CRT_FLAGS(buffer, fd)           \
    *(reinterpret_cast<unsigned char*>(buffer) + sizeof(int) + fd)

#define CHILD_STDIO_HANDLE(buffer, fd)                          \
    *(reinterpret_cast<HANDLE*>(                                \
                reinterpret_cast<unsigned char*>(buffer) +      \
                 sizeof(int) +                                  \
                 sizeof(unsigned char) *                        \
                 CHILD_STDIO_COUNT((buffer)) +                  \
                 sizeof(HANDLE) * (fd)))


/* CRT file descriptor mode flags */
#define FOPEN       0x01
#define FEOFLAG     0x02
#define FCRLF       0x04
#define FPIPE       0x08
#define FNOINHERIT  0x10
#define FAPPEND     0x20
#define FDEV        0x40
#define FTEXT       0x80

/*
 * Clear the HANDLE_FLAG_INHERIT flag from all HANDLEs that were inherited
 * the parent process. Don't check for errors - the stdio handles may not be
 * valid, or may be closed already. There is no guarantee that this function
 * does a perfect job.
 */
void uv_disable_stdio_inheritance(void) {

  /* Make the windows stdio handles non-inheritable. */
  auto handle = GetStdHandle(STD_INPUT_HANDLE);
  if (handle != nullptr && handle != INVALID_HANDLE_VALUE)
    SetHandleInformation(handle, HANDLE_FLAG_INHERIT, 0);

  handle = GetStdHandle(STD_OUTPUT_HANDLE);
  if (handle != nullptr && handle != INVALID_HANDLE_VALUE)
    SetHandleInformation(handle, HANDLE_FLAG_INHERIT, 0);

  handle = GetStdHandle(STD_ERROR_HANDLE);
  if (handle != nullptr && handle != INVALID_HANDLE_VALUE)
    SetHandleInformation(handle, HANDLE_FLAG_INHERIT, 0);

  /* Make inherited CRT FDs non-inheritable. */
  auto si = STARTUPINFOW{};
  GetStartupInfoW(&si);
  if (uv__stdio_verify(si.lpReserved2, si.cbReserved2))
    uv__stdio_noinherit(si.lpReserved2);
}

static int uv__create_stdio_pipe_pair(uv_loop_t* loop,
    uv_pipe_t* server_pipe, HANDLE* child_pipe_ptr, unsigned int flags) {
  char pipe_name[64];
  auto server_access = 0ul;
  auto client_access = 0ul;

  if (flags & UV_READABLE_PIPE) {
    /* The server needs inbound access too, otherwise CreateNamedPipe() won't
     * give us the FILE_READ_ATTRIBUTES permission. We need that to probe the
     * state of the write buffer when we're trying to shutdown the pipe. */
    server_access |= PIPE_ACCESS_OUTBOUND | PIPE_ACCESS_INBOUND;
    client_access |= GENERIC_READ | FILE_WRITE_ATTRIBUTES;
  }
  if (flags & UV_WRITABLE_PIPE) {
    server_access |= PIPE_ACCESS_INBOUND;
    client_access |= GENERIC_WRITE | FILE_READ_ATTRIBUTES;
  }

  /* Create server pipe handle. */
  auto err = uv_stdio_pipe_server(loop,
                             server_pipe,
                             server_access,
                             pipe_name,
                             sizeof(pipe_name));
  if (err){
    if (server_pipe->handle != INVALID_HANDLE_VALUE) {
      uv_pipe_cleanup(loop, server_pipe);
    }
    return err;
  }

  /* Create child pipe handle. */
  auto sa = SECURITY_ATTRIBUTES{};
  sa.nLength = static_cast<decltype(sa.nLength)>(sizeof(decltype(sa)));
  sa.lpSecurityDescriptor = nullptr;
  sa.bInheritHandle = true;

  auto overlap = server_pipe->ipc || (flags & UV_OVERLAPPED_PIPE);
  auto child_pipe = CreateFileA(pipe_name,
                           client_access,
                           0,
                           &sa,
                           OPEN_EXISTING,
                           overlap ? FILE_FLAG_OVERLAPPED : 0,
                           nullptr);
  if (child_pipe == INVALID_HANDLE_VALUE) {
    err = GetLastError();
    if (server_pipe->handle != INVALID_HANDLE_VALUE) {
      uv_pipe_cleanup(loop, server_pipe);
    }
    if (child_pipe != INVALID_HANDLE_VALUE) {
      CloseHandle(child_pipe);
    }
    return err;
  }

#ifndef NDEBUG
  /* Validate that the pipe was opened in the right mode. */
  {
    auto mode = DWORD{};
    auto r = GetNamedPipeHandleState(child_pipe,
                                     &mode,
                                     nullptr,
                                     nullptr,
                                     nullptr,
                                     nullptr,
                                     0);
    assert(r == 1);
    assert(mode == (PIPE_READMODE_BYTE | PIPE_WAIT));
  }
#endif

  /* Do a blocking ConnectNamedPipe. This should not block because we have both
   * ends of the pipe created. */
  if (!ConnectNamedPipe(server_pipe->handle, nullptr)) {
    if (GetLastError() != ERROR_PIPE_CONNECTED) {
      err = GetLastError();
      if (server_pipe->handle != INVALID_HANDLE_VALUE) {
        uv_pipe_cleanup(loop, server_pipe);
      }
      if (child_pipe != INVALID_HANDLE_VALUE) {
        CloseHandle(child_pipe);
      }
      return err;
    }
  }

  /* The server end is now readable and/or writable. */
  if (flags & UV_READABLE_PIPE)
    server_pipe->flags |= UV_HANDLE_WRITABLE;
  if (flags & UV_WRITABLE_PIPE)
    server_pipe->flags |= UV_HANDLE_READABLE;

  *child_pipe_ptr = child_pipe;
  return 0;
}

static int uv__duplicate_handle(uv_loop_t* loop, HANDLE handle, HANDLE* dup) {
  (void)loop;

  /* _get_osfhandle will sometimes return -2 in case of an error. This seems to
   * happen when fd <= 2 and the process' corresponding stdio handle is set to
   * nullptr. Unfortunately DuplicateHandle will happily duplicate (HANDLE) -2, so
   * this situation goes unnoticed until someone tries to use the duplicate.
   * Therefore we filter out known-invalid handles here. */
  if (handle == INVALID_HANDLE_VALUE ||
      handle == nullptr ||
      handle == reinterpret_cast<HANDLE>(-2)) {
    *dup = INVALID_HANDLE_VALUE;
    return ERROR_INVALID_HANDLE;
  }

  auto current_process = GetCurrentProcess();

  if (!DuplicateHandle(current_process,
                       handle,
                       current_process,
                       dup,
                       0,
                       true,
                       DUPLICATE_SAME_ACCESS)) {
    *dup = INVALID_HANDLE_VALUE;
    return GetLastError();
  }

  return 0;
}

static int uv__duplicate_fd(uv_loop_t* loop, int fd, HANDLE* dup) {

  if (fd == -1) {
    *dup = INVALID_HANDLE_VALUE;
    return ERROR_INVALID_HANDLE;
  }

  auto handle = uv__get_osfhandle(fd);
  return uv__duplicate_handle(loop, handle, dup);
}

int uv__create_nul_handle(HANDLE* handle_ptr,
    DWORD access) {

  auto sa = SECURITY_ATTRIBUTES{};
  sa.nLength = static_cast<decltype(sa.nLength)>(sizeof(decltype(sa)));
  sa.lpSecurityDescriptor = nullptr;
  sa.bInheritHandle = true;

  auto handle = CreateFileW(L"NUL",
                       access,
                       FILE_SHARE_READ | FILE_SHARE_WRITE,
                       &sa,
                       OPEN_EXISTING,
                       0,
                       nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return GetLastError();
  }

  *handle_ptr = handle;
  return 0;
}

int uv__stdio_create(uv_loop_t* loop,
                     const uv_process_options_t* options,
                     BYTE** buffer_ptr) {

  auto count = options->stdio_count;

  if (count < 0 || count > 255) {
    /* Only support FDs 0-255 */
    return ERROR_NOT_SUPPORTED;
  } else if (count < 3) {
    /* There should always be at least 3 stdio handles. */
    count = 3;
  }

  /* Allocate the child stdio buffer */
  auto buffer = static_cast<BYTE*>(uv__malloc(CHILD_STDIO_SIZE(count)));
  if (buffer == nullptr) {
    return ERROR_OUTOFMEMORY;
  }

  /* Prepopulate the buffer with INVALID_HANDLE_VALUE handles so we can clean
   * up on failure. */
  CHILD_STDIO_COUNT(buffer) = count;
  for (auto i = 0; i < count; i++) {
    CHILD_STDIO_CRT_FLAGS(buffer, i) = 0;
    CHILD_STDIO_HANDLE(buffer, i) = INVALID_HANDLE_VALUE;
  }

  for (auto i = 0; i < count; i++) {
    auto fdopt = uv_stdio_container_t{};
    if (i < options->stdio_count) {
      fdopt = options->stdio[i];
    } else {
      fdopt.flags = UV_IGNORE;
    }

    switch (fdopt.flags & (UV_IGNORE | UV_CREATE_PIPE | UV_INHERIT_FD |
            UV_INHERIT_STREAM)) {
      case UV_IGNORE:
        /* Starting a process with no stdin/stout/stderr can confuse it. So no
         * matter what the user specified, we make sure the first three FDs are
         * always open in their typical modes, e. g. stdin be readable and
         * stdout/err should be writable. For FDs > 2, don't do anything - all
         * handles in the stdio buffer are initialized with.
         * INVALID_HANDLE_VALUE, which should be okay. */
        if (i <= 2) {
          auto access = (i == 0) ? FILE_GENERIC_READ :
                                    FILE_GENERIC_WRITE | FILE_READ_ATTRIBUTES;

          auto err = uv__create_nul_handle(&CHILD_STDIO_HANDLE(buffer, i),
                                      access);
          if (err){
            uv__stdio_destroy(buffer);
            return err;
          }

          CHILD_STDIO_CRT_FLAGS(buffer, i) = FOPEN | FDEV;
        }
        break;

      case UV_CREATE_PIPE: {
        /* Create a pair of two connected pipe ends; one end is turned into an
         * uv_pipe_t for use by the parent. The other one is given to the
         * child. */
        auto *parent_pipe = reinterpret_cast<uv_pipe_t*>(fdopt.data.stream);

        /* Create a new, connected pipe pair. stdio[i]. stream should point to
         * an uninitialized, but not connected pipe handle. */
        assert(fdopt.data.stream->type == UV_NAMED_PIPE);
        assert(!(fdopt.data.stream->flags & UV_HANDLE_CONNECTION));
        assert(!(fdopt.data.stream->flags & UV_HANDLE_PIPESERVER));

        auto child_pipe = HANDLE{};
        auto err = uv__create_stdio_pipe_pair(loop,
                                         parent_pipe,
                                         &child_pipe,
                                         static_cast<unsigned int>(fdopt.flags));
        if (err){
          uv__stdio_destroy(buffer);
          return err;
        }

        CHILD_STDIO_HANDLE(buffer, i) = child_pipe;
        CHILD_STDIO_CRT_FLAGS(buffer, i) = FOPEN | FPIPE;
        break;
      }

      case UV_INHERIT_FD: {
        /* Inherit a raw FD. */
        auto child_handle = HANDLE{};

        /* Make an inheritable duplicate of the handle. */
        auto err = uv__duplicate_fd(loop, fdopt.data.fd, &child_handle);
        if (err) {
          /* If fdopt. data. fd is not valid and fd <= 2, then ignore the
           * error. */
          if (fdopt.data.fd <= 2 && err == ERROR_INVALID_HANDLE) {
            CHILD_STDIO_CRT_FLAGS(buffer, i) = 0;
            CHILD_STDIO_HANDLE(buffer, i) = INVALID_HANDLE_VALUE;
            break;
          }
          uv__stdio_destroy(buffer);
          return err;
        }

        /* Figure out what the type is. */
        switch (GetFileType(child_handle)) {
          case FILE_TYPE_DISK:
            CHILD_STDIO_CRT_FLAGS(buffer, i) = FOPEN;
            break;

          case FILE_TYPE_PIPE:
            CHILD_STDIO_CRT_FLAGS(buffer, i) = FOPEN | FPIPE;
            break;

          case FILE_TYPE_CHAR:
          case FILE_TYPE_REMOTE:
            CHILD_STDIO_CRT_FLAGS(buffer, i) = FOPEN | FDEV;
            break;

          case FILE_TYPE_UNKNOWN:
            if (GetLastError() != 0) {
              err = GetLastError();
              CloseHandle(child_handle);
              uv__stdio_destroy(buffer);
              return err;
            }
            CHILD_STDIO_CRT_FLAGS(buffer, i) = FOPEN | FDEV;
            break;

          default:
            assert(0);
            return -1;
        }

        CHILD_STDIO_HANDLE(buffer, i) = child_handle;
        break;
      }

      case UV_INHERIT_STREAM: {
        /* Use an existing stream as the stdio handle for the child. */
        auto crt_flags = unsigned char{};
        auto *stream = fdopt.data.stream;
        auto stream_handle = HANDLE{};

        /* Leech the handle out of the stream. */
        if (stream->type == UV_TTY) {
          stream_handle = reinterpret_cast<uv_tty_t*>(stream)->handle;
          crt_flags = FOPEN | FDEV;
        } else if (stream->type == UV_NAMED_PIPE &&
                   stream->flags & UV_HANDLE_CONNECTION) {
          stream_handle = reinterpret_cast<uv_pipe_t*>(stream)->handle;
          crt_flags = FOPEN | FPIPE;
        } else {
          stream_handle = INVALID_HANDLE_VALUE;
          crt_flags = 0;
        }

        if (stream_handle == nullptr ||
            stream_handle == INVALID_HANDLE_VALUE) {
          /* The handle is already closed, or not yet created, or the stream
           * type is not supported. */
          auto err = ERROR_NOT_SUPPORTED;
          uv__stdio_destroy(buffer);
          return err;
        }

        /* Make an inheritable copy of the handle. */
        auto child_handle = HANDLE{};
        auto err = uv__duplicate_handle(loop, stream_handle, &child_handle);
        if (err){
          uv__stdio_destroy(buffer);
          return err;
        }

        CHILD_STDIO_HANDLE(buffer, i) = child_handle;
        CHILD_STDIO_CRT_FLAGS(buffer, i) = crt_flags;
        break;
      }

      default:
        assert(0);
        return -1;
    }
  }

  *buffer_ptr  = buffer;
  return 0;
}

void uv__stdio_destroy(BYTE* buffer) {

  auto count = CHILD_STDIO_COUNT(buffer);
  for (auto i = 0u; i < count; i++) {
    auto handle = CHILD_STDIO_HANDLE(buffer, i);
    if (handle != INVALID_HANDLE_VALUE) {
      CloseHandle(handle);
    }
  }

  uv__free(buffer);
}

void uv__stdio_noinherit(BYTE* buffer) {

  auto count = CHILD_STDIO_COUNT(buffer);
  for (auto i = 0u; i < count; i++) {
    HANDLE handle = CHILD_STDIO_HANDLE(buffer, i);
    if (handle != INVALID_HANDLE_VALUE) {
      SetHandleInformation(handle, HANDLE_FLAG_INHERIT, 0);
    }
  }
}

int uv__stdio_verify(BYTE* buffer, WORD size) {

  /* Check the buffer pointer. */
  if (buffer == nullptr)
    return 0;

  /* Verify that the buffer is at least big enough to hold the count. */
  if (size < CHILD_STDIO_SIZE(0))
    return 0;

  /* Verify if the count is within range. */
  auto count = CHILD_STDIO_COUNT(buffer);
  if (count > 256)
    return 0;

  /* Verify that the buffer size is big enough to hold info for N FDs. */
  if (size < CHILD_STDIO_SIZE(count))
    return 0;

  return 1;
}

WORD uv__stdio_size(BYTE* buffer) {
  return static_cast<WORD>(CHILD_STDIO_SIZE(CHILD_STDIO_COUNT((buffer))));
}

HANDLE uv__stdio_handle(BYTE* buffer, int fd) {
  return CHILD_STDIO_HANDLE(buffer, fd);
}
