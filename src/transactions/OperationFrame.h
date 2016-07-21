#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include <memory>
#include "ledger/LedgerManager.h"
#include "ledger/AccountFrame.h"
#include "overlay/StellarXDR.h"
#include "util/types.h"

namespace medida
{
class MetricsRegistry;
}

namespace stellar
{
class Application;
class LedgerManager;
class LedgerDelta;

class TransactionFrame;

class OperationFrame
{
  protected:
    Operation const& mOperation;
    TransactionFrame& mParentTx;
    AccountFrame::pointer mSourceAccount;
    OperationResult& mResult;
    OperationFee* mFee;

    bool checkSignature() const;

    virtual bool doCheckValid(Application& app) = 0;
    virtual bool doApply(Application& app, LedgerDelta& delta,
                         LedgerManager& ledgerManager) = 0;
    virtual int32_t getNeededThreshold() const;
    bool checkBankSigned(Application& app);

  public:
    static std::shared_ptr<OperationFrame>
    makeHelper(Operation const& op, OperationResult& res, OperationFee* fee,
               TransactionFrame& parentTx);

    OperationFrame(Operation const& op, OperationResult& res, OperationFee* fee,
                   TransactionFrame& parentTx);
    OperationFrame(OperationFrame const&) = delete;

    AccountFrame&
    getSourceAccount() const
    {
        assert(mSourceAccount);
        return *mSourceAccount;
    }

    // overrides internal sourceAccount used by this operation
    // normally set automatically by checkValid
    void
    setSourceAccountPtr(AccountFrame::pointer sa)
    {
        mSourceAccount = sa;
    }

    AccountID const& getSourceID() const;

    // load account if needed
    // returns true on success
    bool loadAccount(LedgerDelta* delta, Database& db);

    OperationResult&
    getResult() const
    {
        return mResult;
    }
    OperationResultCode getResultCode() const;

    virtual bool checkValid(Application& app, LedgerDelta* delta = nullptr);

    bool apply(LedgerDelta& delta, Application& app);

    Operation const&
    getOperation() const
    {
        return mOperation;
    }
};
}
