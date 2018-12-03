#include "OECWorker.hh"

OECWorker::OECWorker(Config* conf) : _conf(conf) {
  // create local context
  try {
    _processCtx = RedisUtil::createContext(_conf -> _localIp);
    _localCtx = RedisUtil::createContext(_conf -> _localIp);
    _coorCtx = RedisUtil::createContext(_conf -> _coorIp);
  } catch (int e) {
    // TODO: error handling
    cerr << "initializing redis context error" << endl;
  }

  _underfs = FSUtil::createFS(_conf->_fsType, _conf->_fsFactory[_conf->_fsType], _conf);

  // tune performance
  FSObjOutputStream* tuneobjout = new FSObjOutputStream(_conf, "/tmp/tuneoecout", _underfs, 0);
  delete tuneobjout;
  FSObjInputStream* tuneobjin = new FSObjInputStream(_conf, "/tmp/tuneoecout", _underfs);
  delete tuneobjin;
}

OECWorker::~OECWorker() {
  redisFree(_localCtx);
  redisFree(_processCtx);
  redisFree(_coorCtx);
  delete _underfs;
}

void OECWorker::doProcess() {
  redisReply* rReply;
  while (true) {
    cout << "OECWorker::doProcess" << endl;  
    // will never stop looping
    rReply = (redisReply*)redisCommand(_processCtx, "blpop ag_request 0");
    if (rReply -> type == REDIS_REPLY_NIL) {
      cerr << "OECWorker::doProcess() get feed back empty queue " << endl;
      //freeReplyObject(rReply);
    } else if (rReply -> type == REDIS_REPLY_ERROR) {
      cerr << "OECWorker::doProcess() get feed back ERROR happens " << endl;
    } else {
      struct timeval time1, time2;
      gettimeofday(&time1, NULL);
      char* reqStr = rReply -> element[1] -> str;
      AGCommand* agCmd = new AGCommand(reqStr);
      int type = agCmd->getType();
      cout << "OECWorker::doProcess() receive a request of type " << type << endl;
      agCmd->dump();
      switch (type) {
        case 0: clientWrite(agCmd); break;
        case 1: clientRead(agCmd); break;
        case 2: readDisk(agCmd); break;
        case 3: fetchCompute(agCmd); break;
        case 5: persist(agCmd); break;
//        case 6: readDiskList(agCmd); break;
//        case 7: readFetchCompute(agCmd); break;
        default:break;
      }
//      gettimeofday(&time2, NULL);
//      cout << "OECWorker::doProcess().duration = " << RedisUtil::duration(time1, time2) << endl;
      // delete agCmd
      delete agCmd;
    }
    // free reply object
    freeReplyObject(rReply); 
  }
}

void OECWorker::clientWrite(AGCommand* agcmd) {
  cout << "OECWorker::clientWrite" << endl;
  string filename = agcmd->getFilename();
  string ecid = agcmd->getEcid();
  string mode = agcmd->getMode();
  int filesizeMB = agcmd->getFilesizeMB();
  if (mode == "online") onlineWrite(filename, ecid, filesizeMB);
  else if (mode == "offline") offlineWrite(filename, ecid, filesizeMB);
}

void OECWorker::onlineWrite(string filename, string ecid, int filesizeMB) {
  cout <<"OECWorker::onlineWrite" << endl;
  struct timeval time1, time2, time3, time4;
  
  // 0. send request to coordinator that I want to write a file with online erasure coding
  //    wait for responses from coordinator with a set of tasks
  gettimeofday(&time1, NULL);
  CoorCommand* coorCmd = new CoorCommand();
  coorCmd->buildType0(0, _conf->_localIp, filename, ecid, 0, filesizeMB);
  coorCmd->sendTo(_coorCtx);
  delete coorCmd;

  // 1. wait for coordinator's instructions
  redisReply* rReply;
  redisContext* waitCtx = RedisUtil::createContext(_conf->_localIp);
  string wkey = "registerFile:" + filename;
  rReply = (redisReply*)redisCommand(waitCtx, "blpop %s 0", wkey.c_str());
  char* reqStr = rReply -> element[1] -> str;
  AGCommand* agCmd = new AGCommand(reqStr);
  freeReplyObject(rReply);
 
  int ecn = agCmd->getN();
  int eck = agCmd->getK();
  int ecw = agCmd->getW();
  int computen = agCmd->getComputen();
  delete agCmd;

  int totalNumPkt = 1048576/_conf->_pktSize * filesizeMB;
  int totalNumRounds = totalNumPkt / eck;
  int lastNum = totalNumPkt % eck;
  bool zeropadding = false;
  if (lastNum > 0) zeropadding = true;

  vector<ECTask*> computeTasks;
  for (int i=0; i<computen; i++) {
    string wkey = "compute:" + filename+":"+to_string(i);
    rReply = (redisReply*)redisCommand(waitCtx, "blpop %s 0", wkey.c_str());
    char* reqStr = rReply -> element[1] -> str;
    ECTask* compute = new ECTask(reqStr);
    compute->dump();
    freeReplyObject(rReply);
    computeTasks.push_back(compute);
  }
  redisFree(waitCtx);
  gettimeofday(&time2, NULL);
  cout << "OECWorker::onlineWrite.registerFile.duraiton = " << RedisUtil::duration(time1, time2) << endl;

  // 2. create threads for Load tasks to load data from local redis
  BlockingQueue<OECDataPacket*>** loadQueue = (BlockingQueue<OECDataPacket*>**)calloc(eck, sizeof(BlockingQueue<OECDataPacket*>*));
  for (int i=0; i<eck; i++) {
    loadQueue[i] = new BlockingQueue<OECDataPacket*>();
  }
  vector<thread> loadThreads = vector<thread>(eck);
  for (int i=0; i<eck; i++) {
    int curnum = totalNumRounds;
    bool curzero = false;
    if (lastNum > 0 && i < lastNum) curnum = curnum + 1;
    if (lastNum > 0 && i >= lastNum) curzero = true;
    loadThreads[i] = thread([=]{loadWorker(loadQueue[i], filename, i, eck, curnum, curzero);});
  }

  // 3. create threads for Persist tasks to persist data to DSS
  FSObjOutputStream** objstreams = (FSObjOutputStream**)calloc(ecn, sizeof(FSObjOutputStream*));
  vector<thread> createThreads = vector<thread>(ecn);
  for (int i=0; i<ecn; i++) {
    // figure out number of pkts to persist for this stream
    int curnum = totalNumRounds;
    if (lastNum > 0 && i < lastNum) curnum = curnum + 1;
    if (lastNum > 0 && i >= eck) curnum = curnum + 1;
    string objname = filename+"_oecobj_"+to_string(i);
    createThreads[i] = thread([=]{objstreams[i] = new FSObjOutputStream(_conf, objname, _underfs, curnum);});
//    objstreams[i] = new FSObjOutputStream(_conf, objname, _underfs, curnum);
  }
  // join create thread
  for (int i=0; i<ecn; i++) createThreads[i].join();

  vector<thread> persistThreads = vector<thread>(ecn);
  for (int i=0; i<ecn; i++) {  
    persistThreads[i] = thread([=]{objstreams[i]->writeObj();});
  }

  // 4. create thread to do calculation in iterations
  int stripenum=totalNumRounds;
  if (lastNum>0) stripenum = totalNumRounds+1;
  thread computeThread([=]{computeWorker(computeTasks, loadQueue, objstreams, stripenum, ecn, eck, ecw);});
   
  // join
  for (int i=0; i<eck; i++) loadThreads[i].join();
  computeThread.join();
  for (int i=0; i<ecn; i++) persistThreads[i].join();

  // check the finish flag in streams and then return finish flag for file
  bool finish = true;
  for (int i=0; i<ecn; i++) {
    if (!objstreams[i]->getFinish()) {
      finish = false;
      break;
    }
  }

  // tell client that write finishes
  if (finish) {
    // writefinish:filename
    redisReply* rReply;
    redisContext* waitCtx = RedisUtil::createContext(_conf->_localIp);
    string wkey = "writefinish:" + filename;
    int tmpval = htonl(1);
    rReply = (redisReply*)redisCommand(waitCtx, "rpush %s %b", wkey.c_str(), (char*)&tmpval, sizeof(tmpval));
    freeReplyObject(rReply);
    redisFree(waitCtx);
  } 
  gettimeofday(&time3, NULL);
  cout << "OECWorker::onlineWrite.duration: " << RedisUtil::duration(time1, time3) << endl;

  // free
  for (int i=0; i<eck; i++) delete loadQueue[i];
  free(loadQueue);
  for (int i=0; i<ecn; i++) delete objstreams[i];
  free(objstreams);
  for (auto compute: computeTasks) delete compute;
}

