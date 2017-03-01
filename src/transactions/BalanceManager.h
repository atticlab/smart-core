#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "overlay/StellarXDR.h"
#include "transactions/TransactionFrame.h"
#include "ledger/TrustFrame.h"
#include "ledger/StatisticsFrame.h"
#include "ledger/AssetFrame.h"
#include <functional>

namespace medida
{
	class MetricsRegistry;
}

namespace stellar
{
	class Application;
	class Database;

	class BalanceManager
	{
	protected:
		Application& mApp;
		Database& mDb;
		LedgerDelta& mDelta;
		LedgerManager& mLm;
		TransactionFrame& mParentTx;

		bool isAllowedToHoldAsset(AccountFrame::pointer account, AssetFrame::pointer asset);

	private:
		// returns updated statistics
		std::shared_ptr<StatisticsFrame::accountCounterpartyStats> getUpdatedStats(AccountFrame::pointer account, AssetFrame::pointer asset, int64 amount, bool isIncome, AccountType counterpartyType, time_t timePaymentPerformed);
		// returns true, if trustline and statistics do not exceed limits
		bool checkAssetLimits(AccountFrame::pointer account, TrustFrame::pointer trustLine, StatisticsFrame::accountCounterpartyStats statistics, AssetFrame::pointer asset, bool isIncome, AccountType counterpartyType);
		bool checkInAssetLimits(TrustFrame::pointer trustLine, StatisticsFrame::accountCounterpartyStats statistics, AssetFrame::pointer asset);
		bool checkOutAssetLimits(AccountType counterparty, StatisticsFrame::accountCounterpartyStats statistics, AssetFrame::pointer asset);
		// returns -1 if overflow
		int64 getStatisticsForPeriod(StatisticsFrame::accountCounterpartyStats statistics, std::function<int64(StatisticsFrame::pointer)> periodProvider, std::vector<AccountType> counterparties);
	public:
		enum Result {
			SUCCESS,

			ASSET_NOT_ALLOWED,
			NOT_AUTHORIZED,
			NO_TRUST_LINE,
			LINE_FULL,
			UNDERFUNDED,
			ASSET_LIMITS_EXCEEDED,
			STATS_OVERFLOW
		};

		BalanceManager(Application& app, Database& db, LedgerDelta& delta, LedgerManager& lm, TransactionFrame& parentTx);

		Result add(AccountFrame::pointer account, Asset const& asset, int64 amount, bool isIncome, AccountType counterpartyType, time_t timePaymentPerformed);
	};
}
