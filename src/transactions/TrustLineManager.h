#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "overlay/StellarXDR.h"
#include "transactions/TransactionFrame.h"
#include "ledger/TrustFrame.h"

namespace medida
{
	class MetricsRegistry;
}

namespace stellar
{
	class Application;
	class Database;

	class TrustLineManager
	{
	protected:
		Application& mApp;
		Database& mDb;
		LedgerDelta& mDelta;
		LedgerManager& mLm;
		TransactionFrame& mParentTx;

	public:
		TrustLineManager(Application& app, Database& db, LedgerDelta& delta, LedgerManager& lm, TransactionFrame& parentTx);
		//tries to create trust line, returns nill if failed
		TrustFrame::pointer createTrustLine(AccountFrame::pointer account, Asset const& asset);
	};
}