void OECWorker::offlineWrite(string filename, string ecpoolid, int filesizeMB) {
  struct timeval time1, time2, time3, time4;
  
  // 0. send request to coordinator that I want to write a file with offline erasure coding
  //    wait for responses from coordinator with a set of tasks
  gettimeofday(&time1, NULL);
  CoorCommand* coorCmd = new CoorCommand();
  coorCmd->buildType0(0, _conf->_localIp, filename, ecpoolid, 1, filesizeMB);
  coorCmd->sendTo(_coorCtx);
  delete coorCmd;

  // 1. wait for coordinator's instructions
  redisReply* rReply;
  redisContext* waitCtx = RedisUtil::createContext(_conf->_localIp);
  string wkey = "registerFile:" + filename;
  rReply = (redisReply*)redisCommand(waitCtx, "blpop %s 0", wkey.c_str());
  char* reqStr = rReply -> element[1] -> str;
  AGCommand* agCmd = new AGCommand(reqStr);
  freeReplyObject(rReply);
  redisFree(waitCtx);
  
  int objnum = agCmd->getObjnum();
  int basesizeMB = agCmd->getBasesizeMB();
  delete agCmd;
  cout << "offlineWrite::objnum = " << objnum << ", basesizeMB = " << basesizeMB << endl;

  vector<int> pktnums;
  for (int i=0; i<objnum; i++) {
    int sizeMB;
    if (i != (objnum-1)) {
      sizeMB = basesizeMB;
    } else {
      sizeMB = filesizeMB - basesizeMB * (objnum-1);
    }
    int pktnum = 1048576/_conf->_pktSize * sizeMB;
    pktnums.push_back(pktnum);
  }
  // 2. create outputstream for each obj
  FSObjOutputStream** objstreams = (FSObjOutputStream**)calloc(objnum, sizeof(FSObjOutputStream*));
  for (int i=0; i<objnum; i++) {
    // figure out number of pkts to persist for this stream
    int curnum = pktnums[i];
    string objname = filename+"_oecobj_"+to_string(i);
    objstreams[i] = new FSObjOutputStream(_conf, objname, _underfs, curnum);
  }
  BlockingQueue<OECDataPacket*>** loadQueue = (BlockingQueue<OECDataPacket*>**)calloc(objnum, sizeof(BlockingQueue<OECDataPacket*>*));
  for (int i=0; i<objnum; i++) {
    loadQueue[i] = objstreams[i]->getQueue();
  }
  // 3. create loadThreads
  vector<thread> loadThreads = vector<thread>(objnum);
  int startid = 0;
  for (int i=0; i<objnum; i++) {
    int curnum = pktnums[i];
    loadThreads[i] = thread([=]{loadWorker(loadQueue[i], filename, startid, 1, curnum, false);});
    startid += curnum;
  }
  // 4. create persistThreads
  vector<thread> persistThreads = vector<thread>(objnum);
  for (int i=0; i<objnum; i++) {  
    persistThreads[i] = thread([=]{objstreams[i]->writeObj();});
  }

  // join
  for (int i=0; i<objnum; i++) loadThreads[i].join();
  for (int i=0; i<objnum; i++) persistThreads[i].join();

  // check the finish flag in streams and then return finish flag for file
  bool finish = true;
  for (int i=0; i<objnum; i++) {
    if (!objstreams[i]->getFinish()) {
      finish = false;
      break;
    }
  }

  // tell client that write finishes
  if (finish) {
    // writefinish:filename
    redisReply* rReply;
    redisContext* waitCtx = RedisUtil::createContext(_conf->_localIp);
    string wkey = "writefinish:" + filename;
    cout << "write " << wkey << " into redis" << endl;
    int tmpval = htonl(1);
    rReply = (redisReply*)redisCommand(waitCtx, "rpush %s %b", wkey.c_str(), (char*)&tmpval, sizeof(tmpval));
    freeReplyObject(rReply);
    redisFree(waitCtx);
  } 
   
  // free
  for (int i=0; i<objnum; i++) delete objstreams[i];
  free(objstreams);
  free(loadQueue);

  // finalize writing offline-encoded file
  CoorCommand* coorCmd1 = new CoorCommand();
  coorCmd1->buildType2(2, _conf->_localIp, filename); 
  coorCmd1->sendTo(_coorCtx);
  delete coorCmd1;
}

void OECWorker::loadWorker(BlockingQueue<OECDataPacket*>* readQueue,
                    string keybase,
                    int startid,
                    int step,
                    int round,
                    bool zeropadding) {
  cout << "OECWorker::loadWorker. keybase = " << keybase;
  cout << ", startid = " << startid;
  cout << ", step = " << step;
  cout << ", round = " << round;
  cout << ", zeropadding = " << zeropadding << endl;
  struct timeval time1, time2, time3;
  gettimeofday(&time1, NULL);
  // read from redis
  redisContext* readCtx = RedisUtil::createContext(_conf->_localIp);
  int startidx = startid;
  for (int i=0; i<round; i++) {
    int curidx = startidx + i * step;
    string key = keybase + ":" + to_string(curidx);
    redisAppendCommand(readCtx, "blpop %s 1", key.c_str());
  }
  redisReply* rReply;
  for (int i=0; i<round; i++) {
    redisGetReply(readCtx, (void**)&rReply);
    char* content = rReply->element[1]->str;
    OECDataPacket* pkt = new OECDataPacket(content);
    int curDataLen = pkt->getDatalen();
    readQueue->push(pkt);
    freeReplyObject(rReply);
  }
  if (zeropadding) {
    // create a packet that contains all zero
    char* content = (char*)calloc(_conf->_pktSize + 4, sizeof(char));
    memset(content, 0, _conf->_pktSize + 4);
    int tmplen = htonl(_conf->_pktSize);
    memcpy(content, (char*)&tmplen, 4);
    OECDataPacket* pkt = new OECDataPacket(content);
    readQueue->push(pkt);
    free(content);
  }
  redisFree(readCtx);
  gettimeofday(&time2, NULL);
  cout << "OECWorker::loadWorker.from client.duration = " << RedisUtil::duration(time1, time2) << endl;
}

