/*
 * Copyright (c) 2017-2018 Dong Han
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the names of the authors or the names of any contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHORS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <HsFFI.h>  // for HsInt
#include <Rts.h>
#include <assert.h>
#include <stdlib.h> // for malloc, free, etc.
#include <string.h> // for fs path
#include <uv.h>

#if !defined(_WIN32)

#include <unistd.h> 

#if defined(__sun)
# include <sys/port.h>
# include <port.h>
#endif /* __sun */

#if defined(_AIX)
# define reqevents events
# define rtnevents revents
# include <sys/poll.h>
#else
# include <poll.h>
#endif /* _AIX */

#include <dirent.h>

#endif /* _WIN32 */

////////////////////////////////////////////////////////////////////////////////
// CONSTANT
#define INIT_LOOP_SIZE 128
#define INIT_LOOP_SIZE_BIT 7

#if defined(__linux__) && defined(SO_REUSEPORT)
#define SO_REUSEPORT_LOAD_BALANCE 1
#else
#define SO_REUSEPORT_LOAD_BALANCE 0
#endif

// this function will be a noop when SO_REUSEPORT_LOAD_BALANCE == 0
int hs_set_socket_reuse(uv_stream_t* server);

////////////////////////////////////////////////////////////////////////////////
// error handling
int uv_translate_sys_error(int sys_errno);

////////////////////////////////////////////////////////////////////////////////
// loop
//
// the memory pool item struct, we use the largest possible value,
// except uv_getnameinfo_t, which is too large than average :(
// anyway we don't do getaddrinfo or getaddrname in libuv's thread pool
//
// following size data is from libuv v1.12, in bytes
//                     win  unix
// uv_timer_t        : 160  152
// uv_prepare_t      : 120  120
// uv_check_t        : 120  120
// uv_idle_t         : 120  120
// uv_async_t        : 224  128
// uv_poll_t         : 416  160
// uv_signal_t       : 264  152
// uv_process_t      : 264  136
// uv_tcp_t          : 320  248
// uv_pipe_t         : 576  264
// uv_tty_t          : 344  312
// uv_udp_t          : 424  216
// uv_fs_event_t     : 272  136
// uv_fs_poll_t      : 104  104
// uv_req_t          : 112  64
// uv_getaddrinfo_t  : 216  160
// uv_getnameinfo_t  : 1368 1320  !too large
// uv_shutdown_t     : 128  80
// uv_write_t        : 176  192
// uv_connect_t      : 128  96
// uv_udp_send_t     : 128  320
// uv_fs_t           : 456  440
// uv_work_t         : 176  128
// uv_random_t       : 144  144
typedef union {
    uv_timer_t          timer_t      ;
    uv_prepare_t        prepare_t    ;
    uv_check_t          check_t      ;
    uv_idle_t           idle_t       ;
    uv_async_t          async_t      ;
    uv_poll_t           poll_t       ;
    uv_signal_t         signal_t     ;
    uv_process_t        process_t    ;
    uv_tcp_t            tcp_t        ;
    uv_pipe_t           pipe_t       ;
    uv_tty_t            tty_t        ;
    uv_udp_t            udp_t        ;
    uv_fs_event_t       fs_event_t   ;
    uv_fs_poll_t        fs_poll_t    ;
    uv_req_t            req_t        ;
//    uv_getaddrinfo_t    getaddrinfo_t;
//    uv_getnameinfo_t    getnameinfo_t; !too large
    uv_shutdown_t       shutdown_t   ;
    uv_write_t          write_t      ;
    uv_connect_t        connect_t    ;
    uv_udp_send_t       udp_send_t   ;
    uv_fs_t             fs_t         ;
    uv_work_t           work_t       ;
    uv_random_t         random_t     ;
} hs_uv_struct;

