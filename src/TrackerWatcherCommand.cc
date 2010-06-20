/* <!-- copyright */
/*
 * aria2 - The high speed download utility
 *
 * Copyright (C) 2006 Tatsuhiro Tsujikawa
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 *
 * In addition, as a special exception, the copyright holders give
 * permission to link the code of portions of this program with the
 * OpenSSL library under certain conditions as described in each
 * individual source file, and distribute linked combinations
 * including the two.
 * You must obey the GNU General Public License in all respects
 * for all of the code used other than OpenSSL.  If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so.  If you
 * do not wish to do so, delete this exception statement from your
 * version.  If you delete this exception statement from all source
 * files in the program, then also delete it here.
 */
/* copyright --> */
#include "TrackerWatcherCommand.h"

#include <sstream>

#include "DownloadEngine.h"
#include "BtAnnounce.h"
#include "BtRuntime.h"
#include "PieceStorage.h"
#include "PeerStorage.h"
#include "Peer.h"
#include "prefs.h"
#include "message.h"
#include "ByteArrayDiskWriterFactory.h"
#include "RecoverableException.h"
#include "PeerInitiateConnectionCommand.h"
#include "DiskAdaptor.h"
#include "FileEntry.h"
#include "RequestGroup.h"
#include "Option.h"
#include "DlAbortEx.h"
#include "Logger.h"
#include "A2STR.h"
#include "SocketCore.h"
#include "Request.h"
#include "AnnounceTier.h"
#include "DownloadContext.h"
#include "bittorrent_helper.h"
#include "a2functional.h"
#include "util.h"
#include "RequestGroupMan.h"
#include "FileAllocationEntry.h"
#include "CheckIntegrityEntry.h"
#include "ServerStatMan.h"