void OECWorker::computeWorkerDegradedOffline(FSObjInputStream** readStreams,
                                      vector<int> idlist,
                                      BlockingQueue<OECDataPacket*>* writeQueue,
                                      int lostidx,
                                      vector<ECTask*> computeTasks,
                                      int stripenum,
                                      int ecn,
                                      int eck,
                                      int ecw) {
  // In this method, we read available data from readStreams, whose stripeidx is in idlist
  // Then we perform compute task one by one in computeTakss for each stripe
  // Finally, we put pkt for lostidx in writeQueue
  OECDataPacket** curStripe = (OECDataPacket**)calloc(ecn, sizeof(OECDataPacket*));
  for (int i=0; i<ecn; i++) curStripe[i] = NULL; 
  int splitsize = _conf->_pktSize / ecw;
  for (int stripeid = 0; stripeid < stripenum; stripeid++) {
    unordered_map<int, char*> bufMap;
    // read from readStreams
    for (int i=0; i<idlist.size(); i++) {
      int sid = idlist[i];
      OECDataPacket* curpkt = readStreams[i]->dequeue();
      curStripe[sid] = curpkt;
      char* pktbuf = curpkt->getData();
      for (int j=0; j<ecw; j++) {
        int ecidx = sid * ecw + j;
        char* bufaddr = pktbuf + j*splitsize;
        bufMap.insert(make_pair(ecidx, bufaddr));
      }
    }
    // prepare for lostidx
    OECDataPacket* curpkt = new OECDataPacket(_conf->_pktSize);
    curStripe[lostidx] = curpkt;
    char* pktbuf = curpkt->getData();
    for (int j=0; j<ecw; j++) {
      int ecidx = lostidx*ecw+j;
      char* bufaddr = pktbuf + j*splitsize;
      bufMap.insert(make_pair(ecidx, bufaddr));
    }

    // now perform computation in computeTasks one by one
    for (int taskid=0; taskid<computeTasks.size(); taskid++) {
      ECTask* compute = computeTasks[taskid];
      vector<int> children = compute->getChildren();
      unordered_map<int, vector<int>> coefMap = compute->getCoefMap();
      int col = children.size();
      int row = coefMap.size();
      vector<int> targets;
      // here xiaolu modify > to >=
      if (col*row >= 1) {
        int* matrix = (int*)calloc(row*col, sizeof(int));
        char** data = (char**)calloc(col, sizeof(char*));
        char** code = (char**)calloc(row, sizeof(char*));

        // prepare the data buf
        // actually, data buf should always exist
        for (int bufIdx = 0; bufIdx < children.size(); bufIdx++) {
          int child = children[bufIdx];
          // check whether there is buf in databuf
          assert (bufMap.find(child) != bufMap.end());
          data[bufIdx] = bufMap[child];
        }
        // prepare the code buf
        int codeBufIdx = 0;
        for (auto it: coefMap) {
          int target = it.first;
          char* codebuf;
          if (bufMap.find(target) == bufMap.end()) {
            codebuf = (char*)calloc(splitsize, sizeof(char));
            bufMap.insert(make_pair(target, codebuf)); 
          } else {
            codebuf = bufMap[target];
          }
          code[codeBufIdx] = codebuf;
          targets.push_back(target);
          vector<int> curcoef = it.second;
          for (int j=0; j<col; j++) {
            matrix[codeBufIdx * col + j] = curcoef[j];
          }
          codeBufIdx++;
        }
        // perform compute operation
        Computation::Multi(code, data, matrix, row, col, splitsize, "Isal");
      }
      // check whether there is a need to discuss about row*col = 1
    }
    // now computation is finished, we take out pkt from stripe and put into outputstream
    for (int i=0; i<ecn; i++) {
      if (i == lostidx) {
        writeQueue->push(curStripe[lostidx]);
        curStripe[lostidx] = NULL;
      } else if (curStripe[i] != NULL) {
        delete curStripe[i];
        curStripe[i] = NULL;
      }
    }
    // clear data in bufMap
    unordered_map<int, char*>::iterator it = bufMap.begin();
    while (it != bufMap.end()) {
      int sidx = it->first/ecw;
      if (sidx < ecn) {
        curStripe[sidx] = NULL;
      } else {
        if (it->second) free(it->second);
      }
      it++;
    }
    bufMap.clear();
  }
 
  // delete
  for (int i=0; i<ecn; i++) {
    if (curStripe[i] != NULL) delete curStripe[i];
  }
  if (curStripe) free(curStripe);
}

void OECWorker::computeWorker(FSObjInputStream** readStreams,
                              vector<int> idlist,
                              BlockingQueue<OECDataPacket*>* writeQueue,
                              vector<ECTask*> computeTasks,
                              int stripenum,
                              int ecn,
                              int eck,
                              int ecw) {
  // In this method, we read available data from readStreams, whose stripeidx is in idlist
  // Then we perform compute task one by on in computeTasks for each stripe.
  // Finally, we put original eck data pkts in writeQueue
  OECDataPacket** curStripe = (OECDataPacket**)calloc(ecn, sizeof(OECDataPacket*));
  for (int i=0; i<ecn; i++) curStripe[i] = NULL; 
  int splitsize = _conf->_pktSize / ecw;
  for (int stripeid = 0; stripeid < stripenum; stripeid++) {
    unordered_map<int, char*> bufMap;
    // read from readStreams
    for (int i=0; i<idlist.size(); i++) {
      int sid = idlist[i];
      OECDataPacket* curpkt = readStreams[i]->dequeue();
      curStripe[sid] = curpkt;
      char* pktbuf = curpkt->getData();
      for (int j=0; j<ecw; j++) {
        int ecidx = sid * ecw + j;
        char* bufaddr = pktbuf + j*splitsize;
        bufMap.insert(make_pair(ecidx, bufaddr));
      }
    }
    // prepare for lost
    for (int i=0; i<eck; i++) {
      if (curStripe[i] == NULL) {
        OECDataPacket* curpkt = new OECDataPacket(_conf->_pktSize);
        curStripe[i] = curpkt;
        char* pktbuf = curpkt->getData();
        for (int j=0; j<ecw; j++) {
          int ecidx = i * ecw + j;
          char* bufaddr = pktbuf + j*splitsize;
          bufMap.insert(make_pair(ecidx, bufaddr));
        }
      }
    }

    // now perform computation in computeTasks one by one
    for (int taskid=0; taskid<computeTasks.size(); taskid++) {
      ECTask* compute = computeTasks[taskid];
      vector<int> children = compute->getChildren();
      unordered_map<int, vector<int>> coefMap = compute->getCoefMap();
      int col = children.size();
      int row = coefMap.size();
      vector<int> targets;
      // here xiaolu modify > to >=
      if (col*row >= 1) {
        int* matrix = (int*)calloc(row*col, sizeof(int));
        char** data = (char**)calloc(col, sizeof(char*));
        char** code = (char**)calloc(row, sizeof(char*));

        // prepare the data buf
        // actually, data buf should always exist
        for (int bufIdx = 0; bufIdx < children.size(); bufIdx++) {
          int child = children[bufIdx];
          // check whether there is buf in databuf
          assert (bufMap.find(child) != bufMap.end());
          data[bufIdx] = bufMap[child];
        }
        // prepare the code buf
        int codeBufIdx = 0;
        for (auto it: coefMap) {
          int target = it.first;
          char* codebuf;
          if (bufMap.find(target) == bufMap.end()) {
            codebuf = (char*)calloc(splitsize, sizeof(char));
            bufMap.insert(make_pair(target, codebuf)); 
          } else {
            codebuf = bufMap[target];
          }
          code[codeBufIdx] = codebuf;
          targets.push_back(target);
          vector<int> curcoef = it.second;
          for (int j=0; j<col; j++) {
            matrix[codeBufIdx * col + j] = curcoef[j];
          }
          codeBufIdx++;
        }
        // perform compute operation
        Computation::Multi(code, data, matrix, row, col, splitsize, "Isal");
      }
      // check whether there is a need to discuss about row*col = 1
    }
    // now computation is finished, we take out pkt from stripe and put into outputstream
    for (int pktidx=0; pktidx<eck; pktidx++) {
      writeQueue->push(curStripe[pktidx]);
      curStripe[pktidx] = NULL;
    }
    // clear data in bufMap
    unordered_map<int, char*>::iterator it = bufMap.begin();
    while (it != bufMap.end()) {
      int sidx = it->first/ecw;
      if (sidx < ecn) {
        curStripe[sidx] = NULL;
      } else {
        if (it->second) free(it->second);
      }
      it++;
    }
    bufMap.clear();
  }
 
  // delete
  for (int i=0; i<ecn; i++) {
    if (curStripe[i] != NULL) delete curStripe[i];
  }
  if (curStripe) free(curStripe);
}

