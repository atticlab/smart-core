// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "transactions/TrustLineManager.h"
#include "transactions/ChangeTrustOpFrame.h"
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

	TrustLineManager::TrustLineManager(Application& app, Database& db, LedgerDelta& delta, LedgerManager& lm, TransactionFrame& parentTx)
		: mApp(app), mDb(db), mDelta(delta), mLm(lm), mParentTx(parentTx)
	{
	}

	TrustFrame::pointer TrustLineManager::createTrustLine(AccountFrame::pointer account, Asset const& asset)
	{
		// build a changeTrustOp
		Operation op;
		op.sourceAccount.activate() = account->getID();
		op.body.type(CHANGE_TRUST);
		ChangeTrustOp& caOp = op.body.changeTrustOp();
		caOp.limit = INT64_MAX;
		caOp.line = asset;

		OperationResult opRes;
		opRes.code(opINNER);
		opRes.tr().type(CHANGE_TRUST);

		//no need to take fee twice
		OperationFee fee;
		fee.type(OperationFeeType::opFEE_NONE);

		ChangeTrustOpFrame changeTrust(op, opRes, &fee, mParentTx);
		changeTrust.setSourceAccountPtr(account);

		// create trust line
		if (!changeTrust.doCheckValid(mApp) ||
			!changeTrust.doApply(mApp, mDelta, mLm))
		{
			if (changeTrust.getResultCode() != opINNER)
			{
				throw std::runtime_error("Unexpected error code from changeTrust");
			}
			switch (ChangeTrustOpFrame::getInnerCode(changeTrust.getResult()))
			{
			case CHANGE_TRUST_NO_ISSUER:
			case CHANGE_TRUST_LOW_RESERVE:
			case CHANGE_TRUST_ASSET_NOT_ALLOWED:
			case CHANGE_TRUST_NOT_AUTHORIZED:
				return nullptr;
			case CHANGE_TRUST_MALFORMED:
				throw std::runtime_error("Failed to create trust line - change trust line op is malformed");
			case CHANGE_TRUST_INVALID_LIMIT:
				throw std::runtime_error("Failed to create trust line - invalid limit");
			default:
				throw std::runtime_error("Unexpected error code from change trust line");
			}
		}
		return changeTrust.getTrustLine();
	}
}
