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

#define DBTUP_C
#include "Dbtup.hpp"
#include <RefConvert.hpp>
#include <ndb_limits.h>
#include <pc.hpp>
#include <AttributeDescriptor.hpp>
#include "AttributeOffset.hpp"
#include <AttributeHeader.hpp>
#include <signaldata/TupAccess.hpp>
#include <signaldata/TuxMaint.hpp>

#define ljam() { jamLine(28000 + __LINE__); }
#define ljamEntry() { jamEntryLine(28000 + __LINE__); }

// methods used by ordered index

void
Dbtup::execTUP_READ_ATTRS(Signal* signal)
{
  ljamEntry();
  TupReadAttrs* const sig = (TupReadAttrs*)signal->getDataPtrSend();
  TupReadAttrs reqCopy = *sig;
  TupReadAttrs* const req = &reqCopy;
  req->errorCode = 0;
  // get table
  TablerecPtr tablePtr;
  tablePtr.i = req->tableId;
  ptrCheckGuard(tablePtr, cnoOfTablerec, tablerec);
  // get fragment
  FragrecordPtr fragPtr;
  if (req->fragPtrI == RNIL) {
    ljam();
    getFragmentrec(fragPtr, req->fragId, tablePtr.p);
    ndbrequire(fragPtr.i != RNIL);
    req->fragPtrI = fragPtr.i;
  } else {
    fragPtr.i = req->fragPtrI;
    ptrCheckGuard(fragPtr, cnoOfFragrec, fragrecord);
    ndbrequire(req->fragId == fragPtr.p->fragmentId);
  }
  // get page
  PagePtr pagePtr;
  if (req->pageId == RNIL) {
    ljam();
    Uint32 fragPageId = req->tupAddr >> MAX_TUPLES_BITS;
    Uint32 pageIndex = req->tupAddr & ((1 << MAX_TUPLES_BITS ) - 1);
    ndbrequire((pageIndex & 0x1) == 0);
    // data returned for original tuple
    req->pageId = getRealpid(fragPtr.p, fragPageId);
    req->pageOffset = ZPAGE_HEADER_SIZE + (pageIndex >> 1) * tablePtr.p->tupheadsize;
  }
  pagePtr.i = req->pageId;
  ptrCheckGuard(pagePtr, cnoOfPage, page);
  Uint32 pageOffset = req->pageOffset;
  // search for tuple version if not original
  if (! (req->requestInfo & TupReadAttrs::ReadKeys) &&
      pagePtr.p->pageWord[pageOffset + 1] != req->tupVersion) {
    ljam();
    OperationrecPtr opPtr;
    opPtr.i = pagePtr.p->pageWord[pageOffset];
    Uint32 loopGuard = 0;
    while (true) {
      ptrCheckGuard(opPtr, cnoOfOprec, operationrec);
      if (opPtr.p->realPageIdC != RNIL) {
        pagePtr.i = opPtr.p->realPageIdC;
        pageOffset = opPtr.p->pageOffsetC;
        ptrCheckGuard(pagePtr, cnoOfPage, page);
        if (pagePtr.p->pageWord[pageOffset + 1] == req->tupVersion) {
          ljam();
          break;
        }
      }
      ljam();
      // next means before in event order
      opPtr.i = opPtr.p->nextActiveOp;
      ndbrequire(++loopGuard < (1 << ZTUP_VERSION_BITS));
    }
  }
  // shared buffer
  Uint32* buffer = (Uint32*)sig + TupReadAttrs::SignalLength;
  // if request is for keys then we create input section
  if (req->requestInfo & TupReadAttrs::ReadKeys) {
    ljam();
    buffer[0] = tablePtr.p->noOfKeyAttr;
    const Uint32* keyArray = &tableDescriptor[tablePtr.p->readKeyArray].tabDescr;
    MEMCOPY_NO_WORDS(&buffer[1], keyArray, tablePtr.p->noOfKeyAttr);
  }
  Uint32 inBufLen = buffer[0];
  Uint32* inBuffer = &buffer[1];
  Uint32* outBuffer = &buffer[1 + inBufLen];
  Uint32 maxRead = ZATTR_BUFFER_SIZE;
  // save globals
  TablerecPtr tabptr_old = tabptr;
  FragrecordPtr fragptr_old = fragptr;
  OperationrecPtr operPtr_old = operPtr;
  // new globals
  tabptr = tablePtr;
  fragptr = fragPtr;
  operPtr.i = RNIL;     // XXX check later
  operPtr.p = NULL;
  int ret = readAttributes(pagePtr.p, pageOffset, inBuffer, inBufLen, outBuffer, maxRead);
  // restore globals
  tabptr = tabptr_old;
  fragptr = fragptr_old;
  operPtr = operPtr_old;
  // check error
  if ((Uint32)ret == (Uint32)-1) {
    ljam();
    req->errorCode = terrorCode;
  }
  // copy back
  *sig = *req;
}