void OECWorker::computeWorker(vector<ECTask*> computeTasks,
                       BlockingQueue<OECDataPacket*>** readQueue,
                       FSObjOutputStream** objstreams,
                       int stripenum,
                       int ecn,
                       int eck,
                       int ecw) {
  struct timeval time1, time2, time3;
  gettimeofday(&time1, NULL);
  cout << "OECWorker::computeWorker.stripenum: " << stripenum;
  // In this method, we fetch eck pkts from readQueue and cache into runtime memory
  // If there is need, we need to divide pkt into splits for sub-packetization
  // Then we perform calculations inside ECTask list one by one, results are cached in runtime memory
  // Finally we put data from ecn into output streams

  OECDataPacket** curStripe = (OECDataPacket**)calloc(ecn, sizeof(OECDataPacket*));
  int pktsize = _conf->_pktSize;
  int splitsize = pktsize / ecw;
  cout << ", pktsize: " << pktsize
       << ", splitsize: " << splitsize
       << ", ecn: " << ecn
       << ", eck: " << eck
       << ", ecw: " << ecw << endl;

  for (int stripeid=0; stripeid<stripenum; stripeid++) {
    for (int pktidx=0; pktidx < eck; pktidx++) {
      OECDataPacket* curPkt = readQueue[pktidx]->pop();
      curStripe[pktidx] = curPkt;
    }
    // now we have k pkt in a stripe, prepare pkt for parity pkt 
    for (int i=0; i<(ecn-eck); i++) {
      OECDataPacket* paritypkt = new OECDataPacket(pktsize);
      curStripe[eck+i] = paritypkt;
    }

    unordered_map<int, char*> bufMap;
    // add pkt into bufMap
    for (int i=0; i<ecn; i++) {
      char* pktbuf = curStripe[i]->getData();
      for (int j=0; j<ecw; j++) {
        int ecidx = i*ecw + j;
        char* bufaddr = pktbuf+j*splitsize;
        bufMap.insert(make_pair(ecidx, bufaddr));
      }
    }
    // now perform computation in compute task one by one
    for (int taskid=0; taskid<computeTasks.size(); taskid++) {
      ECTask* compute = computeTasks[taskid];
      vector<int> children = compute->getChildren();

      if (stripeid == 0) {
        cout << "children: ";
        for (int childid=0; childid<children.size(); childid++) {
          cout << children[childid] << " ";
        }
        cout << endl;
      }

      unordered_map<int, vector<int>> coefMap = compute->getCoefMap();

      if (stripeid == 0) {
        cout << "coef: "<< endl;
        for (auto item: coefMap) {
          int target = item.first;
          vector<int> coefs = item.second;
          cout << "    " << target << ": " << "( ";
          for (int coefidx=0; coefidx<coefs.size(); coefidx++) cout << coefs[coefidx] << " ";
          cout << ")" << endl;
        }
      }

      int col = children.size();
      int row = coefMap.size();
      vector<int> targets;
      // here xiaolu modify > to >=
      if (col*row >= 1) {
        int* matrix = (int*)calloc(row*col, sizeof(int));
        char** data = (char**)calloc(col, sizeof(char*));
        char** code = (char**)calloc(row, sizeof(char*));

        // prepare the data buf
        // actually, data buf should always exist
        for (int bufIdx = 0; bufIdx < children.size(); bufIdx++) {
          int child = children[bufIdx];
          // check whether there is buf in databuf
          assert (bufMap.find(child) != bufMap.end());
          data[bufIdx] = bufMap[child];
          if (stripeid == 0) {
            cout << "data["<<bufIdx<<"] = bufMap[" <<child<<"]"<< endl;
          }
        }
        // prepare the code buf
        int codeBufIdx = 0;
        for (auto it: coefMap) {
          int target = it.first;
          char* codebuf;
          if (bufMap.find(target) == bufMap.end()) {
            codebuf = (char*)calloc(splitsize, sizeof(char));
            bufMap.insert(make_pair(target, codebuf)); 
            if (stripeid == 0) cout << "code["<<codeBufIdx<<"] = new" << endl;
          } else {
            codebuf = bufMap[target];
            if (stripeid == 0) cout << "code["<<codeBufIdx<<"] = bufMap[" << target << "]" << endl;
          }
          code[codeBufIdx] = codebuf;

          targets.push_back(target);
          vector<int> curcoef = it.second;
          for (int j=0; j<col; j++) {
            matrix[codeBufIdx * col + j] = curcoef[j];
          }
          codeBufIdx++;
        }
        if (stripeid == 0) {
          cout << "matrix: " << endl;
          for (int ii=0; ii<row; ii++) {
            for (int jj=0; jj<col; jj++) {
              cout << matrix[ii*col+jj] << " ";
            }
            cout << endl;
          }
        }
        // perform compute operation
        Computation::Multi(code, data, matrix, row, col, splitsize, "Isal");
      }
      // check whether there is a need to discuss about row*col = 1
    }
    // now computation is finished, we take out pkt from stripe and put into outputstream
    for (int pktidx=0; pktidx<ecn; pktidx++) {
      objstreams[pktidx]->enqueue(curStripe[pktidx]);
      curStripe[pktidx] = NULL;
    }
    // clear data in bufMap
    unordered_map<int, char*>::iterator it = bufMap.begin();
    while (it != bufMap.end()) {
      int sidx = it->first/ecw;
      if (sidx < ecn) {
        curStripe[sidx] = NULL;
      } else {
        if (it->second) free(it->second);
      }
      it++;
    }
    bufMap.clear();
  }

  // free
  free(curStripe);
  gettimeofday(&time2, NULL);
  cout << "OECWorker::computeWorker.duration = " << RedisUtil::duration(time1, time2) << endl;
}

void OECWorker::readDisk(AGCommand* agcmd) {
  string stripename = agcmd->getStripeName();
  int w = agcmd->getW();
  int num = agcmd->getNum();
  string objname = agcmd->getReadObjName();
  vector<int> cidlist = agcmd->getReadCidList();
  sort(cidlist.begin(), cidlist.end());
  unordered_map<int, int> refs = agcmd->getCacheRefs();

  int pktsize = _conf->_pktSize;
  int slicesize = pktsize/w;
 
  int numThreads = cidlist.size();

  FSObjInputStream* objstream = new FSObjInputStream(_conf, objname, _underfs);
  if (!objstream->exist()) {
    cout << "OECWorker::readWorker." << objname << " does not exist!" << endl;
    return;
  }

  // read data in serial from disk
  thread readThread = thread([=]{objstream->readObj(slicesize);});
  BlockingQueue<OECDataPacket*>* readQueue = objstream->getQueue();
  // cacheThread
  thread cacheThread = thread([=]{selectCacheWorker(readQueue, num, stripename, w, cidlist, refs);});

  //join
  readThread.join();
  cacheThread.join();

  // delete
  if (objstream) delete objstream;
  cout << "OECWorker::readDisk finishes!" << endl;
}

void OECWorker::selectCacheWorker(BlockingQueue<OECDataPacket*>* cacheQueue,
                                  int pktnum,
                                  string keybase,
                                  int w,
                                  vector<int> idxlist,
                                  unordered_map<int, int> refs) {
  redisContext* writeCtx = RedisUtil::createContext(_conf->_localIp);
  redisReply* rReply;
  
  vector<int> units;
  unordered_map<int, int> unit2idx;
  for (int i=0; i<idxlist.size(); i++) {
    int curunit = idxlist[i] % w;
    units.push_back(curunit);  
    unit2idx.insert(make_pair(curunit, idxlist[i]));
  }

  struct timeval time1, time2;
  gettimeofday(&time1, NULL);

  int count=0;
  int replyid=0;
 
  for (int i=0; i<pktnum; i++) {
    for (int j=0; j<w; j++) {
      OECDataPacket* curslice = cacheQueue->pop();
      if (find(units.begin(), units.end(), j) == units.end()) {
        delete curslice;
        continue;
      }
      int curidx = unit2idx[j];
      string key = keybase+":"+to_string(curidx)+":"+to_string(i);
      // we write data into redis
      int refnum = refs[curidx];
      //cout << "curidx = " << curidx << ", refnum = " << refnum << endl;
      int len = curslice->getDatalen();
      //cout << "len = "  << len << ", key = " << key << endl;
      char* raw = curslice->getRaw();
      int rawlen = len + 4;
      for (int k=0; k<refnum; k++) {
        redisAppendCommand(writeCtx, "RPUSH %s %b", key.c_str(), raw, rawlen); count++;
      }
      delete curslice;
      if (i>1) {
        redisGetReply(writeCtx, (void**)&rReply); replyid++;
        freeReplyObject(rReply);
      }
    }
  }
  for (int i=replyid; i<count; i++)  {
    redisGetReply(writeCtx, (void**)&rReply); replyid++;
    freeReplyObject(rReply);
  }

  gettimeofday(&time2, NULL);
  cout << "OECWorker::selectCacheWorker.duration: " << RedisUtil::duration(time1, time2) << " for " << keybase << endl;
  redisFree(writeCtx);
}

