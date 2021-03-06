/*
 * lftp - file transfer program
 *
 * Copyright (c) 1999-2010 by Alexander V. Lukyanov (lav@yars.free.net)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

/* $Id: NetAccess.cc,v 1.112 2011/06/16 11:08:42 lav Exp $ */

#include <config.h>

#include <errno.h>
#include <assert.h>
#include <math.h>
#include <sys/types.h>

#include "NetAccess.h"
#include "log.h"
#include "url.h"
#include "LsCache.h"
#include "misc.h"
#include "Speedometer.h"

#define super FileAccess

void NetAccess::Init()
{
   resolver=0;
   idle_timer.SetResource("net:idle",0);
   timeout_timer.SetResource("net:timeout",0);
   max_retries=0;
   max_persist_retries=0;
   persist_retries=0;
   socket_buffer=0;
   socket_maxseg=0;

   peer_curr=0;

   reconnect_interval=30;  // retry with 30 second interval
   reconnect_interval_multiplier=1.2;
   reconnect_interval_max=300;

   rate_limit=0;

   connection_limit=0;	// no limit.
   connection_takeover=false;

   Reconfig(0);
   reconnect_interval_current=reconnect_interval;
}

NetAccess::NetAccess()
{
   Init();
}
NetAccess::NetAccess(const NetAccess *o) : super(o)
{
   Init();
   if(o->peer)
   {
      peer.set(o->peer);
      peer_curr=o->peer_curr;
      if(peer_curr>=peer.count())
	 peer_curr=0;
   }
   home_auto.set(o->home_auto);
}
NetAccess::~NetAccess()
{
   ClearPeer();
}

void NetAccess::Reconfig(const char *name)
{
   super::Reconfig(name);

   const char *c=hostname;

   reconnect_interval = ResMgr::Query("net:reconnect-interval-base",c);
   reconnect_interval_multiplier = ResMgr::Query("net:reconnect-interval-multiplier",c);
   if(reconnect_interval_multiplier<1)
      reconnect_interval_multiplier=1;
   reconnect_interval_max = ResMgr::Query("net:reconnect-interval-max",c);
   if(reconnect_interval_max<reconnect_interval)
      reconnect_interval_max=reconnect_interval;
   max_retries = ResMgr::Query("net:max-retries",c);
   max_persist_retries = ResMgr::Query("net:persist-retries",c);
   socket_buffer = ResMgr::Query("net:socket-buffer",c);
   socket_maxseg = ResMgr::Query("net:socket-maxseg",c);
   connection_limit = ResMgr::Query("net:connection-limit",c);
   connection_takeover = ResMgr::QueryBool("net:connection-takeover",c);

   if(rate_limit)
      rate_limit->Reconfig(name,c);
}

int NetAccess::CheckHangup(const struct pollfd *pfd,int num)
{
   for(int i=0; i<num; i++)
   {
#ifdef SO_ERROR
      int   s_errno=0;
      socklen_t len;

      errno=0;

// Where does the error number go - to errno or to the pointer?
// It seems that to errno, but if the pointer is NULL it dumps core.
// (solaris 2.5)
// It seems to be different on glibc 2.0 - check both errno and s_errno

      len=sizeof(s_errno);
      getsockopt(pfd[i].fd,SOL_SOCKET,SO_ERROR,(char*)&s_errno,&len);
      if(errno==ENOTSOCK)
	 return 0;
      if(errno!=0 || s_errno!=0)
      {
	 LogError(0,_("Socket error (%s) - reconnecting"),
				    strerror(errno?errno:s_errno));
	 return 1;
      }
#endif /* SO_ERROR */
      if(pfd[i].revents&POLLERR)
      {
	 LogError(0,"POLLERR on fd %d",pfd[i].fd);
	 return 1;
      }
   } /* end for */
   return 0;
}
int NetAccess::Poll(int fd,int ev)
{
   struct pollfd pfd;
   pfd.fd=fd;
   pfd.events=ev;
   pfd.revents=0;
   int res=poll(&pfd,1,0);
   if(res<1)
      return 0;
   if(CheckHangup(&pfd,1))
      return -1;
   if(pfd.revents)
      timeout_timer.Reset();
   return pfd.revents;
}

