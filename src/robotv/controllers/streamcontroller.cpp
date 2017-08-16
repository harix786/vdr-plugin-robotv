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
#include "robotv/robotvclient.h"
#include "tools/hash.h"

StreamController::StreamController(RoboTvClient* parent) :
    m_langStreamType(StreamInfo::Type::AC3),
    m_parent(parent) {
}

StreamController::StreamController(const StreamController& orig) {
}

StreamController::~StreamController() {
    stopStreaming();
}

MsgPacket* StreamController::process(MsgPacket* request) {
    /** OPCODE 20 - 39: RoboTV network functions for live streaming */

    switch(request->getMsgID()) {
        case ROBOTV_CHANNELSTREAM_OPEN:
            return processOpen(request);

        case ROBOTV_CHANNELSTREAM_CLOSE:
            return processClose(request);

        case ROBOTV_CHANNELSTREAM_REQUEST:
            return processRequest(request);

        case ROBOTV_CHANNELSTREAM_PAUSE:
            return processPause(request);

        case ROBOTV_CHANNELSTREAM_SIGNAL:
            return processSignal(request);

        case ROBOTV_CHANNELSTREAM_SEEK:
            return processSeek(request);
    }

    return nullptr;
}

MsgPacket* StreamController::processOpen(MsgPacket* request) {
    uint32_t uid = request->get_U32();
    int32_t priority = 50;

    if(!request->eop()) {
        priority = request->get_S32();
    }

    if(!request->eop()) {
        request->get_U8(); // BINARY COMPATIBILITY (was waitForKeyFrame)
    }

    // get preferred language
    if(!request->eop()) {
        m_language = request->get_String();
        m_langStreamType = (StreamInfo::Type)request->get_U8();
    }

    if(m_langStreamType == StreamInfo::Type::NONE) {
        m_langStreamType = StreamInfo::Type::AC3;
    }

    if(!m_language.empty()) {
        isyslog("Preferred language: %s / type: %i", m_language.c_str(), (int)m_langStreamType);
    }

    stopStreaming();

    // try to find channel by uid first

    int status;
    MsgPacket *response = nullptr;
    std::string channelName;
    const cChannel *channel = nullptr;

    {
        LOCK_CHANNELS_READ;
        channel = findChannelByUid(Channels, uid);

        response = createResponse(request);

        if (channel == nullptr) {
            esyslog("Can't find channel %08x", uid);
            response->put_U32(ROBOTV_RET_DATAINVALID);
            return response;
        }

        channelName = channel->Name();
        status = startStreaming(
                channel,
                priority);
    }

    if(status == ROBOTV_RET_OK) {
        isyslog("--------------------------------------");
        isyslog("Started streaming of channel %s (priority %i)", channelName.c_str(), priority);
    }
    else {
        LOCK_TIMERS_READ;
        time_t now = time(nullptr);

        for (const cTimer *ti = Timers->First(); ti; ti = Timers->Next(ti)) {
            if (ti->Recording() && ti->Matches(now)) {
                esyslog("Recording running !");
                status = ROBOTV_RET_RECRUNNING;
            }
        }
        esyslog("Can't stream channel %s (status: %i)", channelName.c_str(), status);
    }

    response->put_U32(status);
    return response;
}

MsgPacket* StreamController::processClose(MsgPacket* request) {
    stopStreaming();
    return createResponse(request);
}

MsgPacket* StreamController::processRequest(MsgPacket* request) {
    if(m_streamer == nullptr) {
        return nullptr;
    }

    MsgPacket* p = m_streamer->requestPacket();

    if(p == nullptr) {
        return createResponse(request);
    }

    return createResponse(request, p);
}

MsgPacket* StreamController::processPause(MsgPacket* request) {
    if(m_streamer == nullptr) {
        return nullptr;
    }

    bool on = request->get_U32();
    isyslog("LIVESTREAM: %s", on ? "PAUSED" : "TIMESHIFT");

    m_streamer->pause(on);

    return createResponse(request);
}

MsgPacket* StreamController::processSignal(MsgPacket* request) {
    if(m_streamer == nullptr) {
        return nullptr;
    }

    m_streamer->requestSignalInfo();
    return nullptr;
}

void StreamController::processChannelChange(const cChannel* Channel) {
    if(m_streamer != NULL) {
        m_streamer->processChannelChange(Channel);
    }
}

int StreamController::startStreaming(const cChannel* channel, int32_t priority) {
    std::lock_guard<std::mutex> lock(m_lock);

    m_streamer = new LiveStreamer(m_parent, priority);
    m_streamer->setLanguage(m_language.c_str(), m_langStreamType);

    return m_streamer->switchChannel(channel);
}

void StreamController::stopStreaming() {
    std::lock_guard<std::mutex> lock(m_lock);

    delete m_streamer;
    m_streamer = NULL;
}

MsgPacket* StreamController::processSeek(MsgPacket* request) {
    std::lock_guard<std::mutex> lock(m_lock);

    if(m_streamer == nullptr) {
        return nullptr;
    }

    int64_t position = request->get_S64();
    int64_t pts = m_streamer->seek(position);

    MsgPacket* response = createResponse(request);
    response->put_S64(pts);
    return response;
}
