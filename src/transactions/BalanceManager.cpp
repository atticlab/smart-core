// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/BalanceManager.h"
#include "transactions/TrustLineManager.h"
#include "util/types.h"
#include "main/Application.h"
#include "ledger/AssetFrame.h"
#include "ledger/AccountFrame.h"
#include "ledger/LedgerManager.h"
#include "ledger/AssetFrame.h"
#include "ledger/TrustFrame.h"

namespace stellar
{

	using namespace std;

	BalanceManager::BalanceManager(Application& app, Database& db, LedgerDelta& delta, LedgerManager& lm, TransactionFrame& parentTx)
		: mApp(app), mDb(db), mDelta(delta), mLm(lm), mParentTx(parentTx)
	{
	}

	std::shared_ptr<StatisticsFrame::accountCounterpartyStats> BalanceManager::getUpdatedStats(AccountFrame::pointer account, AssetFrame::pointer asset, int64 amount, bool isIncome, AccountType counterpartyType, time_t timePaymentPerformed)
	{
		auto accountStats = StatisticsFrame::loadStatistics(account->getID(), asset->getAsset().asset, mDb, &mDelta);
		auto statsIt = accountStats.find(counterpartyType);
		StatisticsFrame::pointer accountCounterpartyStats;
		bool isNew = false;
		if (statsIt == accountStats.end())
		{
			isNew = true;

			LedgerEntry resultEntry;
			resultEntry.data.type(STATISTICS);
			StatisticsEntry& stats = resultEntry.data.stats();
			stats.accountID = account->getID();
			stats.asset = asset->getAsset().asset;
			stats.counterpartyType = counterpartyType;
			stats.annualIncome = 0;
			stats.annualOutcome = 0;
			stats.monthlyIncome = 0;
			stats.monthlyOutcome = 0;
			stats.dailyIncome = 0;
			stats.dailyOutcome = 0;
			stats.updatedAt = 0;

			accountCounterpartyStats = make_shared<StatisticsFrame>(resultEntry);
		}
		else {
			accountCounterpartyStats = statsIt->second;
		}

		time_t now = mLm.getCloseTime();
		int64 income = isIncome ? amount : 0;
		int64 outcome = isIncome ? 0 : amount;
		accountCounterpartyStats->clearObsolete(now);
		if (!accountCounterpartyStats->add(income, outcome, now, timePaymentPerformed))
		{
			return nullptr;
		}

		if (isNew) {
			assert(account->addNumEntries(1, mLm));
			account->storeChange(mDelta, mDb);
			accountCounterpartyStats->storeAdd(mDelta, mDb);
		}

		for (auto accountCounterparty : accountStats)
		{
			// update only if changed 
			if (accountCounterparty.first == counterpartyType || accountCounterparty.second->clearObsolete(now))
			{
				accountCounterparty.second->storeChange(mDelta, mDb);
			}
		}

		accountStats[counterpartyType] = accountCounterpartyStats;
		return make_shared<StatisticsFrame::accountCounterpartyStats>(accountStats);
	}

	bool BalanceManager::isAllowedToHoldAsset(AccountFrame::pointer account, AssetFrame::pointer asset)
	{
		// anonymous user can only hold anonymous asset
		if (account->isAnonymous())
			return asset->getAsset().isAnonymous;
		return true;
	}

	bool BalanceManager::checkInAssetLimits(TrustFrame::pointer trustLine, StatisticsFrame::accountCounterpartyStats statistics, AssetFrame::pointer asset)
	{
		// TODO implemented as required by NBU - have to implement general case
		auto assetEntry = asset->getAsset();
		if (assetEntry.maxBalance > -1 && trustLine->getBalance() > assetEntry.maxBalance)
			return false;
		return true;
	}

