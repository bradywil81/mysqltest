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

#include <signaldata/DumpStateOrd.hpp>
#include <NdbBackup.hpp>
#include <NdbOut.hpp>
#include <NDBT_Output.hpp>
#include <NdbConfig.h>
#include <ConfigRetriever.hpp>
#include <ndb_version.h>
#include <NDBT.hpp>
#include <NdbSleep.h>
#include <random.h>
#include <NdbTick.h>

#define CHECK(b, m) { int _xx = b; if (!(_xx)) { \
  ndbout << "ERR: "<< m \
           << "   " << "File: " << __FILE__ \
           << " (Line: " << __LINE__ << ")" << "- " << _xx << endl; \
  return NDBT_FAILED; } }


int 
NdbBackup::start(unsigned int & _backup_id){

  
  if (!isConnected())
    return -1;

  ndb_mgm_reply reply;
  reply.return_code = 0;

  if (ndb_mgm_start_backup(handle, 
			   &_backup_id,
			   &reply) == -1) {
    g_err  << "Could not start backup " << endl;
    g_err << "Error: " << reply.message << endl;
    return -1;
  }

  if(reply.return_code != 0){
    g_err  << "PLEASE CHECK CODE NdbBackup.cpp line=" << __LINE__ << endl;
    g_err << "Error: " << reply.message << endl;
    return reply.return_code;
  }
  return 0;
}


const char * 
NdbBackup::getFileSystemPathForNode(int _node_id){

  /**
   * Fetch configuration from management server
   */
  ConfigRetriever cr;


  Properties * p = cr.getConfig(host,
				port,
				_node_id, 
				NDB_VERSION);
  if(p == 0){
    const char * s = cr.getErrorString();
    if(s == 0)
      s = "No error given!";

    ndbout << "Could not fetch configuration" << endl;
    ndbout << s << endl;
    return NULL;
  }
  
  /**
   * Setup cluster configuration data
   */
  const Properties * db = 0;
  if (!p->get("Node", _node_id, &db)) {
    ndbout << "Invalid configuration fetched, DB missing" << endl;
    return NULL;
  }
  const char * type;
  if(!(db->get("Type", &type) && strcmp(type, "DB") == 0)){
    ndbout <<"Invalid configuration fetched, I'm wrong type of node" << endl;
    return NULL;
  }  

  const char * path;
  if (!db->get("FileSystemPath", &path)){
    ndbout << "FileSystemPath not found" << endl;
    return NULL;
  }

  return path;

}

int  
NdbBackup::execRestore(bool _restore_data,
		       bool _restore_meta,
		       int _node_id,
		       unsigned _backup_id){
  const int buf_len = 1000;
  char buf[buf_len];

  const char* path = getFileSystemPathForNode(_node_id);
  if (path == NULL)
    return -1;  

  const char *host;
  if (!getHostName(_node_id, &host)){
    return -1;
  }

  /* 
   * Copy  backup files to local dir
   */ 

  snprintf(buf, buf_len,
	   "scp %s:%s/BACKUP/BACKUP-%d/* .",
	   host, path,
	   _backup_id);

  ndbout << "buf: "<< buf <<endl;
  int res = system(buf);  
  
  ndbout << "res: " << res << endl;
  
#if 0
  snprintf(buf, 255, "restore -c \"nodeid=%d;host=%s\" -n %d -b %d %s %s %s/BACKUP/BACKUP-%d", 
	   ownNodeId,
	   addr,
	   _node_id, 
	   _backup_id,
	   _restore_data?"-r":"",
	   _restore_meta?"-m":"",
	   path,
	   _backup_id);

#endif

  snprintf(buf, 255, "restore -c \"nodeid=%d;host=%s\" -n %d -b %d %s %s .", 
	   ownNodeId,
	   addr,
	   _node_id, 
	   _backup_id,
	   _restore_data?"-r":"",
	   _restore_meta?"-m":"");

  //	   path,
  //	   _backup_id);


  ndbout << "buf: "<< buf <<endl;
  res = system(buf);  

  ndbout << "res: " << res << endl;

  return res;
  
}

