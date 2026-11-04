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
#include "Tokenize.h"
#include "StringConvert.h"
#include <unordered_set>
#include "WorldSession.h"
#include "WorldSessionMgr.h"

using namespace Acore::ChatCommands;

static bool   stimeistime_enable,
              stimeistime_announce;
static float  stimeistime_speed_rate,
              stimeistime_hour_offset,
              stimeistime_outland_hour, // TimeIsTime.Outland.FixedHour, below 0 = Outland follows the world clock
              stimeistime_maps_hour;    // TimeIsTime.FixedHour.Hour for the maps of TimeIsTime.FixedHour.MapIds
static std::unordered_set<uint32> stimeistime_fixed_maps;
static uint32 stimeistime_time_start;
static int64  stimeistime_gm_offset = 0; // game-time seconds added by .worldtime, not persisted

static time_t TimeIsTimeNow() {
    uint32 speed_time  = (GameTime::GetGameTime().count() - GameTime::GetUptime().count()) + (GameTime::GetUptime().count() * stimeistime_speed_rate);
    float  hour_offset = stimeistime_hour_offset  * 3600;
    uint32 time_start  = stimeistime_time_start + hour_offset;

    time_t base = stimeistime_time_start > 0 ? time_start : speed_time;
    return base + stimeistime_gm_offset;
}

// The world clock the clients see, for other modules (mod-capital-weather-sync: morning fog by game hour).
// Declared there as `time_t TimeIsTimeWorldNow();` - both modules are linked into the same worldserver.
time_t TimeIsTimeWorldNow() {
    return stimeistime_enable ? TimeIsTimeNow() : GameTime::GetGameTime().count();
}

// what the player's client was told last: the world clock (below 0) or the hour it stands at
struct TimeIsTimePlayerData : public DataMap::Base {
    float fixedHour = -1.0f;
};

// Outland has no day and night: there the client clock stands at TimeIsTime.Outland.FixedHour.
// Map 530 also holds Eversong Woods, Ghostlands, Silvermoon, the Isle of Quel'Danas, Azuremyst, Bloodmyst and
// the Exodar - Azeroth, with an ordinary day. All of them lie far south of Outland (y below -5000).
static bool TimeIsTimeInOutland(Player const* player) {
    return stimeistime_outland_hour >= 0.0f && player->GetMapId() == 530 && player->GetPositionY() > -2500.0f;
}

// the hour the client clock stands at here, below 0 = the world clock. Besides Outland: whole maps with an endless
// day or night, such as the test range on Azshara Crater (map 37, docs/test-range.md)
static float TimeIsTimeFixedHour(Player const* player) {
    if (TimeIsTimeInOutland(player))
        return stimeistime_outland_hour;

    if (stimeistime_maps_hour >= 0.0f && stimeistime_fixed_maps.count(player->GetMapId()))
        return stimeistime_maps_hour;

    return -1.0f;
}

// today's date at the fixed hour
static time_t TimeIsTimeFixed(float hour) {
    std::tm lt = Acore::Time::TimeBreakdown(TimeIsTimeNow());
    uint32 minutes = uint32(hour * 60.0f) % (24 * 60);
    lt.tm_hour = minutes / 60;
    lt.tm_min = minutes % 60;
    lt.tm_sec = 0;
    return mktime(&lt);
}

static void TimeIsTimeSend(WorldSession* session) {
    Player* player = session->GetPlayer();
    float hour = player ? TimeIsTimeFixedHour(player) : -1.0f;
    bool fixed = hour >= 0.0f;
    if (player)
        player->CustomData.GetDefault<TimeIsTimePlayerData>("TimeIsTime")->fixedHour = hour;

    WorldPacket data(SMSG_LOGIN_SETTIMESPEED, 4 + 4 + 4);
    data.AppendPackedTime(fixed ? TimeIsTimeFixed(hour) : TimeIsTimeNow());
    // game minutes per real second. A standing clock: not 0 (the client is not known to take a stopped clock),
    // one game minute in more than a day
    data << (fixed ? 0.00001f : float(0.01666667f) * stimeistime_speed_rate);
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
        stimeistime_outland_hour = sConfigMgr->GetOption<float>("TimeIsTime.Outland.FixedHour", 12.0);
        stimeistime_maps_hour = sConfigMgr->GetOption<float>("TimeIsTime.FixedHour.Hour", 12.0);
        stimeistime_fixed_maps.clear();
        std::string const maps = sConfigMgr->GetOption<std::string>("TimeIsTime.FixedHour.MapIds", "");
        for (std::string_view id : Acore::Tokenize(maps, ',', false))
            if (Optional<uint32> map = Acore::StringTo<uint32>(id))
                stimeistime_fixed_maps.insert(*map);
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

    // Outland and the fixed-hour maps can be entered and left without a change of map (the Shattrath portals to Silvermoon and the
    // Exodar, a GM teleport), and then no initial packets are sent
    void OnPlayerUpdateZone(Player* player, uint32 /*newZone*/, uint32 /*newArea*/) override {
        if (!stimeistime_enable)
            return;

        if (TimeIsTimeFixedHour(player) != player->CustomData.GetDefault<TimeIsTimePlayerData>("TimeIsTime")->fixedHour)
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