void NetAccess::SayConnectingTo()
{
   assert(peer_curr<peer.count());
   const char *h=(proxy?proxy:hostname);
   LogNote(1,_("Connecting to %s%s (%s) port %u"),proxy?"proxy ":"",
      h,SocketNumericAddress(&peer[peer_curr]),SocketPort(&peer[peer_curr]));
}

void NetAccess::SetProxy(const char *px)
{
   bool was_proxied=(proxy!=0);

   proxy.set(0);
   proxy_port.set(0);
   proxy_user.set(0);
   proxy_pass.set(0);
   proxy_proto.set(0);

   if(!px)
      px="";

   ParsedURL url(px);
   if(!url.host || url.host[0]==0)
   {
      if(was_proxied)
	 ClearPeer();
      return;
   }

   proxy.set(url.host);
   proxy_port.set(url.port);
   proxy_user.set(url.user);
   proxy_pass.set(url.pass);
   proxy_proto.set(url.proto);
   ClearPeer();
}

bool NetAccess::NoProxy(const char *hostname)
{
   // match hostname against no-proxy var.
   if(!hostname)
      return false;
   const char *no_proxy_c=ResMgr::Query("net:no-proxy",0);
   if(!no_proxy_c)
      return false;
   char *no_proxy=alloca_strdup(no_proxy_c);
   int h_len=strlen(hostname);
   for(char *p=strtok(no_proxy," ,"); p; p=strtok(0," ,"))
   {
      int p_len=strlen(p);
      if(p_len>h_len || p_len==0)
	 continue;
      if(!strcasecmp(hostname+h_len-p_len,p))
	 return true;
   }
   return false;
}

void NetAccess::HandleTimeout()
{
   LogError(0,_("Timeout - reconnecting"));
   Disconnect();
   timeout_timer.Reset();
}

bool NetAccess::CheckTimeout()
{
   if(timeout_timer.Stopped())
   {
      HandleTimeout();
      return(true);
   }
   return(false);
}

void NetAccess::ClearPeer()
{
   peer.unset();
   peer_curr=0;
}

void NetAccess::NextPeer()
{
   peer_curr++;
   if(peer_curr>=peer.count())
      peer_curr=0;
   else
      try_time=0;	// try next address immediately
}

void NetAccess::ResetLocationData()
{
   Disconnect();
   ClearPeer();
   super::ResetLocationData();
   timeout_timer.SetResource("net:timeout",hostname);
   idle_timer.SetResource("net:idle",hostname);
}

void NetAccess::Open(const char *fn,int mode,off_t offs)
{
   timeout_timer.Reset();
   super::Open(fn,mode,offs);
}

int NetAccess::Resolve(const char *defp,const char *ser,const char *pr)
{
   int m=STALL;

   if(!resolver)
   {
      peer.unset();
      if(proxy)
	 resolver=new Resolver(proxy,proxy_port,defp);
      else
	 resolver=new Resolver(hostname,portname,defp,ser,pr);
      if(!resolver)
	 return MOVED;
      resolver->Roll();
      m=MOVED;
   }

   if(!resolver->Done())
      return m;

   if(resolver->Error())
   {
      SetError(LOOKUP_ERROR,resolver->ErrorMsg());
      return(MOVED);
   }

   peer.set(resolver->Result());
   if(peer_curr>=peer.count())
      peer_curr=0;

   resolver=0;
   return MOVED;
}

bool NetAccess::ReconnectAllowed()
{
   if(max_retries>0 && retries>=max_retries)
      return true; // it will fault later - no need to wait.
   if(connection_limit>0 && connection_limit<=CountConnections())
      return false;
   if(try_time==0)
      return true;
   if(time_t(now) >= try_time+long(reconnect_interval_current))
      return true;
   TimeoutS(long(reconnect_interval_current)-(time_t(now)-try_time));
   return false;
}

const char *NetAccess::DelayingMessage()
{
   if(connection_limit>0 && connection_limit<=CountConnections())
      return _("Connection limit reached");
   long remains=long(reconnect_interval_current)-(time_t(now)-try_time);
   if(remains<=0)
      return "";
   current->TimeoutS(1);
   return xstring::format("%s: %ld",_("Delaying before reconnect"),remains);
}

