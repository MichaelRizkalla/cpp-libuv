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

#include "uv.h"
#include "task.h"
#include <stdio.h>
#include <stdlib.h>
#include "utils/allocator.cpp"
typedef struct {
  uv_write_t req;
  uv_buf_t buf;
} write_req_t;

static uv_loop_t* loop;

static int server_closed;
static stream_type serverType;
static uv_tcp_t tcpServer;
static uv_udp_t udpServer;
static uv_pipe_t pipeServer;
static uv_handle_t* server;
static uv_udp_send_t* send_freelist;

static void after_write(uv_write_t* req, int status);
static void after_read(uv_stream_t*, ssize_t nread, const uv_buf_t* buf);
static void on_close(uv_handle_t* peer);
static void on_server_close(uv_handle_t* handle);
static void on_connection(uv_stream_t*, int status);


static void after_write(uv_write_t* req, int status) {
  write_req_t* wr;

  /* Free the read/write buffer and the request */
  wr = (write_req_t*) req;
  free(wr->buf.base);
  free(wr);

  if (status == 0)
    return;

  fprintf(stderr,
          "uv_write error: %s - %s\n",
          uv_err_name(status),
          uv_strerror(status));
}


static void after_shutdown(uv_shutdown_t* req, int status) {
  uv_close((uv_handle_t*) req->handle, on_close);
  free(req);
}


static void after_read(uv_stream_t* handle,
                       ssize_t nread,
                       const uv_buf_t* buf) {
  int i;
  write_req_t *wr;
  uv_shutdown_t* sreq;

  if (nread < 0) {
    /* Error or EOF */
    ASSERT(nread == UV_EOF);

    free(buf->base);
    sreq = test_create_ptrstruct<uv_shutdown_t>(sizeof(uv_shutdown_t));
    ASSERT(0 == uv_shutdown(sreq, handle, after_shutdown));
    return;
  }

  if (nread == 0) {
    /* Everything OK, but nothing read. */
    free(buf->base);
    return;
  }

  /*
   * Scan for the letter Q which signals that we should quit the server.
   * If we get QS it means close the stream.
   */
  if (!server_closed) {
    for (i = 0; i < nread; i++) {
      if (buf->base[i] == 'Q') {
        if (i + 1 < nread && buf->base[i + 1] == 'S') {
          free(buf->base);
          uv_close((uv_handle_t*)handle, on_close);
          return;
        } else {
          uv_close(server, on_server_close);
          server_closed = 1;
        }
      }
    }
  }

  wr = (write_req_t*) malloc(sizeof *wr);
  ASSERT(wr != nullptr);
  wr->buf = uv_buf_init(buf->base, nread);

  if (uv_write(&wr->req, handle, &wr->buf, 1, after_write)) {
    FATAL("uv_write failed");
  }
}


static void on_close(uv_handle_t* peer) {
  free(peer);
}


static void echo_alloc(uv_handle_t* handle,
                       size_t suggested_size,
                       uv_buf_t* buf) {
  buf->base = test_create_ptrstruct<char>(suggested_size);
  buf->len = suggested_size;
}

static void slab_alloc(uv_handle_t* handle,
                       size_t suggested_size,
                       uv_buf_t* buf) {
  /* up to 16 datagrams at once */
  static char slab[16 * 64 * 1024];
  buf->base = slab;
  buf->len = sizeof(slab);
}

static void on_connection(uv_stream_t* server, int status) {
  uv_stream_t* stream;
  int r;

  if (status != 0) {
    fprintf(stderr, "Connect error %s\n", uv_err_name(status));
  }
  ASSERT(status == 0);

  switch (serverType) {
  case TCP:
    stream = test_create_ptrstruct<uv_stream_t>(sizeof(uv_tcp_t));
    ASSERT(stream != nullptr);
    r = uv_tcp_init(loop, (uv_tcp_t*)stream);
    ASSERT(r == 0);
    break;

  case PIPE:
    stream = test_create_ptrstruct<uv_stream_t>(sizeof(uv_pipe_t));
    ASSERT(stream != nullptr);
    r = uv_pipe_init(loop, (uv_pipe_t*)stream, 0);
    ASSERT(r == 0);
    break;

  default:
    ASSERT(0 && "Bad serverType");
    abort();
  }

  /* associate server with stream */
  stream->data = server;

  r = uv_accept(server, stream);
  ASSERT(r == 0);

  r = uv_read_start(stream, echo_alloc, after_read);
  ASSERT(r == 0);
}


static void on_server_close(uv_handle_t* handle) {
  ASSERT(handle == server);
}

static uv_udp_send_t* send_alloc(void) {
  uv_udp_send_t* req = send_freelist;
  if (req != nullptr)
    send_freelist = static_cast<uv_udp_send_t*>(req->data);
  else
    req = test_create_ptrstruct<uv_udp_send_t>(sizeof(uv_udp_send_t));
  return req;
}

static void on_send(uv_udp_send_t* req, int status) {
  ASSERT(req != nullptr);
  ASSERT(status == 0);
  req->data = send_freelist;
  send_freelist = req;
}