namespace aria2 {

TrackerWatcherCommand::TrackerWatcherCommand
(cuid_t cuid, RequestGroup* requestGroup, DownloadEngine* e):
  Command(cuid),
  _requestGroup(requestGroup),
  _e(e)
{
  _requestGroup->increaseNumCommand();
}

TrackerWatcherCommand::~TrackerWatcherCommand()
{
  _requestGroup->decreaseNumCommand();
}

bool TrackerWatcherCommand::execute() {
  if(_requestGroup->isForceHaltRequested()) {
    if(_trackerRequestGroup.isNull()) {
      return true;
    } else if(_trackerRequestGroup->getNumCommand() == 0 ||
              _trackerRequestGroup->downloadFinished()) {
      return true;
    } else {
      _trackerRequestGroup->setForceHaltRequested(true);
      _e->addCommand(this);
      return false;
    }
  }
  if(_btAnnounce->noMoreAnnounce()) {
    if(getLogger()->debug()) {
      getLogger()->debug("no more announce");
    }
    return true;
  }
  if(_trackerRequestGroup.isNull()) {
    _trackerRequestGroup = createAnnounce();
    if(!_trackerRequestGroup.isNull()) {
      try {
        std::vector<Command*>* commands = new std::vector<Command*>();
        auto_delete_container<std::vector<Command*> > commandsDel(commands);
        _trackerRequestGroup->createInitialCommand(*commands, _e);
        _e->addCommand(*commands);
        commands->clear();
        if(getLogger()->debug()) {
          getLogger()->debug("added tracker request command");
        }
      } catch(RecoverableException& ex) {
        getLogger()->error(EX_EXCEPTION_CAUGHT, ex);
      }
    }
  } else if(_trackerRequestGroup->downloadFinished()){
    try {
      std::string trackerResponse = getTrackerResponse(_trackerRequestGroup);

      processTrackerResponse(trackerResponse);
      _btAnnounce->announceSuccess();
      _btAnnounce->resetAnnounce();
    } catch(RecoverableException& ex) {
      getLogger()->error(EX_EXCEPTION_CAUGHT, ex);      
      _btAnnounce->announceFailure();
      if(_btAnnounce->isAllAnnounceFailed()) {
        _btAnnounce->resetAnnounce();
      }
    }
    _trackerRequestGroup.reset();
  } else if(_trackerRequestGroup->getNumCommand() == 0){
    // handle errors here
    _btAnnounce->announceFailure(); // inside it, trackers = 0.
    _trackerRequestGroup.reset();
    if(_btAnnounce->isAllAnnounceFailed()) {
      _btAnnounce->resetAnnounce();
    }
  }
  _e->addCommand(this);
  return false;
}

std::string TrackerWatcherCommand::getTrackerResponse
(const SharedHandle<RequestGroup>& requestGroup)
{
  std::stringstream strm;
  unsigned char data[2048];
  requestGroup->getPieceStorage()->getDiskAdaptor()->openFile();
  while(1) {
    ssize_t dataLength = requestGroup->getPieceStorage()->
      getDiskAdaptor()->readData(data, sizeof(data), strm.tellp());
    if(dataLength == 0) {
      break;
    }
    strm.write(reinterpret_cast<const char*>(data), dataLength);
  }
  return strm.str();
}

// TODO we have to deal with the exception thrown By BtAnnounce
void TrackerWatcherCommand::processTrackerResponse
(const std::string& trackerResponse)
{
  _btAnnounce->processAnnounceResponse
    (reinterpret_cast<const unsigned char*>(trackerResponse.c_str()),
     trackerResponse.size());
  while(!_btRuntime->isHalt() && _btRuntime->lessThanMinPeers()) {
    SharedHandle<Peer> peer = _peerStorage->getUnusedPeer();
    if(peer.isNull()) {
      break;
    }
    peer->usedBy(_e->newCUID());
    PeerInitiateConnectionCommand* command =
      new PeerInitiateConnectionCommand
      (peer->usedBy(), _requestGroup, peer, _e, _btRuntime);
    command->setPeerStorage(_peerStorage);
    command->setPieceStorage(_pieceStorage);
    _e->addCommand(command);
    if(getLogger()->debug()) {
      getLogger()->debug("CUID#%s - Adding new command CUID#%s",
                         util::itos(getCuid()).c_str(),
                         util::itos(peer->usedBy()).c_str());
    }
  }
}

SharedHandle<RequestGroup> TrackerWatcherCommand::createAnnounce() {
  SharedHandle<RequestGroup> rg;
  if(_btAnnounce->isAnnounceReady()) {
    rg = createRequestGroup(_btAnnounce->getAnnounceUrl());
    _btAnnounce->announceStart(); // inside it, trackers++.
  }
  return rg;
}

static bool backupTrackerIsAvailable
(const SharedHandle<DownloadContext>& context)
{
  SharedHandle<TorrentAttribute> torrentAttrs =
    bittorrent::getTorrentAttrs(context);
  if(torrentAttrs->announceList.size() >= 2) {
    return true;
  }
  if(torrentAttrs->announceList.empty()) {
    return false;
  }
  if(torrentAttrs->announceList[0].size() >= 2) {
    return true;
  } else {
    return false;
  }
}

SharedHandle<RequestGroup>
TrackerWatcherCommand::createRequestGroup(const std::string& uri)
{
  std::vector<std::string> uris;
  uris.push_back(uri);
  SharedHandle<RequestGroup> rg(new RequestGroup(getOption()));
  if(backupTrackerIsAvailable(_requestGroup->getDownloadContext())) {
    if(getLogger()->debug()) {
      getLogger()->debug("This is multi-tracker announce.");
    }
  } else {
    if(getLogger()->debug()) {
      getLogger()->debug("This is single-tracker announce.");
    }
  }
  // If backup tracker is available, try 2 times for each tracker
  // and if they all fails, then try next one.
  rg->getOption()->put(PREF_MAX_TRIES, "2");
  // TODO When dry-run mode becomes available in BitTorrent, set
  // PREF_DRY_RUN=false too.
  rg->getOption()->put(PREF_USE_HEAD, V_FALSE);
  // Setting tracker timeouts
  rg->setTimeout(rg->getOption()->getAsInt(PREF_BT_TRACKER_TIMEOUT));
  rg->getOption()->put(PREF_CONNECT_TIMEOUT,
                       rg->getOption()->get(PREF_BT_TRACKER_CONNECT_TIMEOUT));
  static const std::string TRACKER_ANNOUNCE_FILE("[tracker.announce]");
  SharedHandle<DownloadContext> dctx
    (new DownloadContext(getOption()->getAsInt(PREF_SEGMENT_SIZE),
                         0,
                         TRACKER_ANNOUNCE_FILE));
  dctx->setDir(A2STR::NIL);
  dctx->getFileEntries().front()->setUris(uris);
  rg->setDownloadContext(dctx);
  SharedHandle<DiskWriterFactory> dwf(new ByteArrayDiskWriterFactory());
  rg->setDiskWriterFactory(dwf);
  rg->setFileAllocationEnabled(false);
  rg->setPreLocalFileCheckEnabled(false);
  util::removeMetalinkContentTypes(rg);
  if(getLogger()->info()) {
    getLogger()->info("Creating tracker request group GID#%s",
                      util::itos(rg->getGID()).c_str());
  }
  return rg;
}

void TrackerWatcherCommand::setBtRuntime
(const SharedHandle<BtRuntime>& btRuntime)
{
  _btRuntime = btRuntime;
}

void TrackerWatcherCommand::setPeerStorage
(const SharedHandle<PeerStorage>& peerStorage)
{
  _peerStorage = peerStorage;
}

void TrackerWatcherCommand::setPieceStorage
(const SharedHandle<PieceStorage>& pieceStorage)
{
  _pieceStorage = pieceStorage;
}

void TrackerWatcherCommand::setBtAnnounce
(const SharedHandle<BtAnnounce>& btAnnounce)
{
  _btAnnounce = btAnnounce;
}

const SharedHandle<Option>& TrackerWatcherCommand::getOption() const
{
  return _requestGroup->getOption();
}

} // namespace aria2
