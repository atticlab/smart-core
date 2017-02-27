// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/asio.h"
#include "transactions/ManageAssetOpFrame.h"
#include "transactions/AssetsValidator.h"
#include "ledger/AssetFrame.h"
#include "util/Logging.h"
#include "ledger/LedgerDelta.h"
#include "database/Database.h"
#include "medida/meter.h"
#include "medida/metrics_registry.h"
#include "main/Application.h"
#include <algorithm>

namespace stellar
{

using namespace std;
using xdr::operator==;

ManageAssetOpFrame::ManageAssetOpFrame(Operation const& op, OperationResult& res, OperationFee* fee,
                               TransactionFrame& parentTx)
    : OperationFrame(op, res, fee, parentTx), mManageAsset(mOperation.body.manageAssetOp())
{
}

bool ManageAssetOpFrame::manageAsset(Application& app, LedgerDelta& delta, LedgerManager& ledgerManager)
{
	Database& db = ledgerManager.getDatabase();
	AssetsValidator assetsValidator(app, db);
	if (!assetsValidator.isAssetValid(mManageAsset.asset))
	{
		app.getMetrics().NewMeter({ "op-manage-asset", "invalid", "malformed-invalid-asset" },
			"operation").Mark();
		innerResult().code(MANAGE_ASSET_INVALID_ISSUER);
		return false;
	}

	AssetFrame::pointer storedAsset = AssetFrame::loadAsset(mManageAsset.asset, db, &delta);
	bool isNew = false;
	if (!storedAsset)
	{
		if (mManageAsset.isDelete)
		{
			app.getMetrics().NewMeter({ "op-manage-asset", "invalid", "does-not-exist" },
				"operation").Mark();
			innerResult().code(MANAGE_ASSET_NOT_EXIST);
			return false;
		}

		isNew = true;
		storedAsset = std::make_shared<AssetFrame>();
		auto& asset = storedAsset->getAsset();
		asset.asset = mManageAsset.asset;
	}

	if (mManageAsset.isDelete)
	{
		storedAsset->storeDelete(delta, db);
		mSourceAccount->addNumEntries(-1, ledgerManager);
		mSourceAccount->storeChange(delta, db);
		return true;
	}

	storedAsset->getAsset().isAnonymous = mManageAsset.isAnonymous;

	if (!isNew)
	{
		storedAsset->storeChange(delta, db);
		return true;
	}

	if (!mSourceAccount->addNumEntries(1, ledgerManager))
	{
		app.getMetrics().NewMeter({ "op-manage-asset", "invalid", "low-reserve" },
			"operation").Mark();
		innerResult().code(MANAGE_ASSET_LOW_RESERVE);
		return false;
	}
	mSourceAccount->storeChange(delta, db);
	storedAsset->storeAdd(delta, db);
	return true;
}

bool
ManageAssetOpFrame::doApply(Application& app, LedgerDelta& delta,
                        LedgerManager& ledgerManager)
{
	if (manageAsset(app, delta, ledgerManager))
	{
		app.getMetrics().NewMeter({ "op-manage-asset", "success", "apply" }, "operation").Mark();
		innerResult().code(MANAGE_ASSET_SUCCESS);
		return true;
	}

	return false;
}

bool
ManageAssetOpFrame::doCheckValid(Application& app)
{	
	if (!(getSourceID() == app.getConfig().BANK_MASTER_KEY)) {
		app.getMetrics().NewMeter({ "op-manage-asset", "invalid", "bank-is-not-source" },
			"operation").Mark();
		innerResult().code(MANAGE_ASSET_NOT_AUTHORIZED);
		return false;
	}

	bool isAllAdmins = !mUsedSigners.empty();
	for (auto& signer : mUsedSigners) {
		if (signer.signerType != SIGNER_ADMIN) {
			isAllAdmins = false;
			break;
		}
	}

	if (!isAllAdmins) {
		app.getMetrics().NewMeter({ "op-manage-asset", "invalid", "signers-are-not-admins" },
			"operation").Mark();
		innerResult().code(MANAGE_ASSET_NOT_AUTHORIZED);
		return false;
	}

	return true;
}
    
}