typedef struct {
    // following two fields record events during uv_run, inside callback which
    // wants to record a event, push the handler's slot into the queue
    HsInt   event_counter;
    HsInt*  event_queue;
    // following two fields provide buffers allocated in haskell to uv_alloc_cb,
    // the buffer_size_table are also used to record operation's result
    char**  buffer_table;
    HsInt*  buffer_size_table;
    // following fields are used to implemented a stable slot allocator
    // see note below
    HsInt*  slot_table;
    HsInt   free_slot;
    HsInt*  free_slot_queue;
    HsInt   free_slot_counter;
    // uv_struct_table field is a memory pool for uv_handle_t / uv_req_t struct,
    // see note below
    hs_uv_struct**  uv_struct_table;
    HsInt   size;
    size_t  resize;
    // following fields are handlers used to wake up event loop under threaded and
    // non-threaded RTS respectively.
    uv_async_t* async;
    uv_timer_t* timer;
} hs_loop_data;
// Note: the slot allocator
//
// we used to do slot allocation in haskell, but doing it in C allow us to free slot
// in the right place, e.g. uv_close_cb. The allocator use the same algorithm with
// ghc's stable pointer table. When initialized, it's looked like:
//
//  slot_table->[0][1][2][3]...[INIT_LOOP_SIZE-1]
//               |  |  |  |
//              =1 =2 =3 =4 ...
//  free_slot = 0
//
// Every time we allocate a slot, we return current free_slot, and set the free_slot to
// slot_table[free_slot], so after 3 allocation, it's looked like
//
//  slot_table->[0][1][2][3]...[INIT_LOOP_SIZE-1]
//               |  |  |  |
//              =1 =2 =3 =4 ...
//  free_slot = 3
//
// When we free slot x, we set slot_table[x] to current free_slot, and free_slot
// to x. Now let's say we return slot 1, it will be looked like
//
//  slot_table->[0][1][2][3]...[INIT_LOOP_SIZE-1]
//               |  |  |  |
//              =1 =3 =3 =4 ...
//  free_slot = 1
//
// Next time allocation will give 1 back to us, and free_slot will continue point to 3.
//
// But in practice, we never directly return a free slot back to slot_table, because
// the haskell thread allocating slot may be paused by RTS before its takeMVar parking its
// TSO since parking itself need an allocation. Now if uv_run fired its callback and 
// free the slot, next registration will got the same slot, and mess up with previous
// haskell thread. In order to solve this race condition, we free a slot in two steps:
//
// 1. free_slot will first push slot to free_slot_queue, and increase free_slot_counter.
// 2. before uv_run we loop through this queue and put these free slots back to slot_table.
//
// With this scheme, slots will be held one scheduler loop longer, thus ensure the haskell
// thread can successfully wait on its 'MVar', and in turn make sure it will be resumed.
//
////////////////////////////////////////////////////////////////////////////////
//
// Note: uv_handle_t/uv_req_t memory pool
//
// The reasons to use a memory pool for uv structs are:
//
// 1. A malloc/free per request is inefficient.
// 2. It's diffcult to manange if we use haskell heap(pinned).
// 3. uv structs are small mostly.
//
// The memory pool is grow on demand, when initialized, it's looked like:
//
// uv_struct_table [0]
//                  |
//                  V
//      struct_block[INIT_LOOP_SIZE]
//
// After several loop data resize, it will be looked like:
//
// uv_struct_table [0]                [1]                [2] ...
//                  |                  |                  |
//                  V                  |                  |
//      struct_block[INIT_LOOP_SIZE]   V                  |
//                        struct_block[INIT_LOOP_SIZE]    V
//                                            struct_block[INIT_LOOP_SIZE*2]
//                                                                            .
//                                                                            .
//                                                                            .
// That is, we keep the total number of items sync with loop->data->size, while
// not touching previously allocated blocks, so that the structs are never moved.
//
// Check fetch_uv_struct below to see how do we get struct's address by slot.

uv_loop_t* hs_uv_loop_init(HsInt siz);
void hs_uv_loop_close(uv_loop_t* loop);
HsInt alloc_slot(hs_loop_data* loop_data);
void free_slot(hs_loop_data* loop_data, HsInt slot);
hs_uv_struct* fetch_uv_struct(hs_loop_data* loop_data, HsInt slot);
int hs_uv_run(uv_loop_t* loop, uv_run_mode mode);

////////////////////////////////////////////////////////////////////////////////
// wake up
int hs_uv_wake_up_timer(hs_loop_data* loop_data);
int hs_uv_wake_up_async(hs_loop_data* loop_data);