void
Dbtup::execTUP_QUERY_TH(Signal* signal)
{
  ljamEntry();
  Operationrec tempOp;
  TupQueryTh* const req = (TupQueryTh*)signal->getDataPtrSend();
  Uint32 tableId = req->tableId;
  Uint32 fragId = req->fragId;
  Uint32 tupAddr = req->tupAddr;
  Uint32 req_tupVersion = req->tupVersion;
  Uint32 transid1 = req->transId1;
  Uint32 transid2 = req->transId2;
  Uint32 savePointId = req->savePointId;
  Uint32 ret_result = 0;
  // get table
  TablerecPtr tablePtr;
  tablePtr.i = tableId;
  ptrCheckGuard(tablePtr, cnoOfTablerec, tablerec);
  // get fragment
  FragrecordPtr fragPtr;
  getFragmentrec(fragPtr, fragId, tablePtr.p);
  ndbrequire(fragPtr.i != RNIL);
  // get page
  PagePtr pagePtr;
  Uint32 fragPageId = tupAddr >> MAX_TUPLES_BITS;
  Uint32 pageIndex = tupAddr & ((1 << MAX_TUPLES_BITS ) - 1);

  tempOp.fragPageId = fragPageId;
  tempOp.pageIndex = pageIndex;
  tempOp.transid1 = transid1;
  tempOp.transid2 = transid2;
  tempOp.savePointId = savePointId;
  tempOp.optype = ZREAD;
  tempOp.dirtyOp = 1;
  if (getPage(pagePtr, &tempOp, fragPtr.p, tablePtr.p)) {
    /*
    We use the normal getPage which will return the tuple to be used
    for this transaction and savepoint id. If its tuple version equals
    the requested then we have a visible tuple otherwise not.
    */
    jam();
    Uint32 read_tupVersion = pagePtr.p->pageWord[tempOp.pageOffset + 1];
    if (read_tupVersion == req_tupVersion) {
      jam();
      ret_result = 1;
    }
  }
  req->returnCode = ret_result;
  return;
}