void OECWorker::fetchCompute(AGCommand* agcmd) {
  string stripename = agcmd->getStripeName();
  int w = agcmd->getW();
  int num = agcmd->getNum();
  int nprevs = agcmd->getNprevs();  
  vector<int> prevcids = agcmd->getPrevCids();
  vector<unsigned int> prevlocs = agcmd->getPrevLocs();
  unordered_map<int, vector<int>> coefs = agcmd->getCoefs();
  unordered_map<int, int> refs = agcmd->getCacheRefs();

  vector<int> computefor;
  for (auto item:coefs) {
    computefor.push_back(item.first);
  }

  // create fetch queue
  BlockingQueue<OECDataPacket*>** fetchQueue = (BlockingQueue<OECDataPacket*>**)calloc(nprevs, sizeof(BlockingQueue<OECDataPacket*>*));
  for (int i=0; i<nprevs; i++) {
    fetchQueue[i] = new BlockingQueue<OECDataPacket*>();
  }

  // create write queue
  BlockingQueue<OECDataPacket*>** writeQueue = (BlockingQueue<OECDataPacket*>**)calloc(coefs.size(), sizeof(BlockingQueue<OECDataPacket*>*));
  for (int i=0; i<coefs.size(); i++) {
    writeQueue[i] = new BlockingQueue<OECDataPacket*>();
  }

  // create fetch thread
  vector<thread> fetchThreads = vector<thread>(nprevs);
  for (int i=0; i<nprevs; i++) {
    string keybase = stripename+":"+to_string(prevcids[i]);
    fetchThreads[i] = thread([=]{fetchWorker(fetchQueue[i], keybase, prevlocs[i], num);});
  }

  // create compute thread
  thread computeThread = thread([=]{computeWorker(fetchQueue, nprevs, num, coefs, computefor, writeQueue, _conf->_pktSize/w);});

  // create cache thread
  vector<thread> cacheThreads = vector<thread>(computefor.size());
  for (int i=0; i<computefor.size(); i++) {
    string keybase = stripename+":"+to_string(computefor[i]);
    int r = refs[computefor[i]];
    cacheThreads[i] = thread([=]{cacheWorker(writeQueue[i], keybase, num, r);});
  }

  // join
  for (int i=0; i<nprevs; i++) {
    fetchThreads[i].join();
  }
  computeThread.join();
  for (int i=0; i<computefor.size(); i++) {
    cacheThreads[i].join();
  }

  // delete
  for (int i=0; i<nprevs; i++) {
    delete fetchQueue[i];
  }
  free(fetchQueue);
  for (int i=0; i<computefor.size(); i++) {
    delete writeQueue[i];
  }
  free(writeQueue);
  cout << "OECWorker::fetchCompute finishes!" << endl;
}

void OECWorker::fetchWorker(BlockingQueue<OECDataPacket*>* fetchQueue,
                     string keybase,
                     unsigned int loc,
                     int num) {
  redisReply* rReply;
  redisContext* fetchCtx = RedisUtil::createContext(loc);

  struct timeval time1, time2;
  gettimeofday(&time1, NULL);

  int replyid=0;
  for (int i=0; i<num; i++) {
    string key = keybase+":"+to_string(i);
    redisAppendCommand(fetchCtx, "blpop %s 0", key.c_str());
  }

  struct timeval t1, t2;
  double t;
  for (int i=replyid; i<num; i++) {
    string key = keybase+":"+to_string(i);
    gettimeofday(&t1, NULL);
    redisGetReply(fetchCtx, (void**)&rReply);
    gettimeofday(&t2, NULL);
    //if (i == 0) cout << "OECWorker::fetchWorker.fetch first t = " << RedisUtil::duration(t1, t2) << endl;
    char* content = rReply->element[1]->str;
    OECDataPacket* pkt = new OECDataPacket(content);
    int curDataLen = pkt->getDatalen();
    fetchQueue->push(pkt);
    freeReplyObject(rReply);
  }
  gettimeofday(&time2, NULL);
  cout << "OECWorker::fetchWorker.duration: " << RedisUtil::duration(time1, time2) << " for " << keybase << endl;
  redisFree(fetchCtx);
}

void OECWorker::computeWorker(BlockingQueue<OECDataPacket*>** fetchQueue,
                       int nprev,
                       int num,
                       unordered_map<int, vector<int>> coefs,
                       vector<int> cfor,
                       BlockingQueue<OECDataPacket*>** writeQueue,
                       int slicesize) {
  // prepare coding matrix
  int row = cfor.size();
  int col = nprev;
  int* matrix = (int*)calloc(row*col, sizeof(int));
  for (int i=0; i<row; i++) {
    int cid = cfor[i];
    vector<int> coef = coefs[cid];
    for (int j=0; j<col; j++) {
      matrix[i*col+j] = coef[j];
    }
  }
  cout << "OECWorker::computeWorker.num: " << num << ", row: " << row << ", col: " << col << endl;
  cout << "-------------------"<< endl;
  for (int i=0; i<row; i++) {
    for (int j=0; j<col; j++) {
      cout << matrix[i*col+j] << " ";
    }
    cout << endl;
  }
  cout << "-------------------"<< endl;

  OECDataPacket** curstripe = (OECDataPacket**)calloc(row+col, sizeof(OECDataPacket*));
  char** data = (char**)calloc(col, sizeof(char*));
  char** code = (char**)calloc(row, sizeof(char*));
  while(num--) {
    // prepare data
    for (int i=0; i<col; i++) {
      OECDataPacket* curpkt = fetchQueue[i]->pop();
      curstripe[i] = curpkt;
      data[i] = curpkt->getData();
    }
    for (int i=0; i<row; i++) {
      curstripe[col+i] = new OECDataPacket(slicesize);
      code[i] = curstripe[col+i]->getData();
    }
    // compute
    Computation::Multi(code, data, matrix, row, col, slicesize, "Isal");

    // now we free data
    for (int i=0; i<col; i++) {
      delete curstripe[i];
      curstripe[i] = nullptr;
    }
    // add the res to writeQueue
    for (int i=0; i<row; i++) {
      writeQueue[i]->push(curstripe[col+i]);
      curstripe[col+i] = nullptr;
    }
  }

  // free
  free(code);
  free(data);
  free(curstripe);
  free(matrix);
}

void OECWorker::cacheWorker(BlockingQueue<OECDataPacket*>* writeQueue,
                            string keybase,
                            int startidx,
                            int num,
                            int ref) {
  redisReply* rReply;
  redisContext* writeCtx = RedisUtil::createContext("127.0.0.1");
  
  struct timeval time1, time2;
  gettimeofday(&time1, NULL);

  int replyid=0;
  int count=0;
  for (int i=0; i<num; i++) {
    string key = keybase+":"+to_string(startidx+i);
    OECDataPacket* curpkt = writeQueue->pop();
    char* raw = curpkt->getRaw();
    int rawlen = curpkt->getDatalen() + 4;
    for (int k=0; k<ref; k++) {
      redisAppendCommand(writeCtx, "RPUSH %s %b", key.c_str(), raw, rawlen); count++;
    }
    delete curpkt;
    if (i>1) {
      redisGetReply(writeCtx, (void**)&rReply);
      freeReplyObject(rReply);
      replyid++;
    }
  }
  for (int i=replyid; i<count; i++) {
    redisGetReply(writeCtx, (void**)&rReply);
    freeReplyObject(rReply);
  }
  // xiaolu start 20180822 end

  gettimeofday(&time2, NULL);
  cout << "OECWorker::writeWorker.duration: " << RedisUtil::duration(time1, time2) << " for " << keybase << endl;
  redisFree(writeCtx);
}

void OECWorker::cacheWorker(BlockingQueue<OECDataPacket*>* writeQueue,
                            string keybase,
                            int num,
                            int ref) {
  struct timeval time1, time2, time3, time4;
  gettimeofday(&time1, NULL);

  redisReply* rReply;
  redisContext* writeCtx = RedisUtil::createContext("127.0.0.1");
  
  gettimeofday(&time2, NULL);
  cout << "OECWorker::cacheWorker.createCtx: " << RedisUtil::duration(time1, time2) << endl;

  int replyid=0;
  int count=0;
  for (int i=0; i<num; i++) {
    string key = keybase+":"+to_string(i);
    OECDataPacket* curpkt = writeQueue->pop();
    char* raw = curpkt->getRaw();
    int rawlen = curpkt->getDatalen() + 4;
    for (int k=0; k<ref; k++) {
      redisAppendCommand(writeCtx, "RPUSH %s %b", key.c_str(), raw, rawlen); count++;
    }
    delete curpkt;
    if (i>0) {
      redisGetReply(writeCtx, (void**)&rReply);
      freeReplyObject(rReply);
      replyid++;
    }
  }
  gettimeofday(&time3, NULL);
  cout<< "OECWorker::cacheWorker.write all data: " << RedisUtil::duration(time2, time3) << endl;
  for (int i=replyid; i<count; i++) {
    redisGetReply(writeCtx, (void**)&rReply);
    freeReplyObject(rReply);
  }

  gettimeofday(&time4, NULL);
  cout << "OECWorker::writeWorker.duration: " << RedisUtil::duration(time1, time4) << " for " << keybase << endl;
  redisFree(writeCtx);
}

