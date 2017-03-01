// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/asio.h"
#include "transactions/PaymentReversalOpFrame.h"
#include "transactions/BalanceManager.h"
#include "util/Logging.h"
#include "ledger/LedgerDelta.h"
#include "ledger/TrustFrame.h"
#include "ledger/ReversedPaymentFrame.h"
#include "database/Database.h"
#include "medida/meter.h"
#include "medida/metrics_registry.h"
#include "main/Application.h"
#include <algorithm>
#include "AssetsValidator.h"

namespace stellar
{

using namespace std;
using xdr::operator==;

PaymentReversalOpFrame::PaymentReversalOpFrame(Operation const& op, OperationResult& res, OperationFee* fee,
                               TransactionFrame& parentTx)
    : OperationFrame(op, res, fee, parentTx), mPaymentReversal(mOperation.body.paymentReversalOp())
{
}

bool PaymentReversalOpFrame::checkAllowed() {
	return mSourceAccount->isAgent();
}

bool PaymentReversalOpFrame::checkAlreadyReversed(LedgerDelta& delta, Database& db) {
	// check if already reversed
	LedgerKey key;
	key.type(REVERSED_PAYMENT);
	key.reversedPayment().rID = mPaymentReversal.paymentID;
	auto alreadyReversed = ReversedPaymentFrame::exists(db, key);
	if (alreadyReversed) {
		return false;
	}

	auto reversedPayment = make_shared<ReversedPaymentFrame>();
	reversedPayment->getReversedPayment().rID = mPaymentReversal.paymentID;
	reversedPayment->storeAdd(delta, db);
	return true;
}

bool
PaymentReversalOpFrame::doApply(Application& app, LedgerDelta& delta,
                        LedgerManager& ledgerManager)
{
	if (!checkAllowed()) {
		app.getMetrics().NewMeter({ "op-reversal-payment", "failure", "not-allowed" },
			"operation").Mark();
		innerResult().code(PAYMENT_REVERSAL_NOT_ALLOWED);
		return false;
	}

	Database& db = ledgerManager.getDatabase();
	if (!checkAlreadyReversed(delta, db)) {
		app.getMetrics().NewMeter({ "op-reversal-payment", "failure", "already-reversed" },
			"operation").Mark();
		innerResult().code(PAYMENT_REVERSAL_ALREADY_REVERSED);
		return false;
	}

	BalanceManager balanceManager(app, db, delta, ledgerManager, mParentTx);

	auto paymentSource = AccountFrame::loadAccount(mPaymentReversal.paymentSource, db);
	if (!paymentSource)
	{
		app.getMetrics().NewMeter({ "op-reversal-payment", "failure", "no-payment-sender" },
			"operation").Mark();
		innerResult().code(PAYMENT_REVERSAL_NO_PAYMENT_SENDER);
		return false;
	}

	auto paymentSourceResult = balanceManager.add(paymentSource, mPaymentReversal.asset, -mPaymentReversal.amount, false, AccountType(mSourceAccount->getAccount().accountType), mPaymentReversal.performedAt);
	switch (paymentSourceResult)
	{
	case BalanceManager::ASSET_NOT_ALLOWED:
		app.getMetrics().NewMeter({ "op-payment-reversal", "invalid", "malformed-currencies" }, "operation").Mark();
		innerResult().code(PAYMENT_REVERSAL_ASSET_NOT_ALLOWED);
		return false;
	case BalanceManager::NOT_AUTHORIZED:
		app.getMetrics().NewMeter({ "op-payment-reversal", "failure", "paymet-sender-not-authorized" }, "operation").Mark();
		innerResult().code(PAYMENT_REVERSAL_PAYMENT_SENDER_NOT_AUTHORIZED);
		return false;
	case BalanceManager::NO_TRUST_LINE:
		app.getMetrics().NewMeter({ "op-payment-reversal", "failure", "payment-sender-no-trust" }, "operation").Mark();
		innerResult().code(PAYMENT_REVERSAL_NO_PAYMENT_SENDER_TRUST);
		return false;
	case BalanceManager::LINE_FULL:
		app.getMetrics().NewMeter({ "op-payment-reversal", "failure", "payment-sender-line-full" }, "operation").Mark();
		innerResult().code(PAYMENT_REVERSAL_PAYMENT_SENDER_LINE_FULL);
		return false;
	case BalanceManager::UNDERFUNDED:
		throw std::runtime_error("Unexpected error for refersal payment sender UNDERFUNDED!");
	case BalanceManager::ASSET_LIMITS_EXCEEDED:
		app.getMetrics().NewMeter({ "op-payment-reversal", "failure", "payment-sender-asset-limits-exceeded" }, "operation").Mark();
		innerResult().code(PAYMENT_REVERSAL_DEST_ASSET_LIMITS_EXCEEDED);
		return false;
	case BalanceManager::STATS_OVERFLOW:
		app.getMetrics().NewMeter({ "op-payment-reversal", "failure", "payment-sender-stats-overflow" }, "operation").Mark();
		innerResult().code(PAYMENT_REVERSAL_DEST_STATS_OVERFLOW);
		return false;
	case BalanceManager::SUCCESS:
		break;
	default:
		throw std::runtime_error("Unexpected response from balance manager for payment sender!");
	}


	auto paymentDestResult = balanceManager.add(mSourceAccount, mPaymentReversal.asset, -(mPaymentReversal.amount - mPaymentReversal.commissionAmount), true, AccountType(paymentSource->getAccount().accountType), mPaymentReversal.performedAt);
	switch (paymentDestResult)
	{
	case BalanceManager::ASSET_NOT_ALLOWED:
		app.getMetrics().NewMeter({ "op-payment-reversal", "invalid", "malformed-currencies" }, "operation").Mark();
		innerResult().code(PAYMENT_REVERSAL_ASSET_NOT_ALLOWED);
		return false;
	case BalanceManager::NOT_AUTHORIZED:
		app.getMetrics().NewMeter({ "op-payment-reversal", "failure", "paymet-dest-not-authorized" }, "operation").Mark();
		innerResult().code(PAYMENT_REVERSAL_SRC_NOT_AUTHORIZED);
		return false;
	case BalanceManager::NO_TRUST_LINE:
		app.getMetrics().NewMeter({ "op-payment-reversal", "failure", "paymet-dest-no-trust" }, "operation").Mark();
		innerResult().code(PAYMENT_REVERSAL_SRC_NO_TRUST);
		return false;
	case BalanceManager::LINE_FULL:
		throw std::runtime_error("Unexpected error for refersal payment sender LINE_FULL!");
	case BalanceManager::UNDERFUNDED:
		app.getMetrics().NewMeter({ "op-payment-reversal", "failure", "paymet-dest-full" }, "operation").Mark();
		innerResult().code(PAYMENT_REVERSAL_UNDERFUNDED);
		return false;
	case BalanceManager::ASSET_LIMITS_EXCEEDED:
		app.getMetrics().NewMeter({ "op-payment-reversal", "failure", "payment-dest-asset-limits-exceeded" }, "operation").Mark();
		innerResult().code(PAYMENT_REVERSAL_SRC_ASSET_LIMITS_EXCEEDED);
		return false;
	case BalanceManager::STATS_OVERFLOW:
		app.getMetrics().NewMeter({ "op-payment-reversal", "failure", "payment-dest-stats-overflow" }, "operation").Mark();
		innerResult().code(PAYMENT_REVERSAL_SRC_STATS_OVERFLOW);
		return false;
	case BalanceManager::SUCCESS:
		break;
	default:
		throw std::runtime_error("Unexpected response from balance manager for payment dest!");
	}

	// Handle commission
	auto commission = AccountFrame::loadAccount(app.getConfig().BANK_COMMISSION_KEY, db);
	assert(!!commission);
	auto commissionResult = balanceManager.add(commission, mPaymentReversal.asset, -mPaymentReversal.commissionAmount, true, AccountType(paymentSource->getAccount().accountType), mPaymentReversal.performedAt);
	switch (commissionResult)
	{
	case BalanceManager::ASSET_NOT_ALLOWED:
		app.getMetrics().NewMeter({ "op-payment-reversal", "invalid", "malformed-currencies" }, "operation").Mark();
		innerResult().code(PAYMENT_REVERSAL_ASSET_NOT_ALLOWED);
		return false;
	case BalanceManager::NOT_AUTHORIZED:
		app.getMetrics().NewMeter({ "op-payment-reversal", "failure", "paymet-commission-not-authorized" }, "operation").Mark();
		innerResult().code(PAYMENT_REVERSAL_SRC_NOT_AUTHORIZED);
		return false;
	case BalanceManager::NO_TRUST_LINE:
		app.getMetrics().NewMeter({ "op-payment-reversal", "failure", "paymet-commission-no-trust" }, "operation").Mark();
		innerResult().code(PAYMENT_REVERSAL_COMMISSION_UNDERFUNDED);
		return false;
	case BalanceManager::LINE_FULL:
		throw std::runtime_error("Unexpected error for refersal payment commission LINE_FULL!");
	case BalanceManager::UNDERFUNDED:
		app.getMetrics().NewMeter({ "op-payment-reversal", "failure", "paymet-commission-full" }, "operation").Mark();
		innerResult().code(PAYMENT_REVERSAL_COMMISSION_UNDERFUNDED);
		return false;
	case BalanceManager::ASSET_LIMITS_EXCEEDED:
		app.getMetrics().NewMeter({ "op-payment-reversal", "failure", "payment-com-asset-limits-exceeded" }, "operation").Mark();
		innerResult().code(PAYMENT_REVERSAL_COMMISSION_ASSET_LIMITS_EXCEEDED);
		return false;
	case BalanceManager::STATS_OVERFLOW:
		app.getMetrics().NewMeter({ "op-payment-reversal", "failure", "payment-dest-stats-overflow" }, "operation").Mark();
		innerResult().code(PAYMENT_REVERSAL_COM_STATS_OVERFLOW);
		return false;
	case BalanceManager::SUCCESS:
		break;
	default:
		throw std::runtime_error("Unexpected response from balance manager for payment commission!");
	}

	innerResult().code(PAYMENT_REVERSAL_SUCCESS);
	return true;
}

bool
PaymentReversalOpFrame::doCheckValid(Application& app)
{

	if (mPaymentReversal.performedAt <= 0)
	{
		app.getMetrics().NewMeter({ "op-reversal-payment", "invalid", "malformed-performed-at" },
			"operation").Mark();
		innerResult().code(PAYMENT_REVERSAL_MALFORMED);
		return false;
	}

    if (mPaymentReversal.amount <= 0)
    {
        app.getMetrics().NewMeter({"op-reversal-payment", "invalid", "malformed-amount"},
                         "operation").Mark();
        innerResult().code(PAYMENT_REVERSAL_MALFORMED);
        return false;
    }
    
	if (mPaymentReversal.commissionAmount < 0 || mPaymentReversal.commissionAmount > mPaymentReversal.amount)
	{
		app.getMetrics().NewMeter({ "op-reversal-payment", "invalid", "malformed-negative-commission" },
			"operation").Mark();
		innerResult().code(PAYMENT_REVERSAL_MALFORMED);
		return false;
	}

    return true;
}
    
}