bool NetAccess::NextTry()
{
   if(!CheckRetries())
      return false;
   if(retries==0)
      reconnect_interval_current=reconnect_interval;
   else if(reconnect_interval_multiplier>1)
   {
      reconnect_interval_current*=reconnect_interval_multiplier;
      if(reconnect_interval_current>reconnect_interval_max)
	 reconnect_interval_current=reconnect_interval_max;
   }
   retries++;
   return CheckRetries();
}
bool NetAccess::CheckRetries()
{
   if(max_retries>0 && retries>max_retries)
   {
      Fatal(_("max-retries exceeded"));
      return false;
   }
   try_time=now;
   return true;
}
void NetAccess::TrySuccess()
{
   retries=0;
   persist_retries=0;
   reconnect_interval_current=reconnect_interval;
}

void NetAccess::Close()
{
   if(mode!=CLOSED)
      idle_timer.Reset();

   TrySuccess();
   resolver=0;
   super::Close();
}

int NetAccess::CountConnections()
{
   int count=0;
   for(FileAccess *o=FirstSameSite(); o!=0; o=NextSameSite(o))
   {
      if(o->IsConnected())
	 count++;
   }
   return count;
}

void NetAccess::PropagateHomeAuto()
{
   if(!home_auto)
      return;
   for(FA *fo=FirstSameSite(); fo!=0; fo=NextSameSite(fo))
   {
      NetAccess *o=(NetAccess*)fo; // we are sure it is NetAccess.
      if(!o->home_auto)
      {
	 o->home_auto.set(home_auto);
	 if(!o->home)
	    o->set_home(home_auto);
      }
   }
}
const char *NetAccess::FindHomeAuto()
{
   for(FA *fo=FirstSameSite(); fo!=0; fo=NextSameSite(fo))
   {
      NetAccess *o=(NetAccess*)fo; // we are sure it is NetAccess.
      if(o->home_auto)
	 return o->home_auto;
   }
   return 0;
}