////////////////////////////////////////////////////////////////////////////////
// handle
uv_handle_t* hs_uv_handle_alloc(uv_loop_t* loop);
void hs_uv_handle_free(uv_handle_t* handle);
void hs_uv_handle_close(uv_handle_t* handle);

////////////////////////////////////////////////////////////////////////////////
// request
void hs_uv_cancel(uv_loop_t* loop, HsInt slot);


////////////////////////////////////////////////////////////////////////////////
// request
uv_check_t* hs_uv_check_alloc();
int hs_uv_check_init(uv_check_t* check, uv_handle_t* handle);
void hs_uv_check_close(uv_check_t* check);

////////////////////////////////////////////////////////////////////////////////
// stream
int hs_uv_listen(uv_stream_t* stream, int backlog);
void hs_uv_listen_resume(uv_stream_t* server);
int hs_uv_read_start(uv_stream_t* handle);
HsInt hs_uv_write(uv_stream_t* handle, char* buf, HsInt buf_size);
int hs_uv_accept_check_start(uv_check_t* check);

////////////////////////////////////////////////////////////////////////////////
// tcp and pipe
int hs_uv_tcp_open(uv_tcp_t* handle, int32_t sock);
HsInt hs_uv_tcp_connect(uv_tcp_t* handle, const struct sockaddr* addr);

int hs_uv_pipe_open(uv_pipe_t* handle, int32_t sock);
HsInt hs_uv_pipe_connect(uv_pipe_t* handle, const char* name);

#if defined(_WIN32)
#define UV_HANDLE_READING                       0x00000100
#define UV_HANDLE_BOUND                         0x00000200
#define UV_HANDLE_LISTENING                     0x00000800
#define UV_HANDLE_CONNECTION                    0x00001000
#define UV_HANDLE_READABLE                      0x00008000
#define UV_HANDLE_WRITABLE                      0x00010000
enum {
  UV__SIGNAL_ONE_SHOT = 0x80000,  /* On signal reception remove sighandler */
  UV__HANDLE_INTERNAL = 0x8000,
  UV__HANDLE_ACTIVE   = 0x4000,
  UV__HANDLE_REF      = 0x2000,
  UV__HANDLE_CLOSING  = 0 /* no-op on unix */
};
#define UV_HANDLE_TCP_SINGLE_ACCEPT             0x08000000
#define UV_HANDLE_TCP_ACCEPT_STATE_CHANGING     0x10000000
extern unsigned int uv_simultaneous_server_accepts;
extern void uv_tcp_queue_accept(uv_tcp_t* handle, uv_tcp_accept_t* req);
#else
void uv__io_start(uv_loop_t* loop, uv__io_t* w, unsigned int events);
#endif

////////////////////////////////////////////////////////////////////////////////
// udp

int hs_uv_udp_recv_start(uv_udp_t* handle);
HsInt hs_uv_udp_send_connected(uv_udp_t* handle, char* buf, HsInt buf_siz);
HsInt hs_uv_upd_send(uv_udp_t* handle, const struct sockaddr* addr, char* buf, HsInt buf_siz);
int hs_uv_udp_check_start(uv_check_t* check);

////////////////////////////////////////////////////////////////////////////////
// fs

// we define file open flag here for compatibility on libuv < v1.16
// see https://github.com/libuv/libuv/commit/4b666bd2d82a51f1c809b2703a91679789c1ec01#diff-a5e63f9b16ca783355e2d83941c3eafb

#if defined(_WIN32)

