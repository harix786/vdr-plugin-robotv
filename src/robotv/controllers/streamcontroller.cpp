/*
 *      vdr-plugin-robotv - roboTV server plugin for VDR
 *
 *      Copyright (C) 2016 Alexander Pipelka
 *
 *      https://github.com/pipelka/vdr-plugin-robotv
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 *  http://www.gnu.org/copyleft/gpl.html
 *
 */

#include "streamcontroller.h"
#include "config/config.h"
#include "demuxer/streaminfo.h"
#include "robotv/robotvchannels.h"
#include "robotv/robotvclient.h"
#include "tools/hash.h"
#include "net/msgpacket.h"

StreamController::StreamController(RoboTvClient* parent) :
    m_languageIndex(-1),
    m_langStreamType(StreamInfo::stAC3),
    m_parent(parent) {
}

StreamController::StreamController(const StreamController& orig) {
}

StreamController::~StreamController() {
    stopStreaming();
}

bool StreamController::process(MsgPacket* request, MsgPacket* response) {
    /** OPCODE 20 - 39: RoboTV network functions for live streaming */

    switch(request->getMsgID()) {
        case ROBOTV_CHANNELSTREAM_OPEN:
            return processOpen(request, response);

        case ROBOTV_CHANNELSTREAM_CLOSE:
            return processClose(request, response);

        case ROBOTV_CHANNELSTREAM_REQUEST:
            return processRequest(request, response);

        case ROBOTV_CHANNELSTREAM_PAUSE:
            return processPause(request, response);

        case ROBOTV_CHANNELSTREAM_SIGNAL:
            return processSignal(request, response);
    }

    return false;
}

bool StreamController::processOpen(MsgPacket* request, MsgPacket* response) {
    uint32_t uid = request->get_U32();
    int32_t priority = 50;
    bool waitForKeyFrame = false;
    bool rawPts = false;

    if(!request->eop()) {
        priority = request->get_S32();
    }

    if(!request->eop()) {
        waitForKeyFrame = request->get_U8();
    }

    if(!request->eop()) {
        rawPts = request->get_U8();
    }

    // get preferred language
    if(!request->eop()) {
        const char* language = request->get_String();
        m_languageIndex = I18nLanguageIndex(language);
        m_langStreamType = (StreamInfo::Type)request->get_U8();
    }

    if(!m_languageIndex != -1) {
        INFOLOG("Preferred language: %s / type: %i", I18nLanguageCode(m_languageIndex), (int)m_langStreamType);
    }

    uint32_t timeout = RoboTVServerConfig::instance().stream_timeout;

    stopStreaming();

    RoboTVChannels& c = RoboTVChannels::instance();
    c.lock(false);
    const cChannel* channel = NULL;

    // try to find channel by uid first
    channel = findChannelByUid(uid);

    // try channelnumber
    if(channel == NULL) {
        channel = c.get()->GetByNumber(uid);
    }

    c.unlock();

    if(channel == NULL) {
        ERRORLOG("Can't find channel %08x", uid);
        response->put_U32(ROBOTV_RET_DATAINVALID);
        return true;
    }

    int status = startStreaming(
                     request->getProtocolVersion(),
                     channel,
                     timeout,
                     priority,
                     waitForKeyFrame,
                     rawPts);

    if(status == ROBOTV_RET_OK) {
        INFOLOG("--------------------------------------");
        INFOLOG("Started streaming of channel %s (timeout %i seconds, priority %i)", channel->Name(), timeout, priority);
    }
    else {
        DEBUGLOG("Can't stream channel %s", channel->Name());
    }

    response->put_U32(status);
    return true;
}

bool StreamController::processClose(MsgPacket* request, MsgPacket* response) {
    stopStreaming();
    return true;
}

bool StreamController::processRequest(MsgPacket* request, MsgPacket* response) {
    if(m_streamer != NULL) {
        m_streamer->requestPacket();
    }

    // no response needed for the request
    return false;
}

bool StreamController::processPause(MsgPacket* request, MsgPacket* response) {
    bool on = request->get_U32();
    INFOLOG("LIVESTREAM: %s", on ? "PAUSED" : "TIMESHIFT");

    m_streamer->pause(on);

    return true;
}

bool StreamController::processSignal(MsgPacket* request, MsgPacket* response) {
    if(m_streamer == NULL) {
        return false;
    }

    m_streamer->requestSignalInfo();
    return false;
}

void StreamController::processChannelChange(const cChannel* Channel) {
    std::lock_guard<std::mutex> lock(m_lock);

    if(m_streamer != NULL) {
        m_streamer->processChannelChange(Channel);
    }
}

int StreamController::startStreaming(int version, const cChannel* channel, uint32_t timeout, int32_t priority, bool waitForKeyFrame, bool rawPts) {
    std::lock_guard<std::mutex> lock(m_lock);

    m_streamer = new LiveStreamer(m_parent, channel, priority, rawPts);
    m_streamer->setLanguage(m_languageIndex, m_langStreamType);
    m_streamer->setTimeout(timeout);
    m_streamer->setWaitForKeyFrame(waitForKeyFrame);
    m_streamer->setProtocolVersion(version);

    return ROBOTV_RET_OK;
}

void StreamController::stopStreaming() {
    std::lock_guard<std::mutex> lock(m_lock);

    delete m_streamer;
    m_streamer = NULL;
}

