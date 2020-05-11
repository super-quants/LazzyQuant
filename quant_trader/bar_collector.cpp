#include <QHash>
#include <QMetaEnum>

#include "enum_helper.h"
#include "datetime_helper.h"
#include "db_helper.h"
#include "standard_bar_persistence.h"
#include "bar_collector.h"

BarCollector::BarCollector(const QString &instrumentID, int timeFrameFlags, bool saveBarsToDB, QObject *parent) :
    QObject(parent),
    instrument(instrumentID),
    saveBarsToDB(saveBarsToDB)
{
    keys = enumValueToList<TimeFrames>(timeFrameFlags);
    std::sort(keys.begin(), keys.end(), std::greater<int>());
    for (auto key : qAsConst(keys)) {
        barMap.insert(key, StandardBar());
    }

    if (saveBarsToDB) {
        if (!createDbIfNotExists(marketDbName)) {
            this->saveBarsToDB = false;
            return;
        }

        for (auto key : qAsConst(keys)) {
            QString tableName = QString("%1_%2").arg(instrumentID, QMetaEnum::fromType<TimeFrames>().valueToKey(key));
            if (!createTblIfNotExists(marketDbName, tableName, barTableFormat)) {
                this->saveBarsToDB = false;
                break;
            }
        }
    }
}

#define MIN_UNIT    60
#define HOUR_UNIT   3600

const QHash<BarCollector::TimeFrame, int> g_time_table = {
    {BarCollector::SEC1,    1},
    {BarCollector::SEC2,    2},
    {BarCollector::SEC3,    3},
    {BarCollector::SEC4,    4},
    {BarCollector::SEC5,    5},
    {BarCollector::SEC6,    6},
    {BarCollector::SEC10,  10},
    {BarCollector::SEC12,  12},
    {BarCollector::SEC15,  15},
    {BarCollector::SEC20,  20},
    {BarCollector::SEC30,  30},
    {BarCollector::MIN1,    1 * MIN_UNIT},
    {BarCollector::MIN2,    2 * MIN_UNIT},
    {BarCollector::MIN3,    3 * MIN_UNIT},
    {BarCollector::MIN4,    4 * MIN_UNIT},
    {BarCollector::MIN5,    5 * MIN_UNIT},
    {BarCollector::MIN6,    6 * MIN_UNIT},
    {BarCollector::MIN10,  10 * MIN_UNIT},
    {BarCollector::MIN12,  12 * MIN_UNIT},
    {BarCollector::MIN15,  15 * MIN_UNIT},
    {BarCollector::MIN30,  30 * MIN_UNIT},
    {BarCollector::HOUR1,   1 * HOUR_UNIT},
    {BarCollector::HOUR2,   2 * HOUR_UNIT},
    {BarCollector::HOUR3,   3 * HOUR_UNIT},
    {BarCollector::HOUR4,   4 * HOUR_UNIT},
    {BarCollector::HOUR6,   6 * HOUR_UNIT},
    {BarCollector::HOUR8,   8 * HOUR_UNIT},
    {BarCollector::HOUR12, 12 * HOUR_UNIT},
    {BarCollector::DAY,    24 * HOUR_UNIT},
};

void BarCollector::setTradingDay(const QString &tradingDay)
{
    auto newTradingDayBase = dateToUtcTimestamp2(tradingDay);
    if (tradingDayBase != newTradingDayBase) {
        tradingDayBase = newTradingDayBase;
        lastVolume = 0;
    }
}

bool BarCollector::onMarketData(qint64 currentTime, double lastPrice, int volume)
{
    const bool isNewTick = (volume != lastVolume);

    for (auto key : qAsConst(keys)) {
        StandardBar & bar = barMap[key];
        auto timeFrameBegin = getTimeFrameBegin(currentTime, key);
        if (timeFrameBegin != bar.time) {
            saveEmitReset(key, bar);
        }

        if (!isNewTick) {
            continue;
        }

        if (bar.isEmpty()) {
            bar.time = timeFrameBegin;
            bar.open = lastPrice;
        }

        if (lastPrice > bar.high) {
            bar.high = lastPrice;
        }

        if (lastPrice < bar.low) {
            bar.low = lastPrice;
        }

        bar.close = lastPrice;
        bar.tick_volume ++;
        bar.volume += (volume - lastVolume);
    }
    lastVolume = volume;
    return isNewTick;
}

qint64 BarCollector::getTimeFrameBegin(qint64 currentTime, int timeFrame) const
{
    if (timeFrame == DAY) {
        return tradingDayBase;
    }

    if (isStockLike) {
        auto hour = currentTime / HOUR_UNIT % 24;
        if (timeFrame == HOUR1 && hour < 12) {
            return ((currentTime - 30 * MIN_UNIT) / HOUR_UNIT * HOUR_UNIT) + 30 * MIN_UNIT;
        }
        if (timeFrame == HOUR2) {
            auto currentTimeBase = currentTime / (24 * HOUR_UNIT) * 24 * HOUR_UNIT;
            return currentTimeBase + (hour < 12 ? (9 * HOUR_UNIT +  30 * MIN_UNIT) : (13 * HOUR_UNIT));
        }
    }

    auto time_unit = g_time_table[static_cast<TimeFrame>(timeFrame)];
    return (currentTime / time_unit * time_unit);
}

void BarCollector::saveEmitReset(int timeFrame, StandardBar &bar)
{
    if (!bar.isEmpty()) {
        if (saveBarsToDB) {
            QString dbTableName = QString("%1.%2_%3").arg(marketDbName, instrument, QMetaEnum::fromType<TimeFrames>().valueToKey(timeFrame));
            saveBarToDb(dbTableName, bar, 1);
        }
        emit collectedBar(instrument, timeFrame, bar);
        qInfo().noquote() << instrument << bar;
        bar.reset();
    }
}

void BarCollector::flush(bool endOfDay)
{
    for (auto key : qAsConst(keys)) {
        if ((key != DAY) || endOfDay) {
            saveEmitReset(key, barMap[key]);
        }
    }
}