void
Dbtup::execTUP_STORE_TH(Signal* signal)
{
  ljamEntry();
  TupStoreTh* const sig = (TupStoreTh*)signal->getDataPtrSend();
  TupStoreTh reqCopy = *sig;
  TupStoreTh* const req = &reqCopy;
  req->errorCode = 0;
  ndbrequire(req->tupVersion == 0);
  // get table
  TablerecPtr tablePtr;
  tablePtr.i = req->tableId;
  ptrCheckGuard(tablePtr, cnoOfTablerec, tablerec);
  // offset to attribute 0
  Uint32 attrDescIndex = tablePtr.p->tabDescriptor + (0 << ZAD_LOG_SIZE);
  Uint32 attrDataOffset = AttributeOffset::getOffset(tableDescriptor[attrDescIndex + 1].tabDescr);
  // get fragment
  FragrecordPtr fragPtr;
  if (req->fragPtrI == RNIL) {
    ljam();
    getFragmentrec(fragPtr, req->fragId, tablePtr.p);
    ndbrequire(fragPtr.i != RNIL);
    req->fragPtrI = fragPtr.i;
  } else {
    fragPtr.i = req->fragPtrI;
    ptrCheckGuard(fragPtr, cnoOfFragrec, fragrecord);
    ndbrequire(req->fragId == fragPtr.p->fragmentId);
  }
  // handle each case
  switch (req->opCode) {
  case TupStoreTh::OpRead:
    ljam();
    {
      PagePtr pagePtr;
      if (req->pageId == RNIL) {
        ljam();
        Uint32 fragPageId = req->tupAddr >> MAX_TUPLES_BITS;
        Uint32 pageIndex = req->tupAddr & ((1 << MAX_TUPLES_BITS ) - 1);
        ndbrequire((pageIndex & 0x1) == 0);
        req->pageId = getRealpid(fragPtr.p, fragPageId);
        req->pageOffset = ZPAGE_HEADER_SIZE + (pageIndex >> 1) * tablePtr.p->tupheadsize;
      }
      pagePtr.i = req->pageId;
      ptrCheckGuard(pagePtr, cnoOfPage, page);
      Uint32* data = &pagePtr.p->pageWord[req->pageOffset] + attrDataOffset;
      Uint32* buffer = (Uint32*)sig + TupStoreTh::SignalLength;
      ndbrequire(req->dataOffset + req->dataSize <= tablePtr.p->tupheadsize);
      memcpy(buffer + req->dataOffset, data + req->dataOffset, req->dataSize << 2);
    }
    break;
  case TupStoreTh::OpInsert:
    ljam();
    {
      PagePtr pagePtr;
      if (! allocTh(fragPtr.p, tablePtr.p, NORMAL_PAGE, signal, req->pageOffset, pagePtr)) {
        ljam();
        req->errorCode = terrorCode;
        break;
      }
      req->pageId = pagePtr.i;
      Uint32 fragPageId = pagePtr.p->pageWord[ZPAGE_FRAG_PAGE_ID_POS];
      Uint32 pageIndex = ((req->pageOffset - ZPAGE_HEADER_SIZE) / tablePtr.p->tupheadsize) << 1;
      req->tupAddr = (fragPageId << MAX_TUPLES_BITS) | pageIndex;
      ndbrequire(req->dataOffset + req->dataSize <= tablePtr.p->tupheadsize);
      Uint32* data = &pagePtr.p->pageWord[req->pageOffset] + attrDataOffset;
      Uint32* buffer = (Uint32*)sig + TupStoreTh::SignalLength;
      memcpy(data + req->dataOffset, buffer + req->dataOffset, req->dataSize << 2);
    }
    break;
  case TupStoreTh::OpUpdate:
    ljam();
    {
      PagePtr pagePtr;
      if (req->pageId == RNIL) {
        ljam();
        Uint32 fragPageId = req->tupAddr >> MAX_TUPLES_BITS;
        Uint32 pageIndex = req->tupAddr & ((1 << MAX_TUPLES_BITS ) - 1);
        ndbrequire((pageIndex & 0x1) == 0);
        req->pageId = getRealpid(fragPtr.p, fragPageId);
        req->pageOffset = ZPAGE_HEADER_SIZE + (pageIndex >> 1) * tablePtr.p->tupheadsize;
      }
      pagePtr.i = req->pageId;
      ptrCheckGuard(pagePtr, cnoOfPage, page);
      Uint32* data = &pagePtr.p->pageWord[req->pageOffset] + attrDataOffset;
      Uint32* buffer = (Uint32*)sig + TupStoreTh::SignalLength;
      ndbrequire(req->dataOffset + req->dataSize <= tablePtr.p->tupheadsize);
      memcpy(data + req->dataOffset, buffer + req->dataOffset, req->dataSize << 2);
    }
    break;
  case TupStoreTh::OpDelete:
    ljam();
    {
      PagePtr pagePtr;
      if (req->pageId == RNIL) {
        ljam();
        Uint32 fragPageId = req->tupAddr >> MAX_TUPLES_BITS;
        Uint32 pageIndex = req->tupAddr & ((1 << MAX_TUPLES_BITS ) - 1);
        ndbrequire((pageIndex & 0x1) == 0);
        req->pageId = getRealpid(fragPtr.p, fragPageId);
        req->pageOffset = ZPAGE_HEADER_SIZE + (pageIndex >> 1) * tablePtr.p->tupheadsize;
      }
      pagePtr.i = req->pageId;
      ptrCheckGuard(pagePtr, cnoOfPage, page);
      freeTh(fragPtr.p, tablePtr.p, signal, pagePtr.p, req->pageOffset);
      // null location
      req->tupAddr = (Uint32)-1;
      req->pageId = RNIL;
      req->pageOffset = 0;
    }
    break;
  }
  // copy back
  *sig = *req;
}

