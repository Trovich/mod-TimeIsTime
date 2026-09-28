/*
 * Azerothcore Module:     TimeIsTime
 * Author:                 Dunjeon
 * Contributing Author(s): lasyan3, vratam @ RegWorks
 * Version:                20250608
 * License:                GNU Affero General Public License v3.0.
 */

#include "ScriptMgr.h"
#include "Player.h"
#include "Config.h"
#include "Chat.h"
#include "GameTime.h"
#include "Timer.h"
#include "WorldSession.h"
#include "WorldSessionMgr.h"

using namespace Acore::ChatCommands;

static bool   stimeistime_enable,
              stimeistime_announce;
static float  stimeistime_speed_rate,
              stimeistime_hour_offset;
static uint32 stimeistime_time_start;
static int64  stimeistime_gm_offset = 0; // game-time seconds added by .worldtime, not persisted

static time_t TimeIsTimeNow() {
    uint32 speed_time  = (GameTime::GetGameTime().count() - GameTime::GetUptime().count()) + (GameTime::GetUptime().count() * stimeistime_speed_rate);
    float  hour_offset = stimeistime_hour_offset  * 3600;
    uint32 time_start  = stimeistime_time_start + hour_offset;

    time_t base = stimeistime_time_start > 0 ? time_start : speed_time;
    return base + stimeistime_gm_offset;
}

static void TimeIsTimeSend(WorldSession* session) {
    WorldPacket data(SMSG_LOGIN_SETTIMESPEED, 4 + 4 + 4);
    data.AppendPackedTime(TimeIsTimeNow());
    data << float(0.01666667f) * stimeistime_speed_rate;
    data << uint32(0);
    session->SendPacket(&data);
}

class TimeIsTimeBeforeConfigLoad : public WorldScript {
public:

    TimeIsTimeBeforeConfigLoad() : WorldScript("TimeIsTimeBeforeConfigLoad") { }

    void OnBeforeConfigLoad(bool /*reload*/) override {
        stimeistime_enable = sConfigMgr->GetOption<bool>("TimeIsTime.Enable", true);
        stimeistime_announce = sConfigMgr->GetOption<bool>("TimeIsTime.Announce", true);
        stimeistime_speed_rate = sConfigMgr->GetOption<float>("TimeIsTime.SpeedRate", 1.0);
        stimeistime_hour_offset = sConfigMgr->GetOption<float>("TimeIsTime.HourOffset", 0.0);
        stimeistime_time_start = sConfigMgr->GetOption<int>("TimeIsTime.TimeStart", 0);
    }
};

class TimeIsTime : public PlayerScript {
public:

    TimeIsTime() : PlayerScript("TimeIsTime") { }

    void OnPlayerLogin(Player* player) {
        if (stimeistime_enable && stimeistime_announce)
            ChatHandler(player->GetSession()).SendSysMessage("This server is running the |cff4CFF00TimeIsTime |rmodule");
    }
	
    void OnPlayerSendInitialPacketsBeforeAddToMap(Player* player, WorldPacket& /*data*/) override {
        if (!stimeistime_enable)
            return;

        TimeIsTimeSend(player->GetSession());
    }
};

class TimeIsTimeCommands : public CommandScript {
public:

    TimeIsTimeCommands() : CommandScript("TimeIsTimeCommands") { }

    ChatCommandTable GetCommands() const override {
        static ChatCommandTable worldtimeCommands = {
            { "",    HandleShow, SEC_GAMEMASTER, Console::Yes },
            { "set", HandleSet,  SEC_GAMEMASTER, Console::Yes },
            { "add", HandleAdd,  SEC_GAMEMASTER, Console::Yes },
        };

        static ChatCommandTable commandTable = {
            { "worldtime", worldtimeCommands },
        };

        return commandTable;
    }

    static void ShowTime(ChatHandler* handler) {
        std::tm lt = Acore::Time::TimeBreakdown(TimeIsTimeNow());
        handler->PSendSysMessage("TimeIsTime: game time {:02}:{:02}, speed x{}, GM offset {} min.",
            lt.tm_hour, lt.tm_min, stimeistime_speed_rate, stimeistime_gm_offset / 60);
    }

    static void Broadcast() {
        for (auto const& [accountId, session] : sWorldSessionMgr->GetAllSessions())
            if (session && session->GetPlayer() && session->GetPlayer()->IsInWorld())
                TimeIsTimeSend(session);
    }

    // .worldtime
    static bool HandleShow(ChatHandler* handler) {
        if (!stimeistime_enable) {
            handler->SendErrorMessage("TimeIsTime is disabled.");
            return false;
        }

        ShowTime(handler);
        return true;
    }

    // .worldtime set <hour> [minute] - jumps forward to the next such time of day
    static bool HandleSet(ChatHandler* handler, uint32 hour, Optional<uint32> minute) {
        if (!stimeistime_enable) {
            handler->SendErrorMessage("TimeIsTime is disabled.");
            return false;
        }

        uint32 min = minute.value_or(0);
        if (hour > 23 || min > 59) {
            handler->SendErrorMessage("Usage: .worldtime set <0-23> [0-59]");
            return false;
        }

        std::tm lt = Acore::Time::TimeBreakdown(TimeIsTimeNow());
        int64 current = lt.tm_hour * HOUR + lt.tm_min * MINUTE + lt.tm_sec;
        int64 delta = int64(hour * HOUR + min * MINUTE) - current;
        if (delta < 0)
            delta += DAY;

        stimeistime_gm_offset += delta;
        Broadcast();
        ShowTime(handler);
        return true;
    }

    // .worldtime add <minutes> - negative values rewind
    static bool HandleAdd(ChatHandler* handler, int32 minutes) {
        if (!stimeistime_enable) {
            handler->SendErrorMessage("TimeIsTime is disabled.");
            return false;
        }

        stimeistime_gm_offset += int64(minutes) * MINUTE;
        Broadcast();
        ShowTime(handler);
        return true;
    }
};

void AddTimeIsTimeScripts() {
    new TimeIsTimeBeforeConfigLoad();
    new TimeIsTime();
    new TimeIsTimeCommands();
}