void OECWorker::cacheWorker(BlockingQueue<OECDataPacket*>* writeQueue,
                            string keybase,
                            int startidx,
                            int step,
                            int num,
                            int ref) {
  // This cache worker fetch pkt from writeQueue and write into local redis
  // For each pkt: key = keybase:(startidx + i*step)
  // write ref times
  struct timeval time1, time2, time3, time4;
  gettimeofday(&time1, NULL);
 
  redisReply* rReply;
  redisContext* writeCtx = RedisUtil::createContext("127.0.0.1");

  int replyid=0;
  int count=0;
  for (int i=0; i<num; i++) {
    string key = keybase+":"+to_string(startidx + i*step);
    OECDataPacket* curpkt = writeQueue->pop();
    char* raw = curpkt->getRaw();
    int rawlen = curpkt->getDatalen() + 4;
    for (int k=0; k<ref; k++) {
      redisAppendCommand(writeCtx, "RPUSH %s %b", key.c_str(), raw, rawlen); count++;
    }
    delete curpkt;
    if (i>0) {
      redisGetReply(writeCtx, (void**)&rReply);
      freeReplyObject(rReply);
      replyid++;
    }
  }
  
  for (int i=replyid; i<count; i++) {
    redisGetReply(writeCtx, (void**)&rReply);
    freeReplyObject(rReply);
  }

  gettimeofday(&time4, NULL);
  cout << "OECWorker::cacheWorker6.duration: " << RedisUtil::duration(time1, time4) << " for " << keybase << endl;
  redisFree(writeCtx);
}

void OECWorker::persist(AGCommand* agcmd) {
  string stripename = agcmd->getStripeName();
  int w = agcmd->getW();
  int num = agcmd->getNum();
  int nprevs = agcmd->getNprevs();
  vector<int> prevcids = agcmd->getPrevCids();
  vector<unsigned int> prevlocs = agcmd->getPrevLocs();
  string objname = agcmd->getWriteObjName();

  for (int i=0; i<nprevs; i++) {
    string keybase = stripename+":"+to_string(prevcids[i]);
    cout << "OECWorker::persist.fetch "<<keybase<<" from " << RedisUtil::ip2Str(prevlocs[i]) << endl;
  }
  cout << "OECWorker::persist.write as " << objname << " with " << num << " pkts"<< endl;

  // create fetch queue
  BlockingQueue<OECDataPacket*>** fetchQueue = (BlockingQueue<OECDataPacket*>**)calloc(nprevs, sizeof(BlockingQueue<OECDataPacket*>*));
  for (int i=0; i<nprevs; i++) {
    fetchQueue[i] = new BlockingQueue<OECDataPacket*>();
  }

  // create fetch thread
  vector<thread> fetchThreads = vector<thread>(nprevs);
  for (int i=0; i<nprevs; i++) {
    string keybase = stripename+":"+to_string(prevcids[i]);
    fetchThreads[i] = thread([=]{fetchWorker(fetchQueue[i], keybase, prevlocs[i], num);});
  }

  // create objstream and writeThread
  FSObjOutputStream* objstream = new FSObjOutputStream(_conf, objname, _underfs, num*nprevs);
  thread writeThread = thread([=]{objstream->writeObj();});

  int total = num;
  while(total--) {
//    cout << "OECWorker::persist.left = " << total << endl;
    for (int i=0; i<nprevs; i++) {
      OECDataPacket* curpkt = fetchQueue[i]->pop();
      objstream->enqueue(curpkt);
    }
  }

  // join 
  for (int i=0; i<nprevs; i++) {
    fetchThreads[i].join();
  }
  writeThread.join();

  // delete
  for (int i=0; i<nprevs; i++) {
    delete fetchQueue[i];
  }
  free(fetchQueue);
  if (objstream) delete objstream;

  // write a finish flag to local?
  // writefinish:objname
  redisReply* rReply;
  redisContext* writeCtx = RedisUtil::createContext(_conf->_localIp);

  string wkey = "writefinish:" + objname;
  int tmpval = htonl(1);
  rReply = (redisReply*)redisCommand(writeCtx, "rpush %s %b", wkey.c_str(), (char*)&tmpval, sizeof(tmpval));
  freeReplyObject(rReply);
  redisFree(writeCtx);
  cout << "OECWorker::persist finishes!" << endl;
}

void OECWorker::clientRead(AGCommand* agcmd) {
  cout << "OECWorker::clientRead" << endl;
  struct timeval time1, time2;
  gettimeofday(&time1, NULL);
  string filename = agcmd->getFilename();

  // 0. send request to coordinator to get filemeta
  CoorCommand* coorCmd = new CoorCommand();
  coorCmd->buildType3(3, _conf->_localIp, filename); 
  coorCmd->sendTo(_coorCtx);
  delete coorCmd;

  // 1. get response type|filesizeMB
  string metakey = "filemeta:"+filename;
  redisReply* metareply;
  redisContext* metaCtx = RedisUtil::createContext(_conf->_localIp);
  metareply = (redisReply*)redisCommand(metaCtx, "blpop %s 0", metakey.c_str());
  char* metastr = metareply->element[1]->str;
  // 1.1 redundancy type
  int redundancy;
  memcpy((char*)&redundancy, metastr, 4); metastr += 4;
  redundancy=ntohl(redundancy);
  // 1.2 filesizeMB
  int filesizeMB;
  memcpy((char*)&filesizeMB, metastr, 4); metastr += 4;
  filesizeMB = ntohl(filesizeMB);

  // 2. return filesizeMB to client
  redisReply* rReply;
  redisContext* cliCtx = RedisUtil::createContext(_conf->_localIp);
  string skey = "filesize:"+filename;
  int tmpval = htonl(filesizeMB);
  rReply = (redisReply*)redisCommand(cliCtx, "rpush %s %b", skey.c_str(), (char*)&tmpval, sizeof(tmpval));
  freeReplyObject(rReply);
  redisFree(cliCtx);
 
  gettimeofday(&time2, NULL);
  cout << "OECWorker::clientRead.get metadata duration = " << RedisUtil::duration(time1, time2) << endl;

  if (redundancy == 0) {
    // |ecn|eck|ecw|
    // 0.1 ecn
    int ecn;
    memcpy((char*)&ecn, metastr, 4); metastr += 4;
    ecn = ntohl(ecn);
    // 0.2 eck
    int eck;
    memcpy((char*)&eck, metastr, 4); metastr += 4;
    eck = ntohl(eck);
    // 0.3 ecw
    int ecw;
    memcpy((char*)&ecw, metastr, 4); metastr += 4;
    ecw = ntohl(ecw);

    readOnline(filename, filesizeMB, ecn, eck, ecw);
  } else {

  }

  freeReplyObject(metareply);
  redisFree(metaCtx);

//  if (redundancy == 0) readOnline(filename, filesizeMB);
//  else readOffline(filename, filesizeMB);
}