int 
NdbBackup::restore(unsigned _backup_id){
  
  if (!isConnected())
    return -1;

  if (getStatus() != 0)
    return -1;

  int res; 
  if ( ndbNodes.size() == 1) {
    // restore metadata and data in one call
      res = execRestore(true, true, ndbNodes[0].node_id, _backup_id);
  } else {
    assert(ndbNodes.size() > 1);

    // restore metadata first
    res = execRestore(false, true, ndbNodes[0].node_id, _backup_id);
    

    // Restore data once for each node
    for(size_t i = 0; i < ndbNodes.size(); i++){     
      res = execRestore(true, false, ndbNodes[i].node_id, _backup_id);
    }
  }
  
  return 0;
}

// Master failure
int
NFDuringBackupM_codes[] = {
  10003,
  10004,
  10005,
  10007,
  10008,
  10009,
  10010,
  10012,
  10013
};

// Slave failure
int
NFDuringBackupS_codes[] = {
  10014,
  10015,
  10016,
  10017,
  10018,
  10020
};

// Master takeover etc...
int
NFDuringBackupSL_codes[] = {
  10001,
  10002,
  10021
};

int 
NdbBackup::NFMaster(NdbRestarter& _restarter){
  const int sz = sizeof(NFDuringBackupM_codes)/sizeof(NFDuringBackupM_codes[0]);
  return NF(_restarter, NFDuringBackupM_codes, sz, true);
}

int 
NdbBackup::NFMasterAsSlave(NdbRestarter& _restarter){
  const int sz = sizeof(NFDuringBackupS_codes)/sizeof(NFDuringBackupS_codes[0]);
  return NF(_restarter, NFDuringBackupS_codes, sz, true);
}

int 
NdbBackup::NFSlave(NdbRestarter& _restarter){
  const int sz = sizeof(NFDuringBackupS_codes)/sizeof(NFDuringBackupS_codes[0]);
  return NF(_restarter, NFDuringBackupS_codes, sz, false);
}

int 
NdbBackup::NF(NdbRestarter& _restarter, int *NFDuringBackup_codes, const int sz, bool onMaster){
  {
    int nodeId = _restarter.getMasterNodeId();

    CHECK(_restarter.restartOneDbNode(nodeId, false, true, true) == 0,
	  "Could not restart node "<< nodeId);
    
    CHECK(_restarter.waitNodesNoStart(&nodeId, 1) == 0,
	  "waitNodesNoStart failed");
    
    CHECK(_restarter.startNodes(&nodeId, 1) == 0,
	  "failed to start node");

    NdbSleep_SecSleep(10);
  }

  CHECK(_restarter.waitClusterStarted() == 0,
	"waitClusterStarted failed");

  int nNodes = _restarter.getNumDbNodes();

  myRandom48Init(NdbTick_CurrentMillisecond());

  for(int i = 0; i<sz; i++){

    int error = NFDuringBackup_codes[i];
    unsigned int backupId;

    const int masterNodeId = _restarter.getMasterNodeId();
    CHECK(masterNodeId > 0, "getMasterNodeId failed");
    int nodeId;

    nodeId = masterNodeId;
    if (!onMaster) {
      int randomId;
      while (nodeId == masterNodeId) {
	randomId = myRandom48(nNodes);
	nodeId = _restarter.getDbNodeId(randomId);
      }
    }

    g_err << "NdbBackup::NF node = " << nodeId 
	   << " error code = " << error << " masterNodeId = "
	   << masterNodeId << endl;


    int val = DumpStateOrd::CmvmiSetRestartOnErrorInsert;
    CHECK(_restarter.dumpStateOneNode(nodeId, &val, 1) == 0,
	  "failed to set RestartOnErrorInsert");
    CHECK(_restarter.insertErrorInNode(nodeId, error) == 0,
	  "failed to set error insert");
   
    g_info << "error inserted"  << endl;

    g_info << "starting backup"  << endl;
    int r = start(backupId);
    g_info << "r = " << r
	   << " (which should fail) started with id = "  << backupId << endl;
    if (r == 0) {
      g_err << "Backup should have failed on error_insertion " << error << endl
	    << "Master = " << masterNodeId << "Node = " << nodeId << endl;
    }

    CHECK(_restarter.waitNodesNoStart(&nodeId, 1) == 0,
	  "waitNodesNoStart failed");

    g_info << "number of nodes running " << _restarter.getNumDbNodes() << endl;

    if (_restarter.getNumDbNodes() != nNodes) {
      g_err << "Failure: cluster not up" << endl;
      return NDBT_FAILED;
    }

    NdbSleep_SecSleep(1);

    g_info << "starting new backup"  << endl;
    CHECK(start(backupId) == 0,
	  "failed to start backup");
    g_info << "(which should succeed) started with id = "  << backupId << endl;

    g_info << "starting node"  << endl;
    CHECK(_restarter.startNodes(&nodeId, 1) == 0,
	  "failed to start node");

    CHECK(_restarter.waitClusterStarted() == 0,
	  "waitClusterStarted failed");
    g_info << "node started"  << endl;

    CHECK(_restarter.insertErrorInNode(nodeId, 10099) == 0,
	  "failed to set error insert");
  }

  return NDBT_OK;
};

