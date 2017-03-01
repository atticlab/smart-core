#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "overlay/StellarXDR.h"
#include "ledger/AssetFrame.h"

namespace medida
{
	class MetricsRegistry;
}

namespace stellar
{
	class Application;
	class Database;

	class AssetsValidator
	{
	protected:
		Application& mApp;
		Database& mDb;

	public:
		AssetsValidator(Application& app, Database& db);

		// returns true if the Asset value is well formed and issued by bankID
		bool isAssetValid(Asset const& asset) const;
		// returns true if the asset value is valid and asset is allowed (stored in db)
		bool isAssetAllowed(Asset const& asset) const;
		// return AssetFrame if asset is allowed
		AssetFrame::pointer getAllowedAsset(Asset const& asset, LedgerDelta* delta = nullptr) const;
	};
}