/* fs open() flags supported on this platform: */
#ifndef UV_FS_O_APPEND
#define UV_FS_O_APPEND       _O_APPEND
#endif
#ifndef UV_FS_O_CREAT
#define UV_FS_O_CREAT        _O_CREAT
#endif
#ifndef UV_FS_O_EXCL
#define UV_FS_O_EXCL         _O_EXCL
#endif
#ifndef UV_FS_O_RANDOM
#define UV_FS_O_RANDOM       _O_RANDOM
#endif
#ifndef UV_FS_O_RDONLY
#define UV_FS_O_RDONLY       _O_RDONLY
#endif
#ifndef UV_FS_O_RDWR
#define UV_FS_O_RDWR         _O_RDWR
#endif
#ifndef UV_FS_O_SEQUENTIAL
#define UV_FS_O_SEQUENTIAL   _O_SEQUENTIAL
#endif
#ifndef UV_FS_O_SHORT_LIVED
#define UV_FS_O_SHORT_LIVED  _O_SHORT_LIVED
#endif
#ifndef UV_FS_O_TEMPORARY
#define UV_FS_O_TEMPORARY    _O_TEMPORARY
#endif
#ifndef UV_FS_O_TRUNC
#define UV_FS_O_TRUNC        _O_TRUNC
#endif
#ifndef UV_FS_O_WRONLY
#define UV_FS_O_WRONLY       _O_WRONLY
#endif

/* fs open() flags supported on other platforms (or mapped on this platform): */
#ifndef UV_FS_O_DIRECT
#define UV_FS_O_DIRECT       0x2000000 /* FILE_FLAG_NO_BUFFERING */
#endif
#ifndef UV_FS_O_DIRECTORY
#define UV_FS_O_DIRECTORY    0
#endif
#ifndef UV_FS_O_DSYNC
#define UV_FS_O_DSYNC        0x4000000 /* FILE_FLAG_WRITE_THROUGH */
#endif
#ifndef UV_FS_O_EXLOCK
#define UV_FS_O_EXLOCK       0
#endif
#ifndef UV_FS_O_NOATIME
#define UV_FS_O_NOATIME      0
#endif
#ifndef UV_FS_O_NOCTTY
#define UV_FS_O_NOCTTY       0
#endif
#ifndef UV_FS_O_NOFOLLOW
#define UV_FS_O_NOFOLLOW     0
#endif
#ifndef UV_FS_O_NONBLOCK
#define UV_FS_O_NONBLOCK     0
#endif
#ifndef UV_FS_O_SYMLINK
#define UV_FS_O_SYMLINK      0
#endif
#ifndef UV_FS_O_SYNC
#define UV_FS_O_SYNC         0x8000000 /* FILE_FLAG_WRITE_THROUGH */
#endif

#else /* _WIN32 */

#ifndef UV_FS_O_APPEND
#if defined(O_APPEND)
# define UV_FS_O_APPEND       O_APPEND
#else
# define UV_FS_O_APPEND       0
#endif
#endif
#ifndef UV_FS_O_CREAT
#if defined(O_CREAT)
# define UV_FS_O_CREAT        O_CREAT
#else
# define UV_FS_O_CREAT        0
#endif
#endif
#ifndef UV_FS_O_DIRECT
#if defined(O_DIRECT)
# define UV_FS_O_DIRECT       O_DIRECT
#else
# define UV_FS_O_DIRECT       0
#endif
#endif
#ifndef UV_FS_O_DIRECTORY
#if defined(O_DIRECTORY)
# define UV_FS_O_DIRECTORY    O_DIRECTORY
#else
# define UV_FS_O_DIRECTORY    0
#endif
#endif
#ifndef UV_FS_O_DSYNC
#if defined(O_DSYNC)
# define UV_FS_O_DSYNC        O_DSYNC
#else
# define UV_FS_O_DSYNC        0
#endif
#endif
#ifndef UV_FS_O_EXCL
#if defined(O_EXCL)
# define UV_FS_O_EXCL         O_EXCL
#else
# define UV_FS_O_EXCL         0
#endif
#endif
#ifndef UV_FS_O_EXLOCK
#if defined(O_EXLOCK)
# define UV_FS_O_EXLOCK       O_EXLOCK
#else
# define UV_FS_O_EXLOCK       0
#endif
#endif
#ifndef UV_FS_O_NOATIME
#if defined(O_NOATIME)
# define UV_FS_O_NOATIME      O_NOATIME
#else
# define UV_FS_O_NOATIME      0
#endif
#endif
#ifndef UV_FS_O_NOCTTY
#if defined(O_NOCTTY)
# define UV_FS_O_NOCTTY       O_NOCTTY
#else
# define UV_FS_O_NOCTTY       0
#endif
#endif
#ifndef UV_FS_O_NOFOLLOW
#if defined(O_NOFOLLOW)
# define UV_FS_O_NOFOLLOW     O_NOFOLLOW
#else
# define UV_FS_O_NOFOLLOW     0
#endif
#endif
#ifndef UV_FS_O_NONBLOCK
#if defined(O_NONBLOCK)
# define UV_FS_O_NONBLOCK     O_NONBLOCK
#else
# define UV_FS_O_NONBLOCK     0
#endif
#endif
#ifndef UV_FS_O_RDONLY
#if defined(O_RDONLY)
# define UV_FS_O_RDONLY       O_RDONLY
#else
# define UV_FS_O_RDONLY       0
#endif
#endif
#ifndef UV_FS_O_RDWR
#if defined(O_RDWR)
# define UV_FS_O_RDWR         O_RDWR
#else
# define UV_FS_O_RDWR         0
#endif
#endif
#ifndef UV_FS_O_SYMLINK
#if defined(O_SYMLINK)
# define UV_FS_O_SYMLINK      O_SYMLINK
#else
# define UV_FS_O_SYMLINK      0
#endif
#endif
#ifndef UV_FS_O_SYNC
#if defined(O_SYNC)
# define UV_FS_O_SYNC         O_SYNC
#else
# define UV_FS_O_SYNC         0
#endif
#endif
#ifndef UV_FS_O_TRUNC
#if defined(O_TRUNC)
# define UV_FS_O_TRUNC        O_TRUNC
#else
# define UV_FS_O_TRUNC        0
#endif
#endif
#ifndef UV_FS_O_WRONLY
#if defined(O_WRONLY)
# define UV_FS_O_WRONLY       O_WRONLY
#else
# define UV_FS_O_WRONLY       0
#endif
#endif