void OECWorker::readOffline(string filename, int filesizeMB) {
  cout << "OECWorker::readOffline.filename: " << filename << ", filesizeMB: " << filesizeMB << endl;
  string instkey = "offlineinst:"+filename;
  redisReply* instreply;
  redisContext* instCtx = RedisUtil::createContext(_conf->_localIp);
  instreply = (redisReply*)redisCommand(instCtx, "blpop %s 0", instkey.c_str());
  char* inststr = instreply->element[1]->str;
  // 0.1 objnum
  int objnum;
  memcpy((char*)&objnum, inststr, 4); inststr += 4;
  objnum = ntohl(objnum);
  vector<int> integrity;
  for (int i=0; i<objnum; i++) {
    int integ;
    memcpy((char*)&integ, inststr, 4); inststr += 4;
    integ = ntohl(integ);
    integrity.push_back(integ);
  }
  cout << "OECWorker::readOffline.integrity vector: ";
  for (int i=0; i<objnum; i++) cout << integrity[i] << " ";
  cout << endl;

  // read object one by one
  for (int obji=0; obji<objnum; obji++) {
    string objname = filename+"_oecobj_"+to_string(obji);
    int objsize = filesizeMB / objnum;
    if (integrity[obji] == 1) {
      // this obj is in good health
      // 1.0 create objinputstream
      FSObjInputStream* objstream = new FSObjInputStream(_conf, objname, _underfs);
      thread readThread = thread([=]{objstream->readObj();});
      BlockingQueue<OECDataPacket*>* writeQueue = new BlockingQueue<OECDataPacket*>();
      // 1.1 cacheThread
      int pktnum = objsize * 1048576/_conf->_pktSize; 
      thread cacheThread = thread([=]{cacheWorker(writeQueue, filename, pktnum * obji, pktnum, 1);});

      // 1.3 get pkt from readThread to writeThread
      while (true) {
        OECDataPacket* curpkt = objstream->dequeue();
        if (curpkt->getDatalen()) writeQueue->push(curpkt);
        if (!objstream->hasNext()) break;
      }

      // join
      readThread.join();
      cacheThread.join();

      // delete
      delete objstream;
    } else {
      // issue degraded read for this object
      CoorCommand* coorCmd = new CoorCommand();
      coorCmd->buildType5(5, _conf->_localIp, objname); 
      coorCmd->sendTo(_coorCtx);
      delete coorCmd;
     
      // wait for response
      string instkey = "offlinedegradedinst:" +objname;
      redisReply* instreply;
      redisContext* instCtx = RedisUtil::createContext(_conf->_localIp);
      instreply = (redisReply*)redisCommand(instCtx, "blpop %s 0", instkey.c_str());
      char* inststr = instreply->element[1]->str; 

      // opt
      int opt;
      memcpy((char*)&opt, inststr, 4); inststr += 4;
      opt = ntohl(opt);
      cout << "opt = " << opt << endl;

      if (opt < 0) {
        // lostidx
        int lostidx;
        memcpy((char*)&lostidx, inststr, 4); inststr += 4;
        lostidx = ntohl(lostidx);
        cout << "lostidx = " << lostidx << endl;

        // |ecn|eck|ecw|loadn|loadidx-objname|..|computen|computetask|..|
        cout << "OfflineDegradedRead without technique" << endl;
        // 0.1 ecn
        int ecn;
        memcpy((char*)&ecn, inststr, 4); inststr += 4;
        ecn = ntohl(ecn);
        // 0.2 eck
        int eck;
        memcpy((char*)&eck, inststr, 4); inststr += 4;
        eck = ntohl(eck);
        // 0.3 ecw
        int ecw;
        memcpy((char*)&ecw, inststr, 4); inststr += 4;
        ecw = ntohl(ecw);
        cout << "ecn = " << ecn << ", eck = " << eck << ", ecw = " << ecw << endl;
        // 0.4 load
        int loadn;
        memcpy((char*)&loadn, inststr, 4); inststr += 4;
        loadn = ntohl(loadn);
        vector<int> loadidx;
        vector<string> loadobj;
        for (int loadi=0; loadi<loadn; loadi++) {
          int curidx;
          memcpy((char*)&curidx, inststr, 4); inststr += 4;
          curidx = ntohl(curidx);
          loadidx.push_back(curidx);
          int len;
          memcpy((char*)&len, inststr, 4); inststr += 4;
          len = ntohl(len);
          char* objstr = (char*)calloc(len, sizeof(char));
          memcpy(objstr, inststr, len); inststr += len;
          loadobj.push_back(string(objstr));
          free(objstr);
        }
        cout << "loadobj: ";
        for (int loadi=0; loadi<loadn; loadi++) cout << loadobj[loadi] << " ";
        cout << endl;
        // 0.5 computen
        int computen;
        memcpy((char*)&computen, inststr, 4); inststr += 4;
        computen = ntohl(computen);
        assert(computen>0);

        vector<ECTask*> computeTasks;
        redisReply* rReply;
        redisContext* waitCtx = RedisUtil::createContext(_conf->_localIp);
        for (int computei=0; computei<computen; computei++) {
          string wkey = "compute:" + objname+":"+to_string(computei);
          rReply = (redisReply*)redisCommand(waitCtx, "blpop %s 0", wkey.c_str());
          char* reqStr = rReply -> element[1] -> str;
          ECTask* compute = new ECTask(reqStr);
          compute->dump();
          freeReplyObject(rReply);
          computeTasks.push_back(compute);
        }
        redisFree(waitCtx);

        // 1.0 create input stream
        FSObjInputStream** readStreams = (FSObjInputStream**)calloc(loadn, sizeof(FSObjInputStream*));
        for (int loadi=0; loadi<loadn; loadi++) {
          string loadobjname = loadobj[loadi];
          readStreams[loadi] = new FSObjInputStream(_conf, loadobjname, _underfs);
        }
        vector<thread> readThreads = vector<thread>(loadn);
        for (int loadi=0; loadi<loadn; loadi++) {
          readThreads[loadi] = thread([=]{readStreams[loadi]->readObj();});
        }

        BlockingQueue<OECDataPacket*>* writeQueue = new BlockingQueue<OECDataPacket*>();
        int pktnum = objsize * 1048576/_conf->_pktSize;
        thread cacheThread = thread([=]{cacheWorker(writeQueue, filename, pktnum * obji, pktnum, 1);});
 
        // 2.1 computeThread
        thread computeThread = thread([=]{computeWorkerDegradedOffline(readStreams, loadidx, writeQueue, lostidx, computeTasks, pktnum, ecn, eck, ecw);});

        // join
        for (int loadi=0; loadi<loadn; loadi++) readThreads[loadi].join();
        computeThread.join();
        cacheThread.join();

        // delete
        for (int i=0; i<loadn; i++) delete readStreams[i];
        free(readStreams);
        delete writeQueue;
        for (auto item: computeTasks) delete item;
      } else {
        cout << "OfflineDegradedRead using OpenEC technique" << endl;
        // get |stripename|num|key-ip|key-ip|...| 
        // stripename
        int stripenamelen;
        memcpy((char*)&stripenamelen, inststr, 4); inststr += 4;
        stripenamelen = ntohl(stripenamelen);
        char* stripenamestr = (char*)calloc(stripenamelen, sizeof(char));
        memcpy(stripenamestr, inststr, stripenamelen); inststr += stripenamelen;
        string stripename(stripenamestr);
        free(stripenamestr);
        // num 
        int num;
        memcpy((char*)&num, inststr, 4); inststr += 4;
        num = ntohl(num);
        vector<int> cidxlist;
        vector<unsigned int> iplist;
        for (int i=0; i<num; i++) {
          int cidx;
          memcpy((char*)&cidx, inststr, 4); inststr += 4;
          cidx = ntohl(cidx);
          cidxlist.push_back(cidx);
          unsigned int ip;
          memcpy((char*)&ip, inststr, 4); inststr += 4;
          ip = ntohl(ip);
          iplist.push_back(ip);
        }

        // create fetch queue
        BlockingQueue<OECDataPacket*>** fetchQueue = (BlockingQueue<OECDataPacket*>**)calloc(num, sizeof(BlockingQueue<OECDataPacket*>*));
        for (int i=0; i<num; i++) {
          fetchQueue[i] = new BlockingQueue<OECDataPacket*>();
        }
        // create writeQueue
        BlockingQueue<OECDataPacket*>* writeQueue = new BlockingQueue<OECDataPacket*>();

        // create fetchThread
        int pktnum = objsize * 1048576/_conf->_pktSize;
        vector<thread> fetchThreads = vector<thread>(num);
        for (int i=0; i<num; i++) {
          int cid = cidxlist[i];
          string keybase = stripename+":"+to_string(cid);
          fetchThreads[i] = thread([=]{fetchWorker(fetchQueue[i], keybase, iplist[i], pktnum);});
        } 

        thread cacheThread = thread([=]{cacheWorker(writeQueue, filename, pktnum * obji, pktnum, 1);});

        //fetch pkt from fetchQueue to writeQueue
        for (int i=0; i<pktnum; i++) {
          if (num == 1) {
            OECDataPacket* curpkt = fetchQueue[0]->pop();
            writeQueue->push(curpkt);
            continue;
          } 
          int slicesize = _conf->_pktSize/num;
          char* content = (char*)calloc(4+_conf->_pktSize, sizeof(char));
          int tmplen = htonl(_conf->_pktSize);
          memcpy(content, (char*)&tmplen, 4);
          for (int j=0; j<num; j++) { 
            OECDataPacket* curpkt = fetchQueue[j]->pop();
            memcpy(content+4+j*slicesize, curpkt->getData(), slicesize);
            delete curpkt;
          }
          OECDataPacket* retpkt = new OECDataPacket();
          retpkt->setRaw(content);
          writeQueue->push(retpkt);
        }

        // join
        for (int i=0; i<num; i++) fetchThreads[i].join();
        cacheThread.join();

        // delete
        delete writeQueue;
        for (int i=0; i<num; i++) delete fetchQueue[i];
        free(fetchQueue);
      }
 
      // free
      freeReplyObject(instreply);
      redisFree(instCtx);
    }
  }

  // free
  freeReplyObject(instreply);
  redisFree(instCtx); 
}