	bool BalanceManager::checkOutAssetLimits(AccountType counterparty, StatisticsFrame::accountCounterpartyStats statistics, AssetFrame::pointer asset)
	{
		// TODO implemented as required by NBU - have to implement general case
		// check daily and monthly out
		if (counterparty != AccountType::ACCOUNT_MERCHANT)
		{
			if (asset->getAsset().maxDailyOut > 0)
			{
				int64 dailyOut = getStatisticsForPeriod(statistics, [](StatisticsFrame::pointer stat) {
					return stat->getStatistics().dailyOutcome;
				}, {
					AccountType::ACCOUNT_ANONYMOUS_USER,
					AccountType::ACCOUNT_REGISTERED_USER,
					AccountType::ACCOUNT_SETTLEMENT_AGENT,
				});

				if (dailyOut < 0 || dailyOut > asset->getAsset().maxDailyOut)
					return false;
			}

			if (asset->getAsset().maxMonthlyOut > 0)
			{
				int64 monthlyOut = getStatisticsForPeriod(statistics, [](StatisticsFrame::pointer stat) {
					return stat->getStatistics().monthlyOutcome;
				}, {
					AccountType::ACCOUNT_ANONYMOUS_USER,
					AccountType::ACCOUNT_REGISTERED_USER,
					AccountType::ACCOUNT_SETTLEMENT_AGENT,
				});

				if (monthlyOut < 0 || monthlyOut > asset->getAsset().maxMonthlyOut)
					return false;
			}
		}

		// check annual
		if (counterparty != AccountType::ACCOUNT_SETTLEMENT_AGENT && asset->getAsset().maxAnnualOut > 0)
		{
			int64 annualOut = getStatisticsForPeriod(statistics, [](StatisticsFrame::pointer stat) {
				return stat->getStatistics().annualOutcome;
			}, {
				AccountType::ACCOUNT_ANONYMOUS_USER,
				AccountType::ACCOUNT_REGISTERED_USER,
				AccountType::ACCOUNT_MERCHANT,
			});

			if (annualOut < 0 || annualOut > asset->getAsset().maxAnnualOut)
				return false;
		}

		return true;
	}

	int64 BalanceManager::getStatisticsForPeriod(StatisticsFrame::accountCounterpartyStats statistics, std::function<int64(StatisticsFrame::pointer)> periodProvider, std::vector<AccountType> counterparties)
	{
		int64 result = 0;
		for (auto counterparty : counterparties)
		{
			auto counterpartyStat = statistics.find(counterparty);
			if (counterpartyStat == statistics.end())
				continue;
			result += periodProvider(counterpartyStat->second);
			if (result < 0)
				return -1;
		}

		return result;
	}

	bool BalanceManager::checkAssetLimits(AccountFrame::pointer account, TrustFrame::pointer trustLine, StatisticsFrame::accountCounterpartyStats statistics, AssetFrame::pointer asset, bool isIncome, AccountType counterpartyType)
	{
		// asset limits are for users
		auto accountType = account->getAccount().accountType;
		if (accountType != AccountType::ACCOUNT_ANONYMOUS_USER && accountType != AccountType::ACCOUNT_REGISTERED_USER && accountType != AccountType::ACCOUNT_SCRATCH_CARD)
			return true;
		
		if (isIncome)
			return checkInAssetLimits(trustLine, statistics, asset);
		return checkOutAssetLimits(counterpartyType, statistics, asset);
	}

	BalanceManager::Result BalanceManager::add(AccountFrame::pointer account, Asset const& asset, int64 amount, bool isIncome, AccountType counterpartyType, time_t timePaymentPerformed)
	{	
		// check if asset allowed
		AssetFrame::pointer assetFrame = AssetFrame::loadAsset(asset, mDb, &mDelta);
		if (!assetFrame)
			return ASSET_NOT_ALLOWED;

		// check if allowed to hold
		if (!isAllowedToHoldAsset(account, assetFrame))
			return NOT_AUTHORIZED;

		int64 balanceDelta = isIncome ? amount : -amount;
		TrustFrame::pointer trustLine = TrustFrame::loadTrustLine(account->getID(), asset, mDb, &mDelta);
		if (!trustLine)
		{
			// tring to send money from nonexisting trust line
			if (balanceDelta < 0 && !(account->getID() == getIssuer(asset)))
				return NO_TRUST_LINE;
			// create trust line
			TrustLineManager trustLineManager(mApp, mDb, mDelta, mLm, mParentTx);
			trustLine = trustLineManager.createTrustLine(account, asset);
			// check if create
			if (!trustLine)
				return NO_TRUST_LINE;
		}

		if (!trustLine->isAuthorized())
			return NOT_AUTHORIZED;

		if (!trustLine->addBalance(balanceDelta))
		{
			return balanceDelta < 0 ? UNDERFUNDED : LINE_FULL;
		}

		auto updatedStats = getUpdatedStats(account, assetFrame, amount, isIncome, counterpartyType, timePaymentPerformed);
		if (!updatedStats)
		{
			return STATS_OVERFLOW;
		}

		if (!checkAssetLimits(account, trustLine, *updatedStats, assetFrame, isIncome, counterpartyType))
			return ASSET_LIMITS_EXCEEDED;

		trustLine->storeChange(mDelta, mDb);
		return SUCCESS;
	}
}
