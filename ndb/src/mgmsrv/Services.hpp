/* Copyright (C) 2003 MySQL AB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA */

#ifndef MGMAPI_SERVICE_HPP
#define MGMAPI_SERVICE_HPP

#include <SocketServer.hpp>
#include <NdbSleep.h>
#include <Parser.hpp>
#include <OutputStream.hpp>
#include <InputStream.hpp>

#include "MgmtSrvr.hpp"

/** Undefine this to remove backwards compatibility for "GET CONFIG". */
#define MGM_GET_CONFIG_BACKWARDS_COMPAT

class MgmApiSession : public SocketServer::Session {
private:
  typedef Parser<MgmApiSession> Parser_t;

  class MgmtSrvr & m_mgmsrv;
  InputStream *m_input;
  OutputStream *m_output;
  Parser_t *m_parser;

  void getConfig_common(Parser_t::Context &ctx,
			const class Properties &args,
			bool compat = false);

public:
  MgmApiSession(class MgmtSrvr & mgm, NDB_SOCKET_TYPE sock);  
  void runSession();

  void getStatPort(Parser_t::Context &ctx, const class Properties &args);
  void getConfig(Parser_t::Context &ctx, const class Properties &args);
#ifdef MGM_GET_CONFIG_BACKWARDS_COMPAT
  void getConfig_old(Parser_t::Context &ctx);
#endif /* MGM_GET_CONFIG_BACKWARDS_COMPAT */

  void getVersion(Parser_t::Context &ctx, const class Properties &args);
  void getStatus(Parser_t::Context &ctx, const class Properties &args);
  void getInfoClusterLog(Parser_t::Context &ctx, const class Properties &args);
  void restart(Parser_t::Context &ctx, const class Properties &args);
  void restartAll(Parser_t::Context &ctx, const class Properties &args);
  void insertError(Parser_t::Context &ctx, const class Properties &args);
  void setTrace(Parser_t::Context &ctx, const class Properties &args);
  void logSignals(Parser_t::Context &ctx, const class Properties &args);
  void startSignalLog(Parser_t::Context &ctx, const class Properties &args);
  void stopSignalLog(Parser_t::Context &ctx, const class Properties &args);
  void dumpState(Parser_t::Context &ctx, const class Properties &args);
  void startBackup(Parser_t::Context &ctx, const class Properties &args);
  void abortBackup(Parser_t::Context &ctx, const class Properties &args);
  void enterSingleUser(Parser_t::Context &ctx, const class Properties &args);
  void exitSingleUser(Parser_t::Context &ctx, const class Properties &args);
  void stop(Parser_t::Context &ctx, const class Properties &args);
  void stopAll(Parser_t::Context &ctx, const class Properties &args);
  void start(Parser_t::Context &ctx, const class Properties &args);
  void startAll(Parser_t::Context &ctx, const class Properties &args);
  void bye(Parser_t::Context &ctx, const class Properties &args);
  void setLogLevel(Parser_t::Context &ctx, const class Properties &args);
  void setClusterLogLevel(Parser_t::Context &ctx, 
			  const class Properties &args);
  void setLogFilter(Parser_t::Context &ctx, const class Properties &args);
  void configLock(Parser_t::Context &ctx, const class Properties &args);
  void configUnlock(Parser_t::Context &ctx, const class Properties &args);
  void configChange(Parser_t::Context &ctx, const class Properties &args);

  void repCommand(Parser_t::Context &ctx, const class Properties &args);
};

class MgmApiService : public SocketServer::Service {
  class MgmtSrvr * m_mgmsrv;
public:
  MgmApiService(){
    m_mgmsrv = 0;
  }
  
  void setMgm(class MgmtSrvr * mgmsrv){
    m_mgmsrv = mgmsrv;
  }
  
  MgmApiSession * newSession(NDB_SOCKET_TYPE socket){
    return new MgmApiSession(* m_mgmsrv, socket);
  }
};

class MgmStatService : public SocketServer::Service, 
		       public MgmtSrvr::StatisticsListner
{
  class MgmtSrvr * m_mgmsrv;
  MutexVector<NDB_SOCKET_TYPE> m_sockets;
public:
  MgmStatService() : m_sockets(5) {
    m_mgmsrv = 0;
  }
  
  void setMgm(class MgmtSrvr * mgmsrv){
    m_mgmsrv = mgmsrv;
  }
  
  SocketServer::Session * newSession(NDB_SOCKET_TYPE socket){
    m_sockets.push_back(socket);
    m_mgmsrv->startStatisticEventReporting(5);
    return 0;
  }
  
  void stopSessions();
  
  void println_statistics(const BaseString &line);
};
#endif