// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "AssetsValidator.h"
#include "util/types.h"
#include "main/Application.h"
#include "ledger/AssetFrame.h"
#include "ledger/LedgerManager.h"

namespace stellar
{

	using namespace std;

	AssetsValidator::AssetsValidator(Application& app, Database& db)
		: mApp(app), mDb(db)
	{
	}

	bool AssetsValidator::isAssetValid(Asset const& asset) const
	{
		return asset.type() != ASSET_TYPE_NATIVE && stellar::isAssetValid(asset) && getIssuer(asset) == mApp.getIssuer();
	}

	bool AssetsValidator::isAssetAllowed(Asset const& asset) const
	{
		if (!isAssetValid(asset)) {
			return false;
		}

		LedgerKey key;
		key.type(ASSET);
		key.asset().asset = asset;
		return AssetFrame::exists(mDb, key);
	}

	AssetFrame::pointer AssetsValidator::getAllowedAsset(Asset const& asset, LedgerDelta* delta) const
	{
		if (!isAssetValid(asset))
			return nullptr;
		return AssetFrame::loadAsset(asset, mDb, delta);
	}
}