void OECWorker::readOnline(string filename, int filesizeMB, int ecn, int eck, int ecw) {
  struct timeval time1, time2, time3, time4;
  gettimeofday(&time1, NULL);
  cout << "OECWorker::readOnline.filename: " << filename << ", filesizeMB: " << filesizeMB << ", ecn: " << ecn << ", eck: " << eck << ", ecw: " << ecw << endl;

  // 1. create ecn input stream and check integrity
  vector<int> integrity;
  vector<int> corruptIdx;
  bool needRecovery = false;
  FSObjInputStream** objstreams = (FSObjInputStream**)calloc(ecn, sizeof(FSObjInputStream*));
  vector<thread> createThreads = vector<thread>(ecn);
  for (int i=0; i<ecn; i++) {
    string objname = filename+"_oecobj_"+to_string(i);
    createThreads[i] = thread([=]{objstreams[i] = new FSObjInputStream(_conf, objname, _underfs);});
  }
  for (int i=0; i<ecn; i++) createThreads[i].join();  
  for (int i=0; i<ecn; i++) {
    string objname = filename+"_oecobj_"+to_string(i);
    if (objstreams[i]->exist()) integrity.push_back(1);
    else {
      integrity.push_back(0);
      corruptIdx.push_back(i);
      if (i < eck) needRecovery = true;
    }
  }  
  gettimeofday(&time2, NULL);
  cout << "OECWorker::readOnline.createInputStream.duration: " << RedisUtil::duration(time1, time2) << endl;

  if (!needRecovery) {
    // we do not need recovery
    vector<thread> readThreads = vector<thread>(eck);
    for (int i=0; i<eck; i++) {
      readThreads[i] = thread([=]{objstreams[i]->readObj();});
    }

    // version 1 start: single caching thread
    int pktnum = filesizeMB * 1048576/_conf->_pktSize; 
    BlockingQueue<OECDataPacket*>* writeQueue = new BlockingQueue<OECDataPacket*>();
    // 1.1 cacheThread
    thread cacheThread = thread([=]{cacheWorker(writeQueue, filename, pktnum, 1);});

    // 1.3 get pkt from readThread to writeThread
    struct timeval push1, push2;
    gettimeofday(&push1, NULL);
    while (true) {
      for (int i=0; i<eck; i++) {
        if (objstreams[i]->hasNext()) {
          OECDataPacket* curpkt = objstreams[i]->dequeue();
          if (curpkt->getDatalen()) writeQueue->push(curpkt);
        }
      }
      // check whether to do another round
      bool goon=false;
      for (int i=0; i<eck; i++) { 
        if (objstreams[i]->hasNext()) { goon = true; break;}
      }
      if (!goon) break;
    }
    gettimeofday(&push2, NULL);
    cout << "OECWorker::readOnline.pushduration: " << RedisUtil::duration(push1, push2) << endl;

    // join
    for (int i=0; i<eck; i++) readThreads[i].join();
    cacheThread.join();

    // delete
    delete writeQueue;
    // version 1 end

//    // version 2 start: multi caching thread
//    int pktnum = filesizeMB / eck * 1048576 / _conf->_pktSize;
//    vector<thread> cacheThreads = vector<thread>(eck);
//    for (int i=0; i<eck; i++) {
//      cacheThreads[i] = thread([=]{cacheWorker(objstreams[i]->getQueue(), filename, i, eck, pktnum, 1);});
//    }
//    
//    // join
//    for (int i=0; i<eck; i++) readThreads[i].join();
//    for (int i=0; i<eck; i++) cacheThreads[i].join();
//    // version 2 end 
  } else {
    // need recovery
    // send request to coordinator
    CoorCommand* degradedCmd = new CoorCommand();
    degradedCmd->buildType9(9, _conf->_localIp, filename, corruptIdx);
    degradedCmd->sendTo(_conf->_coorIp);
    delete degradedCmd;
    
    // 0. wait for instruction from coordinator
    // |loadn|loadidx|computen|computetasks
    struct timeval instt1, instt2;
    gettimeofday(&instt1, NULL);
    string instkey = "onlinedegradedinst:" +filename;
    redisReply* instreply;
    redisContext* instCtx = RedisUtil::createContext(_conf->_localIp);
    instreply = (redisReply*)redisCommand(instCtx, "blpop %s 0", instkey.c_str());
    char* inststr = instreply->element[1]->str;

    // 0.1 loadn
    cout << "OECWorker::readOnline.degraded.loadidx: ";
    int loadn;
    memcpy((char*)&loadn, inststr, 4); inststr += 4;
    loadn = ntohl(loadn);
    vector<int> loadidx;
    for (int i=0; i<loadn; i++) {
      int idx;
      memcpy((char*)&idx, inststr, 4); inststr += 4;
      idx = ntohl(idx);
      cout << idx << " ";
      loadidx.push_back(idx);
    }
    cout << endl;
    // 0.2 computen
    int computen;
    memcpy((char*)&computen, inststr, 4); inststr += 4;
    computen = ntohl(computen);
    vector<ECTask*> computeTasks;
    freeReplyObject(instreply);
    redisFree(instCtx);

    redisReply* rReply;
    redisContext* waitCtx = RedisUtil::createContext(_conf->_localIp);
    for (int i=0; i<computen; i++) {
      string wkey = "compute:" + filename+":"+to_string(i);
      rReply = (redisReply*)redisCommand(waitCtx, "blpop %s 0", wkey.c_str());
      char* reqStr = rReply -> element[1] -> str;
      ECTask* compute = new ECTask(reqStr);
      compute->dump();
      freeReplyObject(rReply);
      computeTasks.push_back(compute);
    }
    redisFree(waitCtx);
    gettimeofday(&instt2, NULL);

    cout << "OECWorker::readOnline.wait degraded inst = " << RedisUtil::duration(time1, time2) << endl;
    cout << "OECWorker::loadn = " << loadn << ", loadidx.size() = " << loadidx.size() << endl;
 
    FSObjInputStream** readStreams = (FSObjInputStream**)calloc(loadn, sizeof(FSObjInputStream*));
    for (int i=0; i<loadn; i++) {
      readStreams[i] = objstreams[loadidx[i]];
      cout << "readStreams[" << i << "] = objstreams[" << loadidx[i] << "]" << endl;
    }

    vector<thread> readThreads = vector<thread>(loadn);
    for (int i=0; i<loadn; i++) {
      readThreads[i] = thread([=]{readStreams[i]->readObj();});
    }

    BlockingQueue<OECDataPacket*>* writeQueue = new BlockingQueue<OECDataPacket*>();
    // 1.1 cacheThread
    int pktnum = filesizeMB * 1048576/_conf->_pktSize; 
    thread cacheThread = thread([=]{cacheWorker(writeQueue, filename, pktnum, 1);});

    // 2.1 computeThread
    thread computeThread = thread([=]{computeWorker(readStreams, loadidx, writeQueue, computeTasks, pktnum, ecn, eck, ecw);});

    // join
    for (int i=0; i<loadn; i++) {
      readThreads[i].join();
    }
    computeThread.join();
    cacheThread.join();

    // delete
    free(readStreams);
    delete writeQueue;
  }

  // last. delete and free
  for (int i=0; i<ecn; i++) {
    if (objstreams[i]) delete objstreams[i];
  }
  free(objstreams);
  gettimeofday(&time3, NULL);
  cout << "OECWorker::readOnline.duration: " << RedisUtil::duration(time1, time3) << endl;
}
