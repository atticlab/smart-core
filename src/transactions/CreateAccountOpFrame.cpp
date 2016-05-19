// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/asio.h"
#include "transactions/CreateAccountOpFrame.h"
#include "util/Logging.h"
#include "ledger/LedgerDelta.h"
#include "ledger/TrustFrame.h"
#include "ledger/OfferFrame.h"
#include "database/Database.h"
#include "OfferExchange.h"
#include <algorithm>

#include "main/Application.h"
#include "medida/meter.h"
#include "medida/metrics_registry.h"

namespace stellar
{

using namespace std;
using xdr::operator==;

CreateAccountOpFrame::CreateAccountOpFrame(Operation const& op,
                                           OperationResult& res,
                                           OperationFee& fee,
                                           TransactionFrame& parentTx)
    : OperationFrame(op, res, fee, parentTx)
    , mCreateAccount(mOperation.body.createAccountOp())
{
}

bool
CreateAccountOpFrame::doApply(Application& app,
                              LedgerDelta& delta, LedgerManager& ledgerManager)
{
    AccountFrame::pointer destAccount;

    Database& db = ledgerManager.getDatabase();
    mFee.type(opFEE_NONE);

    destAccount =
        AccountFrame::loadAccount(delta, mCreateAccount.destination, db);
    if (!destAccount)
    {
        destAccount = make_shared<AccountFrame>(mCreateAccount.destination);
        destAccount->getAccount().seqNum = 0;
        destAccount->getAccount().accountType  = mCreateAccount.accountType;
        //                delta.getHeaderFrame().getStartingSequenceNumber();
        
        destAccount->storeAdd(delta, db);
        
        app.getMetrics().NewMeter({"op-create-account", "success", "apply"},
                                  "operation").Mark();
        innerResult().code(CREATE_ACCOUNT_SUCCESS);
        return true;
    }
    else
    {
        app.getMetrics().NewMeter({"op-create-account", "failure", "already-exist"},
                         "operation").Mark();
        innerResult().code(CREATE_ACCOUNT_ALREADY_EXIST);
        return false;
    }
}
    
bool
CreateAccountOpFrame::doCheckValid(Application& app)
{
    switch (mCreateAccount.accountType) {
        case ACCOUNT_ANONYMOUS_USER:
            break;
        case ACCOUNT_REGISTERED_USER:
        case ACCOUNT_MERCHANT:
        case ACCOUNT_DISTRIBUTION_AGENT:
        case ACCOUNT_SETTLEMENT_AGENT:
        case ACCOUNT_EXCHANGE_AGENT:
            if (getSourceID() == app.getConfig().BANK_MASTER_KEY){
                break;
            }
            else{
                app.getMetrics().NewMeter({"op-create-account", "invalid",
                    "not-bank-creating-type"},
                                          "operation").Mark();
                innerResult().code(CREATE_ACCOUNT_NOT_AUTHORIZED_TYPE);
                return false;
            }
            break;
        default:
            app.getMetrics().NewMeter({"op-create-account", "invalid",
                "malformed-wrong-type"},
                                      "operation").Mark();
            innerResult().code(CREATE_ACCOUNT_WRONG_TYPE);
            return false;
            break;
    }
    if (mCreateAccount.destination == getSourceID())
    {
        app.getMetrics().NewMeter({"op-create-account", "invalid",
                          "malformed-destination-equals-source"},
                         "operation").Mark();
        innerResult().code(CREATE_ACCOUNT_MALFORMED);
        return false;
    }

    return true;
}
}