/* fs open() flags supported on other platforms: */
#ifndef UV_FS_O_RANDOM
#define UV_FS_O_RANDOM        0
#endif
#ifndef UV_FS_O_SHORT_LIVED
#define UV_FS_O_SHORT_LIVED   0
#endif
#ifndef UV_FS_O_SEQUENTIAL
#define UV_FS_O_SEQUENTIAL    0
#endif
#ifndef UV_FS_O_TEMPORARY
#define UV_FS_O_TEMPORARY     0
#endif

#endif /* _WIN32 */

typedef uv__dirent_t hs_uv__dirent_t;

#if defined(_WIN32)

#define HAVE_DIRENT_TYPES
#define UV__DT_DIR     UV_DIRENT_DIR
#define UV__DT_FILE    UV_DIRENT_FILE
#define UV__DT_LINK    UV_DIRENT_LINK
#define UV__DT_FIFO    UV_DIRENT_FIFO
#define UV__DT_SOCKET  UV_DIRENT_SOCKET
#define UV__DT_CHAR    UV_DIRENT_CHAR
#define UV__DT_BLOCK   UV_DIRENT_BLOCK

#else /* _WIN32 */

#if defined(DT_UNKNOWN)
# define HAVE_DIRENT_TYPES
# if defined(DT_REG)
#  define UV__DT_FILE DT_REG
# else
#  define UV__DT_FILE -1
# endif
# if defined(DT_DIR)
#  define UV__DT_DIR DT_DIR
# else
#  define UV__DT_DIR -2
# endif
# if defined(DT_LNK)
#  define UV__DT_LINK DT_LNK
# else
#  define UV__DT_LINK -3
# endif
# if defined(DT_FIFO)
#  define UV__DT_FIFO DT_FIFO
# else
#  define UV__DT_FIFO -4
# endif
# if defined(DT_SOCK)
#  define UV__DT_SOCKET DT_SOCK
# else
#  define UV__DT_SOCKET -5
# endif
# if defined(DT_CHR)
#  define UV__DT_CHAR DT_CHR
# else
#  define UV__DT_CHAR -6
# endif
# if defined(DT_BLK)
#  define UV__DT_BLOCK DT_BLK
# else
#  define UV__DT_BLOCK -7
# endif
#endif /* DT_UNKNOWN */

#endif /* _WIN32 */


