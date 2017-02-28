#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "overlay/StellarXDR.h"
#include "ledger/TrustFrame.h"
#include "ledger/StatisticsFrame.h"

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

	private:
		// returns stats and true - if created
		std::pair<StatisticsFrame::pointer, bool> getOrCreateStats(std::unordered_map<AccountType, StatisticsFrame::pointer> stats, TrustFrame::pointer trustLineFrame, AccountType counterparty);

	public:
		enum Result {SUCCESS, LINE_FULL, UNDERFUNDED, STATS_OVERFLOW};

		BalanceManager(Application& app, Database& db, LedgerDelta& delta, LedgerManager& lm);

		Result add(AccountFrame::pointer account, TrustFrame::pointer trustLine, int64 amount, bool isIncome, AccountType counterparty, time_t timePerformed, time_t now);
	};
}
