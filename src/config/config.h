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

#ifndef ROBOTV_CONFIG_H
#define ROBOTV_CONFIG_H

#include <string>
#include <stdint.h>

#include <vdr/config.h>

// log output configuration

#ifdef CONSOLEDEBUG
#define DEBUGLOG(format, ...) printf("roboTV: " format, ##__VA_ARGS__)
#elif defined  DEBUG
#define DEBUGLOG(format, ...) dsyslog("roboTV: " format, ##__VA_ARGS__)
#else
#define DEBUGLOG(format, ...)
#endif

#ifdef CONSOLEDEBUG
#define INFOLOG(format, ...) printf("roboTV: " format, ##__VA_ARGS__)
#define ERRORLOG(format, ...) printf("roboTV-Error: " format, ##__VA_ARGS__)
#else
#define INFOLOG(format, ...) isyslog("roboTV: " format, ##__VA_ARGS__)
#define ERRORLOG(format, ...) esyslog("roboTV-Error: " format, ##__VA_ARGS__)
#endif

// default settings

#define ALLOWED_HOSTS_FILE  "allowed_hosts.conf"
#define FRONTEND_DEVICE     "/dev/dvb/adapter%d/frontend%d"
#define GENERAL_CONFIG_FILE "robotv.conf"
#define RESUME_DATA_FILE    "resume.data"
#define CHANNEL_CACHE_FILE  "channelcache.data"
#define STORAGE_DB_FILE     "storage.db"

#define LISTEN_PORT       34891
#define LISTEN_PORT_S    "34891"
#define DISCOVERY_PORT    34891

// backward compatibility

#if APIVERSNUM < 10701
#define FOLDERDELIMCHAR '~'
#endif


class RoboTVServerConfig : public cConfig<cSetupLine> {
protected:

    RoboTVServerConfig();

    bool Parse(const char* Name, const char* Value);

public:

    void Load();

    static RoboTVServerConfig& instance();

    // Remote server settings
    std::string ConfigDirectory;      // config directory path
    std::string CacheDirectory;       // cache directory path
    uint16_t listen_port;         // Port of remote server
    uint16_t stream_timeout;      // timeout in seconds for stream data
    std::string PiconsURL;
    std::string ReorderCmd;
};

#endif // ROBOTV_CONFIG_H