void uv__free(void* p);
#if defined(_WIN32)
# define uv__fs_scandir_free uv__free
#else /* _WIN32 */
# define uv__fs_scandir_free free
// uv__once_init will run WSAStartup which is required to run socket code on WIN32
void uv__once_init(void);
#endif /* _WIN32 */

void hs_uv_fs_scandir_cleanup(uv_dirent_t** dents, HsInt n);
void hs_uv_fs_scandir_extra_cleanup(uv_dirent_t*** dents_p, HsInt n);
void hs_uv_fs_readlink_extra_cleanup(char** path);
void hs_uv_fs_readlink_cleanup(char* path);

////////////////////////////////////////////////////////////////////////////////
// fs, none thread pool version
int64_t hs_seek(int file, int64_t off, int origin);

int32_t hs_uv_fs_open(const char* path, int flags, int mode);
HsInt hs_uv_fs_close(int32_t file);
HsInt hs_uv_fs_read(int32_t file, char* buffer, HsInt buffer_size, int64_t offset);
HsInt hs_uv_fs_write(int32_t file, char* buffer, HsInt buffer_size, int64_t offset);
HsInt hs_uv_fs_unlink(const char* path);
HsInt hs_uv_fs_mkdir(const char* path, int mode);
HsInt hs_uv_fs_mkdtemp(const char* tpl, HsInt tpl_size, char* temp_path);
HsInt hs_uv_fs_rmdir(const char* path);
HsInt hs_uv_fs_scandir(const char* path, uv_dirent_t*** dents);
HsInt hs_uv_fs_stat(const char* path, uv_stat_t* stat);
HsInt hs_uv_fs_fstat(int32_t file, uv_stat_t* stat);
HsInt hs_uv_fs_lstat(const char* path, uv_stat_t* stat);
HsInt hs_uv_fs_rename(const char* path, const char* path2);
HsInt hs_uv_fs_fsync(int32_t file);
HsInt hs_uv_fs_fdatasync(int32_t file);
HsInt hs_uv_fs_ftruncate(int32_t file, int64_t off);
HsInt hs_uv_fs_copyfile(const char* path, const char* path2, int flag);
HsInt hs_uv_fs_access(const char* path, int mode);
HsInt hs_uv_fs_chmod(const char* path, int mode);
HsInt hs_uv_fs_fchmod(int32_t file, int mode);
HsInt hs_uv_fs_utime(const char* path, double atime, double mtime);
HsInt hs_uv_fs_futime(int32_t file, double atime, double mtime);
HsInt hs_uv_fs_lutime(const char* path, double atime, double mtime);
HsInt hs_uv_fs_link(const char* path, const char* path2);
HsInt hs_uv_fs_symlink(const char* path, const char* path2, int flag);
HsInt hs_uv_fs_readlink(const char* path, char** result_path);
HsInt hs_uv_fs_realpath(const char* path, char** result_path);
HsInt hs_uv_fs_chown(const char* path, uv_uid_t uid, uv_gid_t gid);
HsInt hs_uv_fs_fchown(int32_t file, uv_uid_t uid, uv_gid_t gid);
HsInt hs_uv_fs_lchown(const char* path, uv_uid_t uid, uv_gid_t gid);

