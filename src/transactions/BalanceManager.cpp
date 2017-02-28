// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/BalanceManager.h"
#include "util/types.h"
#include "main/Application.h"
#include "ledger/AssetFrame.h"
#include "ledger/AccountFrame.h"
#include "ledger/LedgerManager.h"

namespace stellar
{

	using namespace std;

	BalanceManager::BalanceManager(Application& app, Database& db, LedgerDelta& delta, LedgerManager& lm)
		: mApp(app), mDb(db), mDelta(delta), mLm(lm)
	{
	}

	std::pair<StatisticsFrame::pointer, bool> BalanceManager::getOrCreateStats(std::unordered_map<AccountType, StatisticsFrame::pointer> statsMap, TrustFrame::pointer trustLineFrame, AccountType counterparty)
	{
		auto statsIt = statsMap.find(counterparty);
		if (statsIt != statsMap.end())
		{
			return std::pair<StatisticsFrame::pointer, bool>(statsIt->second, false);
		}

		LedgerEntry resultEntry;
		resultEntry.data.type(STATISTICS);
		StatisticsEntry& stats = resultEntry.data.stats();
		stats.accountID = trustLineFrame->getTrustLine().accountID;
		stats.asset = trustLineFrame->getTrustLine().asset;
		stats.counterpartyType = counterparty;
		stats.annualIncome = 0;
		stats.annualOutcome = 0;
		stats.monthlyIncome = 0;
		stats.monthlyOutcome = 0;
		stats.dailyIncome = 0;
		stats.dailyOutcome = 0;
		stats.updatedAt = 0;
		
		return std::pair<StatisticsFrame::pointer, bool>(make_shared<StatisticsFrame>(resultEntry), true);
	}

	BalanceManager::Result BalanceManager::add(AccountFrame::pointer account, TrustFrame::pointer trustLineFrame, int64 amount, bool isIncome, AccountType counterparty, time_t timePerformed, time_t now)
	{
		int64 balanceDelta = isIncome ? amount : -amount;
		if (!trustLineFrame->addBalance(balanceDelta))
		{
			return balanceDelta < 0 ? UNDERFUNDED : LINE_FULL;
		}

		auto tl = trustLineFrame->getTrustLine();
		auto stats = StatisticsFrame::loadStatistics(tl.accountID, tl.asset, mDb, &mDelta);
		auto counterpartyStats = getOrCreateStats(stats, trustLineFrame, counterparty);
		
		int64 income = isIncome ? amount : 0;
		int64 outcome = isIncome ? 0 : amount;
		if (!counterpartyStats.first->add(income, outcome, now, timePerformed))
		{
			return STATS_OVERFLOW;
		}

		if (counterpartyStats.second) {
			assert(account->addNumEntries(1, mLm));
			account->storeChange(mDelta, mDb);
			counterpartyStats.first->storeAdd(mDelta, mDb);
		}
		else {
			counterpartyStats.first->storeChange(mDelta, mDb);
		}

		return SUCCESS;
	}
}
