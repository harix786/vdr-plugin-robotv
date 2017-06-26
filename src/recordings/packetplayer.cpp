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

#include <live/livestreamer.h>
#include <tools/time.h>
#include "packetplayer.h"

#define MIN_PACKET_SIZE (128 * 1024)

PacketPlayer::PacketPlayer(cRecording* rec) : RecPlayer(rec), m_demuxers(this) {
    m_requestStreamChange = true;
    m_index = new cIndexFile(rec->FileName(), false);
    m_recording = rec;
    m_position = 0;
    m_patVersion = -1;
    m_pmtVersion = -1;

    // initial start / end time
    m_startTime = std::chrono::milliseconds(0);
    m_endTime = std::chrono::milliseconds(0);

    // allocate buffer
    m_buffer = (uint8_t*)malloc(TS_SIZE * maxPacketCount);
}

PacketPlayer::~PacketPlayer() {
    clearQueue();
    free(m_buffer);
    delete m_index;
}

void PacketPlayer::onStreamPacket(TsDemuxer::StreamPacket *p) {
    // stream change needed / requested
    if(m_requestStreamChange && m_demuxers.isReady()) {

        isyslog("demuxers ready");

        for(auto i : m_demuxers) {
            isyslog("%s", i->info().c_str());
        }

        isyslog("create streamchange packet");
        m_requestStreamChange = false;

        // push streamchange into queue
        MsgPacket* packet = LiveStreamer::createStreamChangePacket(m_demuxers);
        m_queue.push_back(packet);

        // push pre-queued packets
        dsyslog("processing %lu pre-queued packets", m_preQueue.size());

        while(!m_preQueue.empty()) {
            packet = m_preQueue.front();
            m_preQueue.pop_front();
            m_queue.push_back(packet);
        }
    }

    // skip non video / audio packets
    if(p->content != StreamInfo::Content::VIDEO && p->content != StreamInfo::Content::AUDIO) {
        return;
    }

    // initialise stream packet
    MsgPacket* packet = new MsgPacket(ROBOTV_STREAM_MUXPKT, ROBOTV_CHANNEL_STREAM);
    packet->disablePayloadCheckSum();

    // write stream data
    packet->put_U16(p->pid);

    packet->put_S64(p->pts);
    packet->put_S64(p->dts);
    packet->put_U32(p->duration);

    // write frame type into unused header field clientid
    packet->setClientID((uint16_t)p->frameType);

    // write payload into stream packet
    packet->put_U32(p->size);
    packet->put_Blob(p->data, p->size);

    int64_t durationMs = (m_recording->LengthInSeconds() * 1000 * p->streamPosition) / m_totalLength;
    int64_t currentTime = m_startTime.count() + durationMs;

    // add timestamp (wallclock time in ms starting at m_startTime)
    dsyslog("timestamp: %lu", currentTime / 1000);
    packet->put_S64(currentTime);

    // pre-queue packet
    if(!m_demuxers.isReady()) {
        if(m_preQueue.size() > 50) {
            esyslog("pre-queue full - skipping packet");
            delete packet;
            return;
        }

        m_preQueue.push_back(packet);
        return;
    }

    m_queue.push_back(packet);
}

void PacketPlayer::onStreamChange() {
    if(!m_requestStreamChange) {
        isyslog("stream change requested");
    }

    m_requestStreamChange = true;
}

MsgPacket* PacketPlayer::getNextPacket() {

    // check packet queue first
    if(!m_queue.empty()) {
        MsgPacket* packet = m_queue.front();
        m_queue.pop_front();

        return packet;
    }

    int pmtVersion = 0;
    unsigned char* p = m_buffer;

    // get next block (TS packets)
    int bytesRead = getBlock(p, m_position, maxPacketCount * TS_SIZE);

    // TS sync
    int offset = 0;
    while(offset < (bytesRead - TS_SIZE) && (*p != TS_SYNC_BYTE || p[TS_SIZE] != TS_SYNC_BYTE || !TsHasPayload(p))) {
        p++;
        offset++;
    }

    if(offset > 0) {
        isyslog("skipping %i bytes until next TS packet !", offset);
    }

    // skip bytes until next sync
    bytesRead -= offset;
    m_position += offset;

    // we need at least one TS packet
    if(bytesRead < TS_SIZE) {
        esyslog("PacketPlayer: packet (%i bytes) smaller than TS packet size", bytesRead);
        return nullptr;
    }

    // round to TS_SIZE border
    int count = (bytesRead / TS_SIZE);
    int bufferSize = TS_SIZE * count;

    // advance to next block
    m_position += bufferSize;

    // new PAT / PMT found ?
    if(m_parser.ParsePatPmt(p, bufferSize)) {
        m_parser.GetVersions(m_patVersion, pmtVersion);

        if(pmtVersion > m_pmtVersion) {
            isyslog("found new PMT version (%i)", pmtVersion);
            m_pmtVersion = pmtVersion;

            // update demuxers from new PMT
            isyslog("updating demuxers");
            StreamBundle streamBundle = createFromPatPmt(&m_parser);
            m_demuxers.updateFrom(&streamBundle);

            m_requestStreamChange = true;
        }
    }

    // put packets into demuxer
    for(int i = 0; i < count; i++) {
        if(*p == TS_SYNC_BYTE) {
            m_demuxers.processTsPacket(p, m_position);
        }

        p += TS_SIZE;
    }

    // currently there isn't any packet available
    return nullptr;
}