////////////////////////////////////////////////////////////////////////////////
// fs, thread pool version
HsInt hs_uv_fs_open_threaded(const char* path, int flags, int mode, uv_loop_t* loop);
HsInt hs_uv_fs_close_threaded(int32_t file, uv_loop_t* loop);
HsInt hs_uv_fs_read_threaded(int32_t file, char* buffer, HsInt buffer_size, int64_t offset, uv_loop_t* loop);
HsInt hs_uv_fs_write_threaded(int32_t file, char* buffer, HsInt buffer_size, int64_t offset, uv_loop_t* loop);
HsInt hs_uv_fs_unlink_threaded(const char* path, uv_loop_t* loop);
HsInt hs_uv_fs_mkdir_threaded(const char* path, int mode, uv_loop_t* loop);
HsInt hs_uv_fs_mkdtemp_threaded(const char* tpl, HsInt tpl_size, char* temp_path, uv_loop_t* loop);
HsInt hs_uv_fs_rmdir_threaded(const char* path, uv_loop_t* loop);
HsInt hs_uv_fs_scandir_threaded(const char* path, uv_dirent_t*** dents, uv_loop_t* loop);
HsInt hs_uv_fs_stat_threaded(const char* path, uv_stat_t* stat, uv_loop_t* loop);
HsInt hs_uv_fs_fstat_threaded(int32_t file, uv_stat_t* stat, uv_loop_t* loop);
HsInt hs_uv_fs_lstat_threaded(const char* path, uv_stat_t* stat, uv_loop_t* loop);
HsInt hs_uv_fs_rename_threaded(const char* path, const char* path2, uv_loop_t* loop);
HsInt hs_uv_fs_fsync_threaded(int32_t file, uv_loop_t* loop);
HsInt hs_uv_fs_fdatasync_threaded(int32_t file, uv_loop_t* loop);
HsInt hs_uv_fs_ftruncate_threaded(int32_t file, int64_t off, uv_loop_t* loop);
HsInt hs_uv_fs_copyfile_threaded(const char* path, const char* path2, int flag, uv_loop_t* loop);
HsInt hs_uv_fs_access_threaded(const char* path, int mode, uv_loop_t* loop);
HsInt hs_uv_fs_chmod_threaded(const char* path, int mode, uv_loop_t* loop);
HsInt hs_uv_fs_fchmod_threaded(int32_t file, int mode, uv_loop_t* loop);
HsInt hs_uv_fs_utime_threaded(const char* path, double atime, double mtime, uv_loop_t* loop);
HsInt hs_uv_fs_futime_threaded(int32_t file, double atime, double mtime, uv_loop_t* loop);
HsInt hs_uv_fs_lutime_threaded(const char* path, double atime, double mtime, uv_loop_t* loop);
HsInt hs_uv_fs_link_threaded(const char* path, const char* path2, uv_loop_t* loop);
HsInt hs_uv_fs_symlink_threaded(const char* path, const char* path2, int flag, uv_loop_t* loop);
HsInt hs_uv_fs_readlink_threaded(const char* path, char** result_path, uv_loop_t* loop);
HsInt hs_uv_fs_realpath_threaded(const char* path, char** result_path, uv_loop_t* loop);
HsInt hs_uv_fs_chown_threaded(const char* path, uv_uid_t uid, uv_gid_t gid, uv_loop_t* loop);
HsInt hs_uv_fs_fchown_threaded(int32_t file, uv_uid_t uid, uv_gid_t gid, uv_loop_t* loop);
HsInt hs_uv_fs_lchown_threaded(const char* path, uv_uid_t uid, uv_gid_t gid, uv_loop_t* loop);

////////////////////////////////////////////////////////////////////////////////
// process

HsInt hs_uv_spawn(uv_loop_t* loop
                 , uv_process_options_t* options
                 , const char* file
#if __GLASGOW_HASKELL__ < 810
                 , const StgMutArrPtrs* all_args_arr
                 , const HsInt args_len
                 , const StgMutArrPtrs* all_env_arr
                 , const HsInt env_len
#else
                 , const StgArrBytes** all_args
                 , const HsInt args_len
                 , const StgArrBytes** all_env
                 , const HsInt env_len
#endif
                 , const char* cwd
                 , uv_stdio_container_t* container);

////////////////////////////////////////////////////////////////////////////////
// dns
int uv__getaddrinfo_translate_error(int sys_err);
HsInt hs_getaddrinfo(const char *node, const char *service,
                     const struct addrinfo *hints,
                     struct addrinfo **res);
HsInt hs_getnameinfo(const struct sockaddr *addr, socklen_t addrlen,
                     char *host, socklen_t hostlen,
                     char *serv, socklen_t servlen, int flags);

////////////////////////////////////////////////////////////////////////////////
// random
HsInt hs_uv_random(char* buf, size_t len, unsigned int flags);
HsInt hs_uv_random_threaded(char* buf, size_t len, unsigned int flags, uv_loop_t* loop);

////////////////////////////////////////////////////////////////////////////////
// fs event
int hs_uv_fs_event_start(uv_fs_event_t* handle, const char* path, unsigned int flags);
int hs_uv_fs_event_check_start(uv_check_t* check);
