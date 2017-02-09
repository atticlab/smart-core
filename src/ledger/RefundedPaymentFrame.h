#pragma once

// Copyright 2016 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/EntryFrame.h"
#include <functional>
#include <unordered_map>

namespace soci
{
class session;
}

namespace stellar
{
class StatementContext;

class RefundedPaymentFrame : public EntryFrame
{
    static void
    loadData(StatementContext& prep,
               std::function<void(LedgerEntry const&)> dataProcessor);

	RefundEntry& mRefundedPayment;

    RefundedPaymentFrame(RefundedPaymentFrame const& from);

    void storeUpdateHelper(LedgerDelta& delta, Database& db, bool insert);

  public:
    typedef std::shared_ptr<RefundedPaymentFrame> pointer;


    RefundedPaymentFrame();
    RefundedPaymentFrame(LedgerEntry const& from);

    RefundedPaymentFrame& operator=(RefundedPaymentFrame const& other);

    EntryFrame::pointer
    copy() const override
    {
        return EntryFrame::pointer(new RefundedPaymentFrame(*this));
    }

    RefundEntry const&
        getRefundedPayment() const
    {
        return mRefundedPayment;
    }

    RefundEntry&
        getRefundedPayment()
    {
        return mRefundedPayment;
    }

    // Instance-based overrides of EntryFrame.
    void storeDelete(LedgerDelta& delta, Database& db) const override;
    void storeChange(LedgerDelta& delta, Database& db) override;
    void storeAdd(LedgerDelta& delta, Database& db) override;

    // Static helpers that don't assume an instance.
    static void storeDelete(LedgerDelta& delta, Database& db,
                            LedgerKey const& key);
    static bool exists(Database& db, LedgerKey const& key);
    static uint64_t countObjects(soci::session& sess);

    // database utilities
    static pointer loadRefundedPayment(Database& db, LedgerKey const& key);

    static void dropAll(Database& db);
    static const char* kSQLCreateStatement1;
};
}