// ordered index build

//#define TIME_MEASUREMENT
#ifdef TIME_MEASUREMENT
  static Uint32 time_events;
  NDB_TICKS tot_time_passed;
  Uint32 number_events;
#endif
void
Dbtup::execBUILDINDXREQ(Signal* signal)
{
  ljamEntry();
#ifdef TIME_MEASUREMENT
  time_events = 0;
  tot_time_passed = 0;
  number_events = 1;
#endif
  // get new operation
  BuildIndexPtr buildPtr;
  if (! c_buildIndexList.seize(buildPtr)) {
    ljam();
    BuildIndexRec buildRec;
    memcpy(buildRec.m_request, signal->theData, sizeof(buildRec.m_request));
    buildRec.m_errorCode = BuildIndxRef::Busy;
    buildIndexReply(signal, &buildRec);
    return;
  }
  memcpy(buildPtr.p->m_request, signal->theData, sizeof(buildPtr.p->m_request));
  // check
  buildPtr.p->m_errorCode = BuildIndxRef::NoError;
  do {
    const BuildIndxReq* buildReq = (const BuildIndxReq*)buildPtr.p->m_request;
    if (buildReq->getTableId() >= cnoOfTablerec) {
      ljam();
      buildPtr.p->m_errorCode = BuildIndxRef::InvalidPrimaryTable;
      break;
    }
    TablerecPtr tablePtr;
    tablePtr.i = buildReq->getTableId();
    ptrCheckGuard(tablePtr, cnoOfTablerec, tablerec);
    if (tablePtr.p->tableStatus != DEFINED) {
      ljam();
      buildPtr.p->m_errorCode = BuildIndxRef::InvalidPrimaryTable;
      break;
    }
    if (! DictTabInfo::isOrderedIndex(buildReq->getIndexType())) {
      ljam();
      buildPtr.p->m_errorCode = BuildIndxRef::InvalidIndexType;
      break;
    }
    const ArrayList<TupTriggerData>& triggerList = tablePtr.p->tuxCustomTriggers;
    TriggerPtr triggerPtr;
    triggerList.first(triggerPtr);
    while (triggerPtr.i != RNIL) {
      if (triggerPtr.p->indexId == buildReq->getIndexId()) {
        ljam();
        break;
      }
      triggerList.next(triggerPtr);
    }
    if (triggerPtr.i == RNIL) {
      ljam();
      // trigger was not created
      buildPtr.p->m_errorCode = BuildIndxRef::InternalError;
      break;
    }
    buildPtr.p->m_triggerPtrI = triggerPtr.i;
    // set to first tuple position
    buildPtr.p->m_fragNo = 0;
    buildPtr.p->m_pageId = 0;
    buildPtr.p->m_tupleNo = 0;
    // start build
    buildIndex(signal, buildPtr.i);
    return;
  } while (0);
  // check failed
  buildIndexReply(signal, buildPtr.p);
  c_buildIndexList.release(buildPtr);
}

