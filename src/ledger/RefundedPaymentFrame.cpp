// Copyright 2016 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/RefundedPaymentFrame.h"
#include "database/Database.h"
#include "crypto/SecretKey.h"
#include "crypto/SHA.h"
#include "LedgerDelta.h"
#include "util/types.h"
#include "util/basen.h"

using namespace std;
using namespace soci;

namespace stellar
{
const char* RefundedPaymentFrame::kSQLCreateStatement1 =
    "CREATE TABLE refunded_payment"
    "("
        "id           BIGINT NOT NULL,"
        "assettype    INT             NOT NULL,"
        "issuer       VARCHAR(56)     NOT NULL,"
        "assetcode    VARCHAR(12)     NOT NULL,"
        "refunded     BIGINT          NOT NULL,"
        "totalamount  BIGINT          NOT NULL,"
        "lastmodified INT             NOT NULL,"
        "PRIMARY KEY (id)"
    ");";

static const char* refundedPaymentColumnSelector =
    "SELECT id, assettype, issuer, assetcode, refunded, totalamount, lastmodified FROM refunded_payment";

RefundedPaymentFrame::RefundedPaymentFrame() : EntryFrame(REFUNDED_PAYMENT), mRefundedPayment(mEntry.data.refundedPayment())
{
}

RefundedPaymentFrame::RefundedPaymentFrame(LedgerEntry const& from)
    : EntryFrame(from), mRefundedPayment(mEntry.data.refundedPayment())
{
}

RefundedPaymentFrame::RefundedPaymentFrame(RefundedPaymentFrame const& from) : RefundedPaymentFrame(from.mEntry)
{
}

RefundedPaymentFrame& RefundedPaymentFrame::operator=(RefundedPaymentFrame const& other)
{
    if (&other != this)
    {
		mRefundedPayment = other.mRefundedPayment;
        mKey = other.mKey;
        mKeyCalculated = other.mKeyCalculated;
    }
    return *this;
}

RefundedPaymentFrame::pointer
RefundedPaymentFrame::loadRefundedPayment(int64 id, Database& db)
{
    RefundedPaymentFrame::pointer retData;

    std::string sql = refundedPaymentColumnSelector;
    sql += " WHERE id = :id";
    auto prep = db.getPreparedStatement(sql);
    auto& st = prep.statement();
    st.exchange(use(id));

    auto timer = db.getSelectTimer("refunded_payment");
    loadData(prep, [&retData](LedgerEntry const& data)
               {
                   retData = make_shared<RefundedPaymentFrame>(data);
               });

    return retData;
}

void
RefundedPaymentFrame::loadData(StatementContext& prep,
                       std::function<void(LedgerEntry const&)> dataProcessor)
{
    LedgerEntry le;
    le.data.type(REFUNDED_PAYMENT);
    RefundEntry& oe = le.data.refundedPayment();

    std::string issuerStrKey, assetCode;
    unsigned int assetType;

    statement& st = prep.statement();
    st.exchange(into(oe.rID));
    st.exchange(into(assetType));
    st.exchange(into(issuerStrKey));
    st.exchange(into(assetCode));
    st.exchange(into(oe.refundedAmount));
    st.exchange(into(oe.totalOriginalAmount));
    st.exchange(into(le.lastModifiedLedgerSeq));
    st.define_and_bind();
    st.execute(true);
    while (st.got_data())
    {
        oe.asset.type((AssetType)assetType);
        if (assetType == ASSET_TYPE_CREDIT_ALPHANUM4)
        {
            oe.asset.alphaNum4().issuer = PubKeyUtils::fromStrKey(issuerStrKey);
            strToAssetCode(oe.asset.alphaNum4().assetCode, assetCode);
        }
        else if (assetType == ASSET_TYPE_CREDIT_ALPHANUM12)
        {
            oe.asset.alphaNum12().issuer =
            PubKeyUtils::fromStrKey(issuerStrKey);
            strToAssetCode(oe.asset.alphaNum12().assetCode, assetCode);
        }

        dataProcessor(le);
        st.fetch();
    }
}

bool
RefundedPaymentFrame::exists(Database& db, LedgerKey const& key)
{
    int exists = 0;
    auto timer = db.getSelectTimer("refunded_payment-exists");
    auto prep =
        db.getPreparedStatement("SELECT EXISTS (SELECT NULL FROM refunded_payment "
                                "WHERE id=:id)");
    auto& st = prep.statement();
    st.exchange(use(key.refundedPayment().rID));
    st.exchange(into(exists));
    st.define_and_bind();
    st.execute(true);
    return exists != 0;
}

uint64_t
RefundedPaymentFrame::countObjects(soci::session& sess)
{
    uint64_t count = 0;
    sess << "SELECT COUNT(*) FROM refunded_payment;", into(count);
    return count;
}

void
RefundedPaymentFrame::storeDelete(LedgerDelta& delta, Database& db) const
{
    storeDelete(delta, db, getKey());
}

void
RefundedPaymentFrame::storeDelete(LedgerDelta& delta, Database& db, LedgerKey const& key)
{
    auto timer = db.getDeleteTimer("refunded_payment");
    auto prep = db.getPreparedStatement("DELETE FROM refunded_payment WHERE id=:id");
    auto& st = prep.statement();
    st.exchange(use(key.refundedPayment().rID));
    st.define_and_bind();
    st.execute(true);
    delta.deleteEntry(key);
}


void
RefundedPaymentFrame::storeChange(LedgerDelta& delta, Database& db)
{
    storeUpdateHelper(delta, db, false);
}

void
RefundedPaymentFrame::storeAdd(LedgerDelta& delta, Database& db)
{
    storeUpdateHelper(delta, db, true);
}

void
RefundedPaymentFrame::storeUpdateHelper(LedgerDelta& delta, Database& db, bool insert)
{
    touch(delta);

    string sql;

    if (insert)
    {
        sql = "INSERT INTO refunded_payment (id, assettype, issuer, assetcode, refunded, totalamount, lastmodified)"
               " VALUES (:id, :at, :iss, :ac, :ref, :tot, :lm)";
    }
    else
    {
        sql = "UPDATE refunded_payment SET refunded=:ref, totalamount=:tot, lastmodified=:lm WHERE id=:id";
    }

    auto prep = db.getPreparedStatement(sql);
    auto& st = prep.statement();
    unsigned int assetType = mRefundedPayment.asset.type();
    std::string issuerStrKey, assetCode;
    issuerStrKey = PubKeyUtils::toStrKey(assetType == ASSET_TYPE_CREDIT_ALPHANUM4 ? mRefundedPayment.asset.alphaNum4().issuer : mRefundedPayment.asset.alphaNum12().issuer);
    if (assetType == ASSET_TYPE_CREDIT_ALPHANUM4)
    {
        assetCodeToStr(mRefundedPayment.asset.alphaNum4().assetCode, assetCode);
    }
    else
    {
        assetCodeToStr(mRefundedPayment.asset.alphaNum12().assetCode, assetCode);
    }
    st.exchange(use(mRefundedPayment.rID, "id"));
    if (insert)
    {
        st.exchange(use(assetType, "at"));
        st.exchange(use(issuerStrKey, "iss"));
        st.exchange(use(assetCode, "ac"));
    }
    st.exchange(use(mRefundedPayment.refundedAmount, "ref"));
    st.exchange(use(mRefundedPayment.totalOriginalAmount, "tot"));
    st.exchange(use(getLastModified(), "lm"));

    st.define_and_bind();
    st.execute(true);

    if (st.get_affected_rows() != 1)
    {
        throw std::runtime_error("could not update SQL");
    }

    if (insert)
    {
        delta.addEntry(*this);
    }
    else
    {
        delta.modEntry(*this);
    }
}

void
RefundedPaymentFrame::dropAll(Database& db)
{
    db.getSession() << "DROP TABLE IF EXISTS refunded_payment;";
    db.getSession() << kSQLCreateStatement1;
}
}