int
FailS_codes[] = {
  10023,
  10024,
  10025,
  10026,
  10027,
  10028,
  10029,
  10030,
  10031
};

int
FailM_codes[] = {
  10023,
  10024,
  10025,
  10026,
  10027,
  10028,
  10029,
  10030,
  10031
};

int 
NdbBackup::FailMaster(NdbRestarter& _restarter){
  const int sz = sizeof(FailM_codes)/sizeof(FailM_codes[0]);
  return Fail(_restarter, FailM_codes, sz, true);
}

int 
NdbBackup::FailMasterAsSlave(NdbRestarter& _restarter){
  const int sz = sizeof(FailS_codes)/sizeof(FailS_codes[0]);
  return Fail(_restarter, FailS_codes, sz, true);
}

int 
NdbBackup::FailSlave(NdbRestarter& _restarter){
  const int sz = sizeof(FailS_codes)/sizeof(FailS_codes[0]);
  return Fail(_restarter, FailS_codes, sz, false);
}

int 
NdbBackup::Fail(NdbRestarter& _restarter, int *Fail_codes, const int sz, bool onMaster){

  CHECK(_restarter.waitClusterStarted() == 0,
	"waitClusterStarted failed");

  int nNodes = _restarter.getNumDbNodes();

  myRandom48Init(NdbTick_CurrentMillisecond());

  for(int i = 0; i<sz; i++){
    int error = Fail_codes[i];
    unsigned int backupId;

    const int masterNodeId = _restarter.getMasterNodeId();
    CHECK(masterNodeId > 0, "getMasterNodeId failed");
    int nodeId;

    nodeId = masterNodeId;
    if (!onMaster) {
      int randomId;
      while (nodeId == masterNodeId) {
	randomId = myRandom48(nNodes);
	nodeId = _restarter.getDbNodeId(randomId);
      }
    }

    g_err << "NdbBackup::Fail node = " << nodeId 
	   << " error code = " << error << " masterNodeId = "
	   << masterNodeId << endl;

    CHECK(_restarter.insertErrorInNode(nodeId, error) == 0,
	  "failed to set error insert");
   
    g_info << "error inserted"  << endl;
    g_info << "waiting some before starting backup"  << endl;

    g_info << "starting backup"  << endl;
    int r = start(backupId);
    g_info << "r = " << r
	   << " (which should fail) started with id = "  << backupId << endl;
    if (r == 0) {
      g_err << "Backup should have failed on error_insertion " << error << endl
	    << "Master = " << masterNodeId << "Node = " << nodeId << endl;
    }

    CHECK(_restarter.waitClusterStarted() == 0,
	  "waitClusterStarted failed");

    CHECK(_restarter.insertErrorInNode(nodeId, 10099) == 0,
	  "failed to set error insert");
  }

  return NDBT_OK;
}
