/* Copyright (C) 2013 Codership Oy <info@codership.com>

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License along
   with this program; if not, write to the Free Software Foundation, Inc.,
   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA. */

#ifndef WSREP_UTILS_H
#define WSREP_UTILS_H

#include "wsrep_priv.h"
#include "wsrep_mysqld.h"

unsigned int wsrep_check_ip (const char* addr);
size_t wsrep_guess_ip (char* buf, size_t buf_len);
size_t wsrep_guess_address(char* buf, size_t buf_len);

namespace wsp {

class Config_state
{
public:
  Config_state() : view_(), status_(WSREP_MEMBER_UNDEFINED)
  {}

  void set(wsrep_member_status_t status, const wsrep_view_info_t* view)
  {
    wsrep_notify_status(status, view);

    lock();

    status_= status;
    view_= *view;
    member_info_.clear();

    wsrep_member_info_t memb;
    for(int i= 0; i < view->memb_num; i ++)
    {
      memb= view->members[i];
      member_info_.append_val(memb);
    }

    unlock();
  }

  void set(wsrep_member_status_t status)
  {
    wsrep_notify_status(status, 0);
    lock();
    status_= status;
    unlock();
  }

  wsrep_view_info_t get_view_info() const
  {
    return view_;
  }

  wsrep_member_status_t get_status() const
  {
    return status_;
  }

  Dynamic_array<wsrep_member_info_t> * get_member_info()
  {
    return &member_info_;
  }

  int lock()
  {
    return mysql_mutex_lock(&LOCK_wsrep_config_state);
  }

  int unlock()
  {
    return mysql_mutex_unlock(&LOCK_wsrep_config_state);
  }

private:
  wsrep_view_info_t                  view_;
  wsrep_member_status_t              status_;
  Dynamic_array<wsrep_member_info_t> member_info_;
};

} /* namespace wsp */

extern wsp::Config_state wsrep_config_state;

namespace wsp {
/* A small class to run external programs. */
class process
{
private:
    const char* const str_;
    FILE*       io_;
    int         err_;
    pid_t       pid_;

public:
/*! @arg type is a pointer to a null-terminated string which  must  contain
         either  the  letter  'r'  for  reading  or the letter 'w' for writing.
 */
    process  (const char* cmd, const char* type);
    ~process ();

    FILE* pipe () { return io_;  }
    int   error() { return err_; }
    int   wait ();
    const char* cmd() { return str_; }
};

class thd
{
  class thd_init
  {
  public:
    thd_init()  { my_thread_init(); }
    ~thd_init() { my_thread_end();  }
  }
  init;

  thd (const thd&);
  thd& operator= (const thd&);

public:

  thd(my_bool wsrep_on);
  ~thd();
  THD* const ptr;
};

class string
{
public:
    string() : string_(0) {}
    void set(char* str) { if (string_) free (string_); string_ = str; }
    ~string() { set (0); }
private:
    char* string_;
};

#ifdef REMOVED
class lock
{
  pthread_mutex_t* const mtx_;

public:

  lock (pthread_mutex_t* mtx) : mtx_(mtx)
  {
    int err = pthread_mutex_lock (mtx_);

    if (err)
    {
      WSREP_ERROR("Mutex lock failed: %s", strerror(err));
      abort();
    }
  }

  virtual ~lock ()
  {
    int err = pthread_mutex_unlock (mtx_);

    if (err)
    {
      WSREP_ERROR("Mutex unlock failed: %s", strerror(err));
      abort();
    }
  }

  inline void wait (pthread_cond_t* cond)
  {
    pthread_cond_wait (cond, mtx_);
  }

private:

  lock (const lock&);
  lock& operator=(const lock&);

};

class monitor
{
  int             mutable refcnt;
  pthread_mutex_t mutable mtx;
  pthread_cond_t  mutable cond;

public:

  monitor() : refcnt(0)
  {
    pthread_mutex_init (&mtx, NULL);
    pthread_cond_init  (&cond, NULL);
  }

  ~monitor()
  {
    pthread_mutex_destroy (&mtx);
    pthread_cond_destroy  (&cond);
  }

  void enter() const
  {
    lock l(&mtx);

    while (refcnt)
    {
      l.wait(&cond);
    }
    refcnt++;
  }

  void leave() const
  {
    lock l(&mtx);

    refcnt--;
    if (refcnt == 0)
    {
      pthread_cond_signal (&cond);
    }
  }

private:

  monitor (const monitor&);
  monitor& operator= (const monitor&);
};

class critical
{
  const monitor& mon;

public:

  critical(const monitor& m) : mon(m) { mon.enter(); }

  ~critical() { mon.leave(); }

private:

  critical (const critical&);
  critical& operator= (const critical&);
};
#endif

} // namespace wsrep

#endif /* WSREP_UTILS_H */