// GenericParseListInfo implementation
int GenericParseListInfo::Do()
{
#define need_size (need&FileInfo::SIZE)
#define need_time (need&FileInfo::DATE)

   FileInfo *file;
   int res;
   int m=STALL;
   int old_mode=mode;
   Ref<FileSet> set;

do_again:
   if(done)
      return m;

   if(!ubuf && !get_info)
   {
      const char *cache_buffer=0;
      int cache_buffer_size=0;
      const FileSet *cache_fset=0;
      int err;
      if(use_cache && FileAccess::cache->Find(session,"",mode,&err,
				    &cache_buffer,&cache_buffer_size,&cache_fset))
      {
	 if(err)
	 {
	    if(mode==FA::MP_LIST)
	    {
	       mode=FA::LONG_LIST;
	       goto do_again;
	    }
	    SetErrorCached(cache_buffer);
	    return MOVED;
	 }
	 if(cache_fset) {
	    Log::global->Write(11,"ListInfo: using cached file set\n");
	    set=new FileSet(cache_fset);
	    old_mode=mode;
	    goto got_fileset;
	 }
	 ubuf=new IOBuffer(IOBuffer::GET);
	 ubuf->Put(cache_buffer,cache_buffer_size);
	 ubuf->PutEOF();
      }
      else
      {
	 session->Open("",mode);
	 session->UseCache(use_cache);
	 ubuf=new IOBufferFileAccess(session);
	 ubuf->SetSpeedometer(new Speedometer());
	 if(FileAccess::cache->IsEnabled(session->GetHostName()))
	    ubuf->Save(FileAccess::cache->SizeLimit());
	 session->Roll();
	 ubuf->Roll();
      }
      m=MOVED;
   }
   if(ubuf)
   {
      if(ubuf->Error())
      {
	 FileAccess::cache->Add(session,"",mode,session->GetErrorCode(),ubuf);
	 if(mode==FA::MP_LIST)
	 {
	    mode=FA::LONG_LIST;
	    ubuf=0;
	    m=MOVED;
	    goto do_again;
	 }
	 SetError(ubuf->ErrorText());
	 ubuf=0;
	 return MOVED;
      }

      if(!ubuf->Eof())
	 return m;

      // now we have all the index in ubuf; parse it.
      const char *b;
      int len;
      ubuf->Get(&b,&len);
      old_mode=mode;
      set=Parse(b,len);

      // cache the list and the set.
      FileAccess::cache->Add(session,"",old_mode,FA::OK,ubuf,set);

got_fileset:
      if(set)
      {
	 bool need_resort=false;
	 set->rewind();
	 for(file=set->curr(); file!=0; file=set->next())
	 {
	    // tilde is special.
	    if(file->name[0]=='~')
	    {
	       file->name.set_substr(0,0,"./");
	       need_resort=true;
	    }
	 }
	 if(need_resort && !result)
	    result=new FileSet; // Merge will sort the names
	 if(result)
	 {
	    result->Merge(set);
	    set=0; // free it now.
	 }
	 else
	    result=set.borrow();
      }

      ubuf=0;
      m=MOVED;

      // try another mode? Parse() can set mode to indicate it wants to try it.
      if(mode!=old_mode)
	 return m;

      if(!result)
	 result=new FileSet;

      result->ExcludeDots();
      result->Exclude(exclude_prefix,exclude);
      result->rewind();
      for(file=result->curr(); file!=0; file=result->next())
      {
	 FA::fileinfo add;
	 add.size=NO_SIZE;
	 add.time=NO_DATE;
	 add.get_size = need_size && !(file->defined & file->SIZE);
	 add.get_time = need_time && (!(file->defined & file->DATE)
				 || (file->date.ts_prec>0 && can_get_prec_time));
	 add.is_dir=false;
	 add.file=0;

	 if(file->defined & file->TYPE)
	 {
	    if(file->filetype==file->SYMLINK && follow_symlinks)
	    {
	       //file->filetype=file->NORMAL;
	       file->defined &= ~(file->SIZE|file->SYMLINK_DEF|file->MODE|file->DATE|file->TYPE);
	       add.get_size=true;
	       add.get_time=true;
	    }

	    if(file->filetype==file->SYMLINK)
	    {
	       // don't need these for symlinks
	       add.get_size=false;
	       add.get_time=false;
	    }
	    else if(file->filetype==file->DIRECTORY)
	    {
	       if(!get_time_for_dirs)
		  continue;
	       // don't need size for directories
	       add.get_size=false;
	       add.is_dir=true;
	    }
	 }

	 if(add.get_size || add.get_time)
	 {
	    add.file=file->name;
	    if(!add.get_size)
	       add.size=NO_SIZE;
	    if(!add.get_time)
	       add.time=NO_DATE;
	    get_info.append(add);
	 }
      }
      if(get_info.count()==0)
      {
	 done=true;
	 return m;
      }
      session->GetInfoArray(get_info.get_non_const(),get_info.count());
   }
   if(get_info)
   {
      res=session->Done();
      if(res==FA::DO_AGAIN)
	 return m;
      if(res==FA::IN_PROGRESS)
	 return m;
      session->Close();
      for(int i=0; i<get_info.count(); i++)
      {
	 if(get_info[i].time!=NO_DATE)
	    result->SetDate(get_info[i].file,get_info[i].time,0);
	 if(get_info[i].size!=NO_SIZE)
	    result->SetSize(get_info[i].file,get_info[i].size);
      }
      get_info.unset();
      done=true;
      m=MOVED;
   }
   return m;
}

GenericParseListInfo::GenericParseListInfo(FileAccess *s,const char *p)
   : ListInfo(s,p)
{
   get_time_for_dirs=true;
   can_get_prec_time=true;
   mode=FA::MP_LIST;
}

const char *GenericParseListInfo::Status()
{
   if(ubuf && !ubuf->Eof() && session->IsOpen())
      return xstring::format("%s (%lld) %s[%s]",_("Getting directory contents"),
		     (long long)session->GetPos(),
		     ubuf->GetRateStrS(),session->CurrentStatus());
   if(get_info)
      return xstring::format("%s (%d%%) [%s]",_("Getting files information"),
		     session->InfoArrayPercentDone(),
		     session->CurrentStatus());
   return "";
}
