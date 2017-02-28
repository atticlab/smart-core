// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/StatisticsFrame.h"
#include "database/Database.h"
#include "crypto/SecretKey.h"
#include "crypto/SHA.h"
#include "LedgerDelta.h"
#include "util/types.h"
#include <time.h>

using namespace std;
using namespace soci;

namespace stellar
{
const char* StatisticsFrame::kSQLCreateStatement1 =
    "CREATE TABLE statistics"
    "("
	"account_id   VARCHAR(56) NOT NULL,"
	"asset_issuer VARCHAR(56) NOT NULL,"
	"asset_code   VARCHAR(12) NOT NULL,"
    "asset_type   INT NOT NULL,"
	"counterparty INT NOT NULL,"
    "daily_in     BIGINT NOT NULL,"
	"daily_out    BIGINT NOT NULL,"
	"monthly_in	  BIGINT NOT NULL,"
	"monthly_out  BIGINT NOT NULL,"
	"annual_in	  BIGINT NOT NULL,"
	"annual_out	  BIGINT NOT NULL,"
	"updated_at   BIGINT NOT NULL,"
    "lastmodified INT NOT NULL,"
    "PRIMARY KEY  (account_id, asset_issuer, asset_code, counterparty)"
    ");";

static const char* statisticsColumnSelector =
    "SELECT account_id, asset_issuer, asset_code, asset_type, counterparty, daily_in, daily_out, monthly_in, monthly_out, annual_in, annual_out, updated_at, lastmodified FROM statistics";

StatisticsFrame::StatisticsFrame() : EntryFrame(STATISTICS), mStatistics(mEntry.data.stats())
{
}

StatisticsFrame::StatisticsFrame(LedgerEntry const& from)
    : EntryFrame(from), mStatistics(mEntry.data.stats())
{
}

StatisticsFrame::StatisticsFrame(StatisticsFrame const& from) : StatisticsFrame(from.mEntry)
{
}

StatisticsFrame& StatisticsFrame::operator=(StatisticsFrame const& other)
{
    if (&other != this)
    {
        mStatistics = other.mStatistics;
        mKey = other.mKey;
        mKeyCalculated = other.mKeyCalculated;
    }
    return *this;
}

bool
StatisticsFrame::isValid(StatisticsEntry const& se)
{
	bool res = isAssetValid(se.asset);
	res = res && (se.dailyIncome >= 0) && (se.dailyOutcome >= 0);
	res = res && (se.monthlyIncome >= se.dailyIncome) && (se.monthlyOutcome >= se.dailyOutcome);
	res = res && (se.annualIncome >= se.monthlyIncome) && (se.annualOutcome >= se.annualOutcome);
	return res;
}

bool
StatisticsFrame::isValid() const
{
    return isValid(mStatistics);
}

StatisticsFrame::pointer StatisticsFrame::loadStatistics(AccountID const& accountID, Asset const& asset, AccountType rawCounterparty, Database& db, LedgerDelta* delta)
{
	std::string strAccountID = PubKeyUtils::toStrKey(accountID);
	std::string assetIssuer = PubKeyUtils::toStrKey(getIssuer(asset));
	std::string assetCode = getCode(asset);
	unsigned int counterparty = rawCounterparty;

	std::string sql = statisticsColumnSelector;
	sql += " WHERE account_id = :id AND asset_issuer = :is AND asset_code = :ac AND counterparty= :cp";
	auto prep = db.getPreparedStatement(sql);
	auto& st = prep.statement();
	st.exchange(use(strAccountID));
	st.exchange(use(assetIssuer));
	st.exchange(use(assetCode));
	st.exchange(use(counterparty));

	auto timer = db.getSelectTimer("statistics");
	pointer retStatistics;
	loadStatistics(prep, [&retStatistics](LedgerEntry const& statistics)
	{
		retStatistics = std::make_shared<StatisticsFrame>(statistics);
	});

	if (delta && retStatistics)
	{
		delta->recordEntry(*retStatistics);
	}

	return retStatistics;
}

std::unordered_map<AccountType, StatisticsFrame::pointer>
StatisticsFrame::loadStatistics(AccountID const& accountID, Asset const& asset, Database& db, LedgerDelta* delta)
{
	std::string strAccountID = PubKeyUtils::toStrKey(accountID);
    std::string assetIssuer = PubKeyUtils::toStrKey(getIssuer(asset));
	std::string assetCode = getCode(asset);

    std::string sql = statisticsColumnSelector;
    sql += " WHERE account_id = :id AND asset_issuer = :is AND asset_code = :ac";
    auto prep = db.getPreparedStatement(sql);
    auto& st = prep.statement();
    st.exchange(use(strAccountID));
    st.exchange(use(assetIssuer));
	st.exchange(use(assetCode));

    auto timer = db.getSelectTimer("statistics");
	std::unordered_map<AccountType, StatisticsFrame::pointer> retStatistics;
    loadStatistics(prep, [&retStatistics](LedgerEntry const& statistics)
               {
                   retStatistics[statistics.data.stats().counterpartyType] = std::make_shared<StatisticsFrame>(statistics);
               });

    if (delta)
    {
		for (auto kv : retStatistics)
		{
			delta->recordEntry(*kv.second);
		}
    }

    return retStatistics;
}

void
StatisticsFrame::loadStatistics(StatementContext& prep,
                       std::function<void(LedgerEntry const&)> statisticsProcessor)
{
    unsigned int assetType, counterpartyType;
	std::string accountID, assetCode, assetIssuer;

    LedgerEntry le;
    le.data.type(STATISTICS);
    StatisticsEntry& se = le.data.stats();

    statement& st = prep.statement();
    st.exchange(into(accountID));
    st.exchange(into(assetIssuer));
    st.exchange(into(assetCode));
	st.exchange(into(assetType));
    st.exchange(into(counterpartyType));

	st.exchange(into(se.dailyIncome));
	st.exchange(into(se.dailyOutcome));
	st.exchange(into(se.monthlyIncome));
	st.exchange(into(se.monthlyOutcome));
	st.exchange(into(se.annualIncome));
	st.exchange(into(se.annualOutcome));

	st.exchange(into(se.updatedAt));
    st.exchange(into(le.lastModifiedLedgerSeq));
    st.define_and_bind();
    st.execute(true);
    while (st.got_data())
    {
		se.accountID = PubKeyUtils::fromStrKey(accountID);

        if (assetType > ASSET_TYPE_CREDIT_ALPHANUM12 || assetType == ASSET_TYPE_NATIVE)
            throw std::runtime_error("bad statistics asset type");

		se.asset.type((AssetType)assetType);
		if (assetType == ASSET_TYPE_CREDIT_ALPHANUM12)
		{
			se.asset.alphaNum12().issuer = PubKeyUtils::fromStrKey(assetIssuer);
			strToAssetCode(se.asset.alphaNum12().assetCode, assetCode);
		}
		else if (assetType == ASSET_TYPE_CREDIT_ALPHANUM4)
		{
			se.asset.alphaNum4().issuer = PubKeyUtils::fromStrKey(assetIssuer);
			strToAssetCode(se.asset.alphaNum4().assetCode, assetCode);
		}

		se.counterpartyType = (AccountType)counterpartyType;

        if (!isValid(se))
        {
            throw std::runtime_error("Invalid statistics");
        }

        statisticsProcessor(le);
        st.fetch();
    }
}

void
StatisticsFrame::loadStatistics(AccountID const& accountID,
                       std::vector<StatisticsFrame::pointer>& retStatistics,
                       Database& db)
{
    std::string strAccountID = PubKeyUtils::toStrKey(accountID);

    std::string sql = statisticsColumnSelector;
    sql += " WHERE account_id = :id";
    auto prep = db.getPreparedStatement(sql);
    auto& st = prep.statement();
    st.exchange(use(strAccountID));

    auto timer = db.getSelectTimer("statistics");
    loadStatistics(prep, [&retStatistics](LedgerEntry const& of)
               {
                   retStatistics.emplace_back(make_shared<StatisticsFrame>(of));
               });
}

std::unordered_map<AccountID, std::vector<StatisticsFrame::pointer>>
StatisticsFrame::loadAllStatistics(Database& db)
{
    std::unordered_map<AccountID, std::vector<StatisticsFrame::pointer>> retStatistics;
    std::string sql = statisticsColumnSelector;
    sql += " ORDER BY account_id";
    auto prep = db.getPreparedStatement(sql);

    auto timer = db.getSelectTimer("statistics");
    loadStatistics(prep, [&retStatistics](LedgerEntry const& as)
               {
                   auto& thisUserStatistics = retStatistics[as.data.stats().accountID];
                   thisUserStatistics.emplace_back(make_shared<StatisticsFrame>(as));
               });
    return retStatistics;
}

bool
StatisticsFrame::exists(Database& db, LedgerKey const& key)
{
	std::string strAccountID = PubKeyUtils::toStrKey(key.stats().accountID);
	std::string assetIssuer = PubKeyUtils::toStrKey(getIssuer(key.stats().asset));
	std::string assetCode = getCode(key.stats().asset);
	unsigned int counterparty = key.stats().counterpartyType;
    int exists = 0;
    auto timer = db.getSelectTimer("statistics-exists");
    auto prep =
        db.getPreparedStatement("SELECT EXISTS (SELECT NULL FROM statistics "
                                "WHERE account_id=:id AND asset_issuer=:ai AND asset_code=:ac AND counterparty=:cp)");
    auto& st = prep.statement();
    st.exchange(use(strAccountID));
    st.exchange(use(assetIssuer));
	st.exchange(use(assetCode));
	st.exchange(use(counterparty));
    st.exchange(into(exists));
    st.define_and_bind();
    st.execute(true);
    return exists != 0;
}

uint64_t
StatisticsFrame::countObjects(soci::session& sess)
{
    uint64_t count = 0;
    sess << "SELECT COUNT(*) FROM statistics;", into(count);
    return count;
}

void
StatisticsFrame::storeDelete(LedgerDelta& delta, Database& db) const
{
    storeDelete(delta, db, getKey());
}

void
StatisticsFrame::storeDelete(LedgerDelta& delta, Database& db, LedgerKey const& key)
{
    auto timer = db.getDeleteTimer("statistics");
    auto prep = db.getPreparedStatement("DELETE FROM statistics WHERE account_id=:id AND asset_issuer=:ai AND asset_code=:ac AND counterparty=:cp");
    auto& st = prep.statement();
	std::string strAccountID = PubKeyUtils::toStrKey(key.stats().accountID);
	std::string assetIssuer = PubKeyUtils::toStrKey(getIssuer(key.stats().asset));
	std::string assetCode = getCode(key.stats().asset);
	unsigned int counterparty = key.stats().counterpartyType;
    st.exchange(use(strAccountID));
	st.exchange(use(assetIssuer));
	st.exchange(use(assetCode));
	st.exchange(use(counterparty));
    st.define_and_bind();
    st.execute(true);
    delta.deleteEntry(key);
}

void
StatisticsFrame::storeChange(LedgerDelta& delta, Database& db)
{
    storeUpdateHelper(delta, db, false);
}

void
StatisticsFrame::storeAdd(LedgerDelta& delta, Database& db)
{
    storeUpdateHelper(delta, db, true);
}

void
StatisticsFrame::storeUpdateHelper(LedgerDelta& delta, Database& db, bool insert)
{
    touch(delta);

    if (!isValid())
    {
        throw std::runtime_error("Invalid statistics");
    }

	std::string strAccountID = PubKeyUtils::toStrKey(mStatistics.accountID);
	std::string assetIssuer = PubKeyUtils::toStrKey(getIssuer(mStatistics.asset));
	std::string assetCode = getCode(mStatistics.asset);
	unsigned int counterparty = mStatistics.counterpartyType;
	unsigned int assetType = mStatistics.asset.type();

    string sql;

    if (insert)
    {
		//  
        sql = "INSERT INTO statistics (account_id, asset_issuer, asset_code, asset_type, counterparty, daily_in, daily_out, "
			  "monthly_in, monthly_out, annual_in, annual_out, updated_at, lastmodified) VALUES "
              "(:aid, :ai, :ac, :at, :cp, :d_in, :d_out, :m_in, :m_out, :a_in, :a_out, :up, :lm)";
    }
    else
    {
        sql = "UPDATE statistics SET asset_type = :at, daily_in = :d_in, daily_out = :d_out, "
			"monthly_in = :m_in, monthly_out = :m_out, annual_in = :a_in, annual_out = :a_out, updated_at = :up, lastmodified = :lm "
			"WHERE account_id=:aid AND asset_issuer =:ai AND asset_code = :ac AND counterparty = :cp";
    }

    auto prep = db.getPreparedStatement(sql);
    auto& st = prep.statement();

    st.exchange(use(strAccountID, "aid"));
    st.exchange(use(assetIssuer, "ai"));
	st.exchange(use(assetCode, "ac"));
    st.exchange(use(assetType, "at"));
	st.exchange(use(counterparty, "cp"));
	st.exchange(use(mStatistics.dailyIncome, "d_in"));
	st.exchange(use(mStatistics.dailyOutcome, "d_out"));
	st.exchange(use(mStatistics.monthlyIncome, "m_in"));
	st.exchange(use(mStatistics.monthlyOutcome, "m_out"));
	st.exchange(use(mStatistics.annualIncome, "a_in"));
	st.exchange(use(mStatistics.annualOutcome, "a_out"));
	st.exchange(use(mStatistics.updatedAt, "up"));
    st.exchange(use(getLastModified(), "lm"));
    st.define_and_bind();

    auto timer =
        insert ? db.getInsertTimer("statistics") : db.getUpdateTimer("statistics");
    st.execute(true);

    if (st.get_affected_rows() != 1)
    {
        throw std::runtime_error("could not update SQL");
    }

    if (insert)
    {
        delta.addEntry(*this);
    }
    else
    {
        delta.modEntry(*this);
    }
}

void StatisticsFrame::clearObsolete(time_t rawCurrentTime)
{
	struct tm* currentTime = std::localtime(&rawCurrentTime);
	struct tm* timeUpdated = std::localtime(&mStatistics.updatedAt);
	bool isYear = timeUpdated->tm_year < currentTime->tm_year;
	if (isYear) 
	{
		mStatistics.annualIncome = 0;
		mStatistics.annualOutcome = 0;
	}

	bool isMonth = isYear || timeUpdated->tm_mon < currentTime->tm_mon;
	if (isMonth)
	{
		mStatistics.monthlyIncome = 0;
		mStatistics.monthlyOutcome = 0;
	}

	bool isDay = isMonth || timeUpdated->tm_yday < currentTime->tm_yday;
	if (isDay)
	{
		mStatistics.dailyIncome = 0;
		mStatistics.dailyOutcome = 0;
	}
}

bool StatisticsFrame::add(int64 income, int64 outcome, time_t rawCurrentTime, time_t rawTimePerformed)
{
	struct tm* currentTime = std::localtime(&rawCurrentTime);
	struct tm* timePerformed = std::localtime(&rawTimePerformed);
	if (currentTime->tm_year != timePerformed->tm_year)
	{
		return true;
	}
	mStatistics.annualIncome += income;
	mStatistics.annualOutcome += outcome;
	if (mStatistics.annualIncome < 0 || mStatistics.annualOutcome < 0)
	{
		return false;
	}

	if (currentTime->tm_mon != timePerformed->tm_mon)
	{
		return true;
	}
	mStatistics.monthlyIncome += income;
	mStatistics.monthlyOutcome += outcome;
	if (mStatistics.monthlyIncome < 0 || mStatistics.monthlyOutcome < 0)
	{
		return false;
	}

	if (currentTime->tm_yday != timePerformed->tm_yday)
	{
		return true;
	}
	mStatistics.dailyIncome += income;
	mStatistics.dailyOutcome += outcome;
	return mStatistics.dailyIncome >= 0 && mStatistics.dailyOutcome >= 0;
}

void
StatisticsFrame::dropAll(Database& db)
{
    db.getSession() << "DROP TABLE IF EXISTS statistics;";
    db.getSession() << kSQLCreateStatement1;
}
}
