#pragma once

#include "transactions/OperationFrame.h"

namespace stellar
{

class PaymentRefundOpFrame : public OperationFrame
{
    RefundResult&
    innerResult()
    {
        return mResult.tr().refundResult();
    }
    RefundOp const& mRefund;

	bool checkAllowed();

public:
    PaymentRefundOpFrame(Operation const& op, OperationResult& res, OperationFee* fee,
                   TransactionFrame& parentTx);

    bool doApply(Application& app, LedgerDelta& delta,
                 LedgerManager& ledgerManager) override;
    bool doCheckValid(Application& app) override;

    static RefundResultCode
    getInnerCode(OperationResult const& res)
    {
        return res.tr().refundResult().code();
    }
};
}
