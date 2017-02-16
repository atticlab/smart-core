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
    
    RefundedPaymentFrame::pointer refundFrame = RefundedPaymentFrame::loadRefundedPayment(key.refundedPayment().rID,db);
    
	if (refundFrame)
    {
        auto& rp = refundFrame->getRefundedPayment();
        if (rp.refundedAmount + mRefund.amount > rp.totalOriginalAmount)
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
                app.getMetrics().NewMeter({"op-refund", "failure", "underfunded"},
                                          "operation").Mark();
                res = REFUND_UNDERFUNDED;
                break;
            case PATH_PAYMENT_SRC_NOT_AUTHORIZED:
                app.getMetrics().NewMeter({"op-refund", "failure", "src-not-authorized"},
                                          "operation").Mark();
                res = REFUND_SRC_NOT_AUTHORIZED;
                break;
            case PATH_PAYMENT_SRC_NO_TRUST:
                app.getMetrics().NewMeter({"op-refund", "failure", "src-no-trust"},
                                          "operation").Mark();
                res = REFUND_SRC_NO_TRUST;
                break;
            case PATH_PAYMENT_NO_DESTINATION:
                app.getMetrics().NewMeter({"op-refund", "failure", "no-destination"},
                                          "operation").Mark();
                res = REFUND_NO_PAYMENT_SENDER;
                break;
            case PATH_PAYMENT_NO_TRUST:
                app.getMetrics().NewMeter({"op-refund", "failure", "no-trust"}, "operation")
                .Mark();
                res = REFUND_NO_PAYMENT_SENDER_TRUST;
                break;
            case PATH_PAYMENT_NOT_AUTHORIZED:
                app.getMetrics().NewMeter({"op-refund", "failure", "not-authorized"},
                                          "operation").Mark();
                res = REFUND_PAYMENT_SENDER_NOT_AUTHORIZED;
                break;
            case PATH_PAYMENT_LINE_FULL:
                app.getMetrics().NewMeter({"op-refund", "failure", "line-full"},
                                          "operation").Mark();
                res = REFUND_PAYMENT_SENDER_LINE_FULL;
                break;
            case PATH_PAYMENT_NO_ISSUER:
                app.getMetrics().NewMeter({"op-refund", "failure", "no-issuer"},
                                          "operation").Mark();
                res = REFUND_NO_ISSUER;
                break;
            default:
                throw std::runtime_error("Unexpected error code from pathPayment");
        }
        innerResult().code(res);
        return false;
    }
    
    assert(PathPaymentOpFrame::getInnerCode(ppayment.getResult()) ==
           PATH_PAYMENT_SUCCESS);
    
    app.getMetrics().NewMeter({"op-refund", "success", "apply"}, "operation").Mark();
    innerResult().code(REFUND_SUCCESS);
    
    return true;
}

bool
PaymentRefundOpFrame::doCheckValid(Application& app)
{
    if (mRefund.amount <= 0)
    {
        app.getMetrics().NewMeter({"op-refund-payment", "invalid", "malformed-amount"},
                         "operation").Mark();
        innerResult().code(REFUND_INVALID_AMOUNT);
        return false;
    }
    
    if (mRefund.amount > mRefund.originalAmount)
    {
        app.getMetrics().NewMeter({"op-refund-payment", "invalid", "malformed-amount-bigger-original"},
                                  "operation").Mark();
        innerResult().code(REFUND_INVALID_AMOUNT);
        return false;
    }
    
	if (!isAssetValid(app.getIssuer(), mRefund.asset) || mRefund.asset.type() == ASSET_TYPE_NATIVE)
	{
		app.getMetrics().NewMeter({ "op-refund-payment", "invalid", "malformed-invalid-asset" },
			"operation").Mark();
		innerResult().code(REFUND_INVALID_ASSET);
		return false;
	}

    return true;
}
    
}