MsgPacket* PacketPlayer::getPacket() {
    if(m_position >= m_totalLength) {
        dsyslog("PacketPlayer: end of file reached (position=%ld / total=%ld)", m_position, m_totalLength);
        // TODO - send end of stream packet
        return nullptr;
    }

    MsgPacket* p = nullptr;

    // process data until the next packet drops out
    while(m_position < m_totalLength && p == nullptr) {
        p = getNextPacket();
    }

    return p;
}

MsgPacket* PacketPlayer::requestPacket() {
    MsgPacket* p = nullptr;

    // create payload packet
    if(m_streamPacket == nullptr) {
        m_streamPacket = new MsgPacket();
        m_streamPacket->disablePayloadCheckSum();
    }

    while((p = getPacket()) != nullptr) {

        // recheck recording duration
        if((p->getClientID() == (uint16_t)StreamInfo::FrameType::IFRAME && update()) || endTime().count() == 0) {
            if(startTime().count() == 0) {
                m_startTime = roboTV::currentTimeMillis();
            }
            m_endTime = m_startTime + std::chrono::milliseconds(m_recording->LengthInSeconds() * 1000);
        }

        // add start / endtime
        if(m_streamPacket->eop()) {
            m_streamPacket->put_S64(startTime().count());
            m_streamPacket->put_S64(endTime().count());
        }

        // add data
        m_streamPacket->put_U16(p->getMsgID());
        m_streamPacket->put_U16(p->getClientID());

        // add payload
        uint8_t* data = p->getPayload();
        uint32_t length = p->getPayloadLength();
        m_streamPacket->put_Blob(data, length);

        delete p;

        // send payload packet if it's big enough
        if(m_streamPacket->getPayloadLength() >= MIN_PACKET_SIZE) {
            MsgPacket* result = m_streamPacket;
            m_streamPacket = nullptr;

            return result;
        }
    }

    dsyslog("PacketPlayer: requestPacket didn't get any packet !");
    return nullptr;
}

void PacketPlayer::clearQueue() {
    MsgPacket* p = NULL;

    while(m_queue.size() > 0) {
        p = m_queue.front();
        m_queue.pop_front();
        delete p;
    }
}

void PacketPlayer::reset() {
    // reset parser
    m_parser.Reset();
    m_demuxers.clear();
    m_requestStreamChange = true;
    m_patVersion = -1;
    m_pmtVersion = -1;

    // reset current stream packet
    delete m_streamPacket;
    m_streamPacket = nullptr;

    // remove pending packets
    clearQueue();
}

int64_t PacketPlayer::filePositionFromClock(int64_t wallclockTimeMs) {
    int64_t durationSinceStartMs = wallclockTimeMs - startTime().count();
    int64_t durationMs = endTime().count() - startTime().count();

    return (m_totalLength * durationSinceStartMs) / durationMs;
}

int64_t PacketPlayer::seek(int64_t wallclockTimeMs) {
    // adujst position to TS packet borders
    m_position = filePositionFromClock(wallclockTimeMs);

    // invalid position ?
    if(m_position >= m_totalLength) {
        m_position = m_totalLength;
    }

    if(m_position < 0) {
        m_position = 0;
    }

    isyslog("seek: %lu / %lu", m_position, m_totalLength);
    dsyslog("SEEK timestamp: %lu", wallclockTimeMs / 1000);

    // reset parser
    reset();
    return 0;
}

StreamBundle PacketPlayer::createFromPatPmt(const cPatPmtParser* patpmt) {
    StreamBundle item;
    int patVersion = 0;
    int pmtVersion = 0;

    if(!patpmt->GetVersions(patVersion, pmtVersion)) {
        return item;
    }

    // add video stream
    int vpid = patpmt->Vpid();
    int vtype = patpmt->Vtype();

    item.addStream(StreamInfo(vpid,
                              vtype == 0x02 ? StreamInfo::Type::MPEG2VIDEO :
                              vtype == 0x1b ? StreamInfo::Type::H264 :
                              vtype == 0x24 ? StreamInfo::Type::H265 :
                              StreamInfo::Type::NONE));

    // add (E)AC3 streams
    for(int i = 0; patpmt->Dpid(i) != 0; i++) {
        int dtype = patpmt->Dtype(i);
        item.addStream(StreamInfo(patpmt->Dpid(i),
                                  dtype == 0x6A ? StreamInfo::Type::AC3 :
                                  dtype == 0x7A ? StreamInfo::Type::EAC3 :
                                  StreamInfo::Type::NONE,
                                  patpmt->Dlang(i)));
    }

    // add audio streams
    for(int i = 0; patpmt->Apid(i) != 0; i++) {
        int atype = patpmt->Atype(i);
        item.addStream(StreamInfo(patpmt->Apid(i),
                                  atype == 0x04 ? StreamInfo::Type::MPEG2AUDIO :
                                  atype == 0x03 ? StreamInfo::Type::MPEG2AUDIO :
                                  atype == 0x0f ? StreamInfo::Type::AAC :
                                  atype == 0x11 ? StreamInfo::Type::LATM :
                                  StreamInfo::Type::NONE,
                                  patpmt->Alang(i)));
    }

    // add subtitle streams
    for(int i = 0; patpmt->Spid(i) != 0; i++) {
        StreamInfo stream(patpmt->Spid(i), StreamInfo::Type::DVBSUB, patpmt->Slang(i));

        stream.setSubtitlingDescriptor(
                patpmt->SubtitlingType(i),
                patpmt->CompositionPageId(i),
                patpmt->AncillaryPageId(i));

        item.addStream(stream);
    }

    return item;
}