void
Dbtup::buildIndex(Signal* signal, Uint32 buildPtrI)
{
  // get build record
  BuildIndexPtr buildPtr;
  buildPtr.i = buildPtrI;
  c_buildIndexList.getPtr(buildPtr);
  const BuildIndxReq* buildReq = (const BuildIndxReq*)buildPtr.p->m_request;
  // get table
  TablerecPtr tablePtr;
  tablePtr.i = buildReq->getTableId();
  ptrCheckGuard(tablePtr, cnoOfTablerec, tablerec);
  // get trigger
  TriggerPtr triggerPtr;
  triggerPtr.i = buildPtr.p->m_triggerPtrI;
  c_triggerPool.getPtr(triggerPtr);
  ndbrequire(triggerPtr.p->indexId == buildReq->getIndexId());
#ifdef TIME_MEASUREMENT
  MicroSecondTimer start;
  MicroSecondTimer stop;
  NDB_TICKS time_passed;
#endif
  do {
    // get fragment
    FragrecordPtr fragPtr;
    if (buildPtr.p->m_fragNo == 2 * NO_OF_FRAG_PER_NODE) {
      ljam();
      // build ready
      buildIndexReply(signal, buildPtr.p);
      c_buildIndexList.release(buildPtr);
      return;
    }
    ndbrequire(buildPtr.p->m_fragNo < 2 * NO_OF_FRAG_PER_NODE);
    fragPtr.i = tablePtr.p->fragrec[buildPtr.p->m_fragNo];
    if (fragPtr.i == RNIL) {
      ljam();
      buildPtr.p->m_fragNo++;
      buildPtr.p->m_pageId = 0;
      buildPtr.p->m_tupleNo = 0;
      break;
    }
    ptrCheckGuard(fragPtr, cnoOfFragrec, fragrecord);
    // get page
    PagePtr pagePtr;
    if (buildPtr.p->m_pageId >= fragPtr.p->noOfPages) {
      ljam();
      buildPtr.p->m_fragNo++;
      buildPtr.p->m_pageId = 0;
      buildPtr.p->m_tupleNo = 0;
      break;
    }
    pagePtr.i = getRealpid(fragPtr.p, buildPtr.p->m_pageId);
    ptrCheckGuard(pagePtr, cnoOfPage, page);
    const Uint32 pageState = pagePtr.p->pageWord[ZPAGE_STATE_POS];
    if (pageState != ZTH_MM_FREE &&
        pageState != ZTH_MM_FREE_COPY &&
        pageState != ZTH_MM_FULL &&
        pageState != ZTH_MM_FULL_COPY) {
      ljam();
      buildPtr.p->m_pageId++;
      buildPtr.p->m_tupleNo = 0;
      break;
    }
    // get tuple
    const Uint32 tupheadsize = tablePtr.p->tupheadsize;
    const Uint32 pageOffset = ZPAGE_HEADER_SIZE +
                              buildPtr.p->m_tupleNo * tupheadsize;
    if (pageOffset + tupheadsize > ZWORDS_ON_PAGE) {
      ljam();
      buildPtr.p->m_pageId++;
      buildPtr.p->m_tupleNo = 0;
      break;
    }
    // skip over free tuple
    bool isFree = false;
    if (pageState == ZTH_MM_FREE ||
        pageState == ZTH_MM_FREE_COPY) {
      ljam();
      if ((pagePtr.p->pageWord[pageOffset] >> 16) == tupheadsize) {
        // verify it really is free  XXX far too expensive
        Uint32 nextTuple = pagePtr.p->pageWord[ZFREELIST_HEADER_POS] >> 16;
        ndbrequire(nextTuple != 0);
        while (nextTuple != 0) {
          ljam();
          if (nextTuple == pageOffset) {
            ljam();
            isFree = true;
            break;
          }
          nextTuple = pagePtr.p->pageWord[nextTuple] & 0xffff;
        }
      }
    }
    if (isFree) {
      ljam();
      buildPtr.p->m_tupleNo++;
      break;
    }
    OperationrecPtr pageOperPtr;
    pageOperPtr.i = pagePtr.p->pageWord[pageOffset];
    Uint32 pageId = buildPtr.p->m_pageId;
    Uint32 pageIndex = buildPtr.p->m_tupleNo << 1;
    if (pageOperPtr.i != RNIL) {
      /*
      If there is an ongoing operation on the tuple then it is either a
      copy tuple or an original tuple with an ongoing transaction. In
      both cases fragPageId and pageIndex refers to the original tuple.
      The tuple address stored in TUX will always be the original tuple
      but with the tuple version of the tuple we found.

      This is necessary to avoid having to update TUX at abort of
      update. If an update aborts then the copy tuple is copied to
      the original tuple. The build will however have found that
      tuple as a copy tuple. The original tuple is stable and is thus
      preferrable to store in TUX.
      */
      jam();
      ptrCheckGuard(pageOperPtr, cnoOfOprec, operationrec);
      pageId = pageOperPtr.p->fragPageId;
      pageIndex = pageOperPtr.p->pageIndex;
    }//if
    Uint32 tup_version = pagePtr.p->pageWord[pageOffset + 1];
#ifdef TIME_MEASUREMENT
    NdbTick_getMicroTimer(&start);
#endif
    // add to index
    TuxMaintReq* const req = (TuxMaintReq*)signal->getDataPtrSend();
    req->errorCode = RNIL;
    req->tableId = tablePtr.i;
    req->indexId = triggerPtr.p->indexId;
    req->fragId = tablePtr.p->fragid[buildPtr.p->m_fragNo];
    req->tupAddr = (pageId << MAX_TUPLES_BITS) | pageIndex;
    req->tupVersion = tup_version;
    req->opInfo = TuxMaintReq::OpAdd;
    EXECUTE_DIRECT(DBTUX, GSN_TUX_MAINT_REQ,
        signal, TuxMaintReq::SignalLength);
    ljamEntry();
    if (req->errorCode != 0) {
      switch (req->errorCode) {
      case TuxMaintReq::NoMemError:
        ljam();
        buildPtr.p->m_errorCode = BuildIndxRef::AllocationFailure;
        break;
      default:
        ndbrequire(false);
        break;
      }
      buildIndexReply(signal, buildPtr.p);
      c_buildIndexList.release(buildPtr);
      return;
    }
#ifdef TIME_MEASUREMENT
    NdbTick_getMicroTimer(&stop);
    time_passed = NdbTick_getMicrosPassed(start, stop);
    if (time_passed < 1000) {
      time_events++;
      tot_time_passed += time_passed;
      if (time_events == number_events) {
        NDB_TICKS mean_time_passed = tot_time_passed / (NDB_TICKS)number_events;
        ndbout << "Number of events = " << number_events;
        ndbout << " Mean time passed = " << mean_time_passed << endl;
        number_events <<= 1;
        tot_time_passed = (NDB_TICKS)0;
        time_events = 0;
      }//if
    }
#endif
    // next tuple
    buildPtr.p->m_tupleNo++;
    break;
  } while (0);
  signal->theData[0] = ZBUILD_INDEX;
  signal->theData[1] = buildPtr.i;
  sendSignal(reference(), GSN_CONTINUEB, signal, 2, JBB);
}

void
Dbtup::buildIndexReply(Signal* signal, const BuildIndexRec* buildPtrP)
{
  const BuildIndxReq* const buildReq = (const BuildIndxReq*)buildPtrP->m_request;
  // conf is subset of ref
  BuildIndxRef* rep = (BuildIndxRef*)signal->getDataPtr();
  rep->setUserRef(buildReq->getUserRef());
  rep->setConnectionPtr(buildReq->getConnectionPtr());
  rep->setRequestType(buildReq->getRequestType());
  rep->setTableId(buildReq->getTableId());
  rep->setIndexType(buildReq->getIndexType());
  rep->setIndexId(buildReq->getIndexId());
  // conf
  if (buildPtrP->m_errorCode == BuildIndxRef::NoError) {
    ljam();
    sendSignal(rep->getUserRef(), GSN_BUILDINDXCONF,
        signal, BuildIndxConf::SignalLength, JBB);
    return;
  }
  // ref
  rep->setErrorCode(buildPtrP->m_errorCode);
  sendSignal(rep->getUserRef(), GSN_BUILDINDXREF,
      signal, BuildIndxRef::SignalLength, JBB);
}