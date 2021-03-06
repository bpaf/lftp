/*
 * lftp and utils
 *
 * Copyright (c) 1998-2002 by Alexander V. Lukyanov (lav@yars.free.net)
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

/* $Id: SleepJob.h,v 1.18 2011/04/29 04:58:28 lav Exp $ */

#ifndef SLEEPJOB_H
#define SLEEPJOB_H

#include "Job.h"
#include "ResMgr.h"
#include "LocalDir.h"
#include "CmdExec.h"

class SleepJob : public SessionJob, public Timer
{
   xstring cmd;
   int exit_code;
   bool done;
   Ref<LocalDirectory> saved_cwd;
   JobRef<CmdExec> exec;
   bool repeat;
   bool weak;  // terminate on `exit bg'.
   int repeat_count;
   int max_repeat_count;
   int continue_code;
   int break_code;

public:
   int Do();
   int Done() { return done; }
   int ExitCode() { return exit_code; }

   SleepJob(const TimeInterval &when,FileAccess *s=0,LocalDirectory *cwd=0,char *what=0);
   ~SleepJob();

   const char *Status();
   xstring& FormatStatus(xstring&,int v,const char *);
   void ShowRunStatus(const SMTaskRef<StatusLine>& s);

   void Repeat(int m) { repeat=true; max_repeat_count=m; Stop(); }
   void SetWeak(bool w) { weak=w; }
   void ContinueCode(int c) { continue_code=c; }
   void BreakCode(int c) { break_code=c; }

   void lftpMovesToBackground();
};

#endif//SLEEPJOB_H
