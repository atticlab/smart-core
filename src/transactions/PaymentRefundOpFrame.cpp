// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/asio.h"
#include "transactions/PaymentRefundOpFrame.h"
#include "transactions/PathPaymentOpFrame.h"
#include "util/Logging.h"
#include "ledger/LedgerDelta.h"
#include "ledger/TrustFrame.h"
#include "ledger/RefundedPaymentFrame.h"
#include "database/Database.h"
#include "medida/meter.h"
#include "medida/metrics_registry.h"
#include "main/Application.h"
#include <algorithm>

namespace stellar
{

using namespace std;
using xdr::operator==;

PaymentRefundOpFrame::PaymentRefundOpFrame(Operation const& op, OperationResult& res, OperationFee* fee,
                               TransactionFrame& parentTx)
    : OperationFrame(op, res, fee, parentTx), mRefund(mOperation.body.prefundOp())
{
}

bool PaymentRefundOpFrame::checkAllowed() {
	return true;
}

bool PaymentRefundOpFrame::checkAlreadyRefunded(LedgerDelta& delta, Database& db) {
	// check if already reversed
	LedgerKey key;
	key.type(REFUNDED_PAYMENT);
	key.refundedPayment().rID = mRefund.paymentID;
    
    RefundedPaymentFrame::pointer refund = RefundedPaymentFrame::loadRefundedPayment(db,key);
    if (refund == nullptr){
        return false;
    }
    if (refund->getRefundedPayment().refundedAmount + mRefund.amount <= refund->getRefundedPayment().totalOriginalAmount)
        return false;
    return true;
//	auto alreadyRefunded ;
//	if (alreadyReversed) {
//		return false;
//	}
//
//	auto reversedPayment = make_shared<RefundedPaymentFrame>();
//	reversedPayment->getReversedPayment().rID = mRefund.paymentID;
//	reversedPayment->storeAdd(delta, db);
//	return true;
}

bool
PaymentRefundOpFrame::doApply(Application& app, LedgerDelta& delta,
                        LedgerManager& ledgerManager)
{
	if (!checkAllowed()) {
		app.getMetrics().NewMeter({ "op-refund-payment", "failure", "not-allowed" },
			"operation").Mark();
		innerResult().code(REFUND_NOT_ALLOWED);
		return false;
	}

	Database& db = ledgerManager.getDatabase();
    LedgerKey key;
    key.type(REFUNDED_PAYMENT);
    key.refundedPayment().rID = mRefund.paymentID;
    
    RefundedPaymentFrame::pointer refundFrame = RefundedPaymentFrame::loadRefundedPayment(db, key);
    
	if (refundFrame)
    {
        auto& rp = refundFrame->getRefundedPayment();
        if (rp.refundedAmount + mRefund.amount <= rp.totalOriginalAmount)
        {
            app.getMetrics().NewMeter({ "op-refund-payment", "failure",
                "already-refunded" }, "operation").Mark();
            innerResult().code(REFUND_ALREADY_REFUNDED);
            return false;
        }
        rp.refundedAmount += mRefund.amount;
        refundFrame->storeChange(delta, db);
    }
    else
    {
        refundFrame = make_shared<RefundedPaymentFrame>();
        auto& rf = refundFrame->getRefundedPayment();
        rf.rID = mRefund.paymentID;
        rf.refundedAmount = mRefund.amount;
        rf.totalOriginalAmount = mRefund.originalAmount;
        rf.asset = mRefund.asset;
        refundFrame->storeAdd(delta, db);
    }
    // build a pathPaymentOp
    Operation op;
    op.sourceAccount = mOperation.sourceAccount;
    op.body.type(PATH_PAYMENT);
    PathPaymentOp& ppOp = op.body.pathPaymentOp();
    ppOp.sendAsset = mRefund.asset;
    ppOp.destAsset = mRefund.asset;
    
    ppOp.destAmount = mRefund.amount;
    ppOp.sendMax = mRefund.amount;
    
    ppOp.destination = mRefund.paymentSource;
    
    OperationResult opRes;
    opRes.code(opINNER);
    opRes.tr().type(PATH_PAYMENT);
    PathPaymentOpFrame ppayment(op, opRes, mFee, mParentTx);
    ppayment.setSourceAccountPtr(mSourceAccount);
    
    if (!ppayment.doCheckValid(app) ||
        !ppayment.doApply(app, delta, ledgerManager))
    {
        if (ppayment.getResultCode() != opINNER)
        {
            throw std::runtime_error("Unexpected error code from pathPayment");
        }
        RefundResultCode res;
        
        switch (PathPaymentOpFrame::getInnerCode(ppayment.getResult()))
        {
            case PATH_PAYMENT_UNDERFUNDED:
                app.getMetrics().NewMeter({"op-payment", "failure", "underfunded"},
                                          "operation").Mark();
                res = PAYMENT_UNDERFUNDED;
                break;
            case PATH_PAYMENT_SRC_NOT_AUTHORIZED:
                app.getMetrics().NewMeter({"op-payment", "failure", "src-not-authorized"},
                                          "operation").Mark();
                res = PAYMENT_SRC_NOT_AUTHORIZED;
                break;
            case PATH_PAYMENT_SRC_NO_TRUST:
                app.getMetrics().NewMeter({"op-payment", "failure", "src-no-trust"},
                                          "operation").Mark();
                res = PAYMENT_SRC_NO_TRUST;
                break;
            case PATH_PAYMENT_NO_DESTINATION:
                app.getMetrics().NewMeter({"op-payment", "failure", "no-destination"},
                                          "operation").Mark();
                res = PAYMENT_NO_DESTINATION;
                break;
            case PATH_PAYMENT_NO_TRUST:
                app.getMetrics().NewMeter({"op-payment", "failure", "no-trust"}, "operation")
                .Mark();
                res = PAYMENT_NO_TRUST;
                break;
            case PATH_PAYMENT_NOT_AUTHORIZED:
                app.getMetrics().NewMeter({"op-payment", "failure", "not-authorized"},
                                          "operation").Mark();
                res = PAYMENT_NOT_AUTHORIZED;
                break;
            case PATH_PAYMENT_LINE_FULL:
                app.getMetrics().NewMeter({"op-payment", "failure", "line-full"},
                                          "operation").Mark();
                res = PAYMENT_LINE_FULL;
                break;
            case PATH_PAYMENT_NO_ISSUER:
                app.getMetrics().NewMeter({"op-payment", "failure", "no-issuer"},
                                          "operation").Mark();
                res = PAYMENT_NO_ISSUER;
                break;
            default:
                throw std::runtime_error("Unexpected error code from pathPayment");
        }
        innerResult().code(res);
        return false;
    }
    
    assert(PathPaymentOpFrame::getInnerCode(ppayment.getResult()) ==
           PATH_PAYMENT_SUCCESS);
    
    app.getMetrics().NewMeter({"op-payment", "success", "apply"}, "operation").Mark();
    innerResult().code(PAYMENT_SUCCESS);
    
    return true;
    
    
    
    
    
    
    
    
    
    // Handle payment reversal source
	TrustFrame::pointer sourceLineFrame;
	auto issuerTrustLine = TrustFrame::loadTrustLineIssuer(getSourceID(), mRefund.asset, db, delta);
	if (!issuerTrustLine.second)
	{
		app.getMetrics().NewMeter({ "op-refund-payment", "failure", "no-issuer" },
			"operation").Mark();
		innerResult().code(PAYMENT_REVERSAL_NO_ISSUER);
		return false;
	}

	bool sourceLineExists = !!issuerTrustLine.first;
	if (!sourceLineExists)
	{
		if (getSourceID() == getIssuer(mRefund.asset))
		{
			auto line = OperationFrame::createTrustLine(app, ledgerManager, delta, mParentTx, mSourceAccount, mRefund.asset);
			sourceLineExists = !!line;
			sourceLineFrame = line;
		}
	}
	else
	{
		sourceLineFrame = issuerTrustLine.first;
	}

	if (!sourceLineExists)
	{
		app.getMetrics().NewMeter({ "op-refund-payment", "failure", "src-no-trust" },
			"operation").Mark();
		innerResult().code(PAYMENT_REVERSAL_SRC_NO_TRUST);
		return false;
	}

	if (!sourceLineFrame->isAuthorized())
	{
		app.getMetrics().NewMeter(
		{ "op-refund-payment", "failure", "src-not-authorized" },
			"operation").Mark();
		innerResult().code(PAYMENT_REVERSAL_SRC_NOT_AUTHORIZED);
		return false;
	}

	int64 sourceRecieved = mRefund.amount - mRefund.commissionAmount;

	if (!sourceLineFrame->addBalance(-sourceRecieved))
	{
		app.getMetrics().NewMeter({ "op-refund-payment", "failure", "underfunded" },
			"operation").Mark();
		innerResult().code(PAYMENT_REVERSAL_UNDERFUNDED);
		return false;
	}

	sourceLineFrame->storeChange(delta, db);

	// Handle destination
	TrustFrame::pointer destLine;

	auto tlI = TrustFrame::loadTrustLineIssuer(mRefund.paymentSource, mRefund.asset, db, delta);
	if (!tlI.second)
	{
		app.getMetrics().NewMeter({ "op-refund-payment", "failure", "no-issuer" },
			"operation").Mark();
		innerResult().code(PAYMENT_REVERSAL_NO_ISSUER);
		return false;
	}

	destLine = tlI.first;

	if (!destLine)
	{
		auto destination = AccountFrame::loadAccount(delta, mRefund.paymentSource, db);
		if (!destination) {
			app.getMetrics().NewMeter({ "op-refund-payment", "failure", "no-payment-sender" },
				"operation").Mark();
			innerResult().code(PAYMENT_REVERSAL_NO_PAYMENT_SENDER);
			return false;
		}

		destLine = OperationFrame::createTrustLine(app, ledgerManager, delta, mParentTx, destination, mRefund.asset);
	}

	if (!destLine)
	{
		app.getMetrics().NewMeter({ "op-refund-payment", "failure", "no-payment-sender-trust" }, "operation").Mark();
		innerResult().code(PAYMENT_REVERSAL_NO_PAYMENT_SENDER_TRUST);
		return false;
	}

	if (!destLine->isAuthorized())
	{
		app.getMetrics().NewMeter({ "op-refund-payment", "failure", "payment-sender-not-authorized" },
			"operation").Mark();
		innerResult().code(PAYMENT_REVERSAL_PAYMENT_SENDER_NOT_AUTHORIZED);
		return false;
	}

	if (!destLine->addBalance(mRefund.amount))
	{
		app.getMetrics().NewMeter({ "op-refund-payment", "failure", "payment-sender-line-full" },
			"operation").Mark();
		innerResult().code(PAYMENT_REVERSAL_PAYMENT_SENDER_LINE_FULL);
		return false;
	}

	destLine->storeChange(delta, db);

	innerResult().code(PAYMENT_REVERSAL_SUCCESS);
	return true;
}

bool
PaymentRefundOpFrame::doCheckValid(Application& app)
{
    if (mRefund.amount <= 0)
    {
        app.getMetrics().NewMeter({"op-refund-payment", "invalid", "malformed-amount"},
                         "operation").Mark();
        innerResult().code(PAYMENT_REVERSAL_MALFORMED);
        return false;
    }
    
	if (mRefund.commissionAmount < 0)
	{
		app.getMetrics().NewMeter({ "op-refund-payment", "invalid", "malformed-negative-commission" },
			"operation").Mark();
		innerResult().code(PAYMENT_REVERSAL_MALFORMED);
		return false;
	}

	if (!isAssetValid(app.getIssuer(), mRefund.asset) || mRefund.asset.type() == ASSET_TYPE_NATIVE)
	{
		app.getMetrics().NewMeter({ "op-refund-payment", "invalid", "malformed-invalid-asset" },
			"operation").Mark();
		innerResult().code(PAYMENT_REVERSAL_MALFORMED);
		return false;
	}

    return true;
}
    
}