static void on_recv(uv_udp_t* handle,
                    ssize_t nread,
                    const uv_buf_t* rcvbuf,
                    const struct sockaddr* addr,
                    unsigned flags) {
  uv_buf_t sndbuf;

  if (nread == 0) {
    /* Everything OK, but nothing read. */
    return;
  }

  ASSERT(nread > 0);
  ASSERT(addr->sa_family == AF_INET);

  uv_udp_send_t* req = send_alloc();
  ASSERT(req != nullptr);
  sndbuf = uv_buf_init(rcvbuf->base, nread);
  ASSERT(0 <= uv_udp_send(req, handle, &sndbuf, 1, addr, on_send));
}

static int tcp4_echo_start(int port) {
  struct sockaddr_in addr;
  int r;

  ASSERT(0 == uv_ip4_addr("0.0.0.0", port, &addr));

  server = (uv_handle_t*)&tcpServer;
  serverType = TCP;

  r = uv_tcp_init(loop, &tcpServer);
  if (r) {
    /* TODO: Error codes */
    fprintf(stderr, "Socket creation error\n");
    return 1;
  }

  r = uv_tcp_bind(&tcpServer, (const struct sockaddr*) &addr, 0);
  if (r) {
    /* TODO: Error codes */
    fprintf(stderr, "Bind error\n");
    return 1;
  }

  r = uv_listen((uv_stream_t*)&tcpServer, SOMAXCONN, on_connection);
  if (r) {
    /* TODO: Error codes */
    fprintf(stderr, "Listen error %s\n", uv_err_name(r));
    return 1;
  }

  return 0;
}


static int tcp6_echo_start(int port) {
  struct sockaddr_in6 addr6;
  int r;

  ASSERT(0 == uv_ip6_addr("::1", port, &addr6));

  server = (uv_handle_t*)&tcpServer;
  serverType = TCP;

  r = uv_tcp_init(loop, &tcpServer);
  if (r) {
    /* TODO: Error codes */
    fprintf(stderr, "Socket creation error\n");
    return 1;
  }

  /* IPv6 is optional as not all platforms support it */
  r = uv_tcp_bind(&tcpServer, (const struct sockaddr*) &addr6, 0);
  if (r) {
    /* show message but return OK */
    fprintf(stderr, "IPv6 not supported\n");
    return 0;
  }

  r = uv_listen((uv_stream_t*)&tcpServer, SOMAXCONN, on_connection);
  if (r) {
    /* TODO: Error codes */
    fprintf(stderr, "Listen error\n");
    return 1;
  }

  return 0;
}


static int udp4_echo_start(int port) {
  struct sockaddr_in addr;
  int r;

  ASSERT(0 == uv_ip4_addr("127.0.0.1", port, &addr));
  server = (uv_handle_t*)&udpServer;
  serverType = UDP;

  r = uv_udp_init(loop, &udpServer);
  if (r) {
    fprintf(stderr, "uv_udp_init: %s\n", uv_strerror(r));
    return 1;
  }

  r = uv_udp_bind(&udpServer, (const struct sockaddr*) &addr, 0);
  if (r) {
    fprintf(stderr, "uv_udp_bind: %s\n", uv_strerror(r));
    return 1;
  }

  r = uv_udp_recv_start(&udpServer, slab_alloc, on_recv);
  if (r) {
    fprintf(stderr, "uv_udp_recv_start: %s\n", uv_strerror(r));
    return 1;
  }

  return 0;
}


static int pipe_echo_start(char* pipeName) {
  int r;

#ifndef _WIN32
  {
    uv_fs_t req;
    uv_fs_unlink(nullptr, &req, pipeName, nullptr);
    uv_fs_req_cleanup(&req);
  }
#endif

  server = (uv_handle_t*)&pipeServer;
  serverType = PIPE;

  r = uv_pipe_init(loop, &pipeServer, 0);
  if (r) {
    fprintf(stderr, "uv_pipe_init: %s\n", uv_strerror(r));
    return 1;
  }

  r = uv_pipe_bind(&pipeServer, pipeName);
  if (r) {
    fprintf(stderr, "uv_pipe_bind: %s\n", uv_strerror(r));
    return 1;
  }

  r = uv_listen((uv_stream_t*)&pipeServer, SOMAXCONN, on_connection);
  if (r) {
    fprintf(stderr, "uv_pipe_listen: %s\n", uv_strerror(r));
    return 1;
  }

  return 0;
}


HELPER_IMPL(tcp4_echo_server) {
  loop = uv_default_loop();

  if (tcp4_echo_start(TEST_PORT))
    return 1;

  notify_parent_process();
  uv_run(loop, UV_RUN_DEFAULT);
  return 0;
}


HELPER_IMPL(tcp6_echo_server) {
  loop = uv_default_loop();

  if (tcp6_echo_start(TEST_PORT))
    return 1;

  notify_parent_process();
  uv_run(loop, UV_RUN_DEFAULT);
  return 0;
}


HELPER_IMPL(pipe_echo_server) {
  loop = uv_default_loop();

  if (pipe_echo_start(const_cast<char*>(TEST_PIPENAME)))
    return 1;

  notify_parent_process();
  uv_run(loop, UV_RUN_DEFAULT);
  return 0;
}


HELPER_IMPL(udp4_echo_server) {
  loop = uv_default_loop();

  if (udp4_echo_start(TEST_PORT))
    return 1;

  notify_parent_process();
  uv_run(loop, UV_RUN_DEFAULT);
  return 0;
}
