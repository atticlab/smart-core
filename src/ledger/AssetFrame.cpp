// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/AssetFrame.h"
#include "database/Database.h"
#include "crypto/SecretKey.h"
#include "crypto/SHA.h"
#include "LedgerDelta.h"
#include "util/types.h"

using namespace std;
using namespace soci;

namespace stellar
{
const char* AssetFrame::kSQLCreateStatement1 =
    "CREATE TABLE asset"
    "("
	"issuer       VARCHAR(56) NOT NULL,"
	"code         VARCHAR(12) NOT NULL,"
    "asset_type   INT NOT NULL,"
    "anonymous    INT NOT NULL,"
    "lastmodified INT NOT NULL,"
    "PRIMARY KEY  (issuer, code)"
    ");";

static const char* assetColumnSelector =
    "SELECT issuer, code, asset_type, anonymous, lastmodified FROM asset";

AssetFrame::AssetFrame() : EntryFrame(ASSET), mAsset(mEntry.data.asset())
{
}

AssetFrame::AssetFrame(LedgerEntry const& from)
    : EntryFrame(from), mAsset(mEntry.data.asset())
{
}

AssetFrame::AssetFrame(AssetFrame const& from) : AssetFrame(from.mEntry)
{
}

AssetFrame& AssetFrame::operator=(AssetFrame const& other)
{
    if (&other != this)
    {
        mAsset = other.mAsset;
        mKey = other.mKey;
        mKeyCalculated = other.mKeyCalculated;
    }
    return *this;
}

bool
AssetFrame::isValid(AssetEntry const& oe)
{
    return isAssetValid(oe.asset);
}

bool
AssetFrame::isValid() const
{
    return isValid(mAsset);
}

AssetFrame::pointer
AssetFrame::loadAsset(Asset const& asset, Database& db, LedgerDelta* delta)
{
    AssetFrame::pointer retAsset;
	AccountID issuer = getIssuer(asset);
    std::string issuerIDStrKey = PubKeyUtils::toStrKey(issuer);
	std::string code = getCode(asset);

    std::string sql = assetColumnSelector;
    sql += " WHERE issuer = :issuer AND code = :code";
    auto prep = db.getPreparedStatement(sql);
    auto& st = prep.statement();
    st.exchange(use(issuerIDStrKey));
    st.exchange(use(code));

    auto timer = db.getSelectTimer("asset");
    loadAssets(prep, [&retAsset](LedgerEntry const& asset)
               {
                   retAsset = make_shared<AssetFrame>(asset);
               });

    if (delta && retAsset)
    {
        delta->recordEntry(*retAsset);
    }

    return retAsset;
}

void
AssetFrame::loadAssets(StatementContext& prep,
                       std::function<void(LedgerEntry const&)> assetProcessor)
{
    unsigned int assetType, anonymous;
	std::string code, issuerStrKey;

    LedgerEntry le;
    le.data.type(ASSET);
    AssetEntry& ae = le.data.asset();

    statement& st = prep.statement();
    st.exchange(into(issuerStrKey));
    st.exchange(into(code));
    st.exchange(into(assetType));
    st.exchange(into(anonymous));
    st.exchange(into(le.lastModifiedLedgerSeq));
    st.define_and_bind();
    st.execute(true);
    while (st.got_data())
    {
        if (assetType > ASSET_TYPE_CREDIT_ALPHANUM12 || assetType == ASSET_TYPE_NATIVE)
            throw std::runtime_error("bad asset type");

		ae.asset.type((AssetType)assetType);
		if (assetType == ASSET_TYPE_CREDIT_ALPHANUM12)
		{
			ae.asset.alphaNum12().issuer = PubKeyUtils::fromStrKey(issuerStrKey);
			strToAssetCode(ae.asset.alphaNum12().assetCode, code);
		}
		else if (assetType == ASSET_TYPE_CREDIT_ALPHANUM4)
		{
			ae.asset.alphaNum4().issuer = PubKeyUtils::fromStrKey(issuerStrKey);
			strToAssetCode(ae.asset.alphaNum4().assetCode, code);
		}

		ae.isAnonymous = anonymous == 0 ? false : true;

        if (!isValid(ae))
        {
            throw std::runtime_error("Invalid asset");
        }

        assetProcessor(le);
        st.fetch();
    }
}

void
AssetFrame::loadAssets(AccountID const& issuer,
                       std::vector<AssetFrame::pointer>& retAssets,
                       Database& db)
{
    std::string issuerStrKey = PubKeyUtils::toStrKey(issuer);

    std::string sql = assetColumnSelector;
    sql += " WHERE issuer = :id";
    auto prep = db.getPreparedStatement(sql);
    auto& st = prep.statement();
    st.exchange(use(issuerStrKey));

    auto timer = db.getSelectTimer("asset");
    loadAssets(prep, [&retAssets](LedgerEntry const& of)
               {
                   retAssets.emplace_back(make_shared<AssetFrame>(of));
               });
}

std::unordered_map<AccountID, std::vector<AssetFrame::pointer>>
AssetFrame::loadAllAssets(Database& db)
{
    std::unordered_map<AccountID, std::vector<AssetFrame::pointer>> retAssets;
    std::string sql = assetColumnSelector;
    sql += " ORDER BY issuer";
    auto prep = db.getPreparedStatement(sql);

    auto timer = db.getSelectTimer("asset");
    loadAssets(prep, [&retAssets](LedgerEntry const& as)
               {
                   auto& thisUserAssets = retAssets[getIssuer(as.data.asset().asset)];
                   thisUserAssets.emplace_back(make_shared<AssetFrame>(as));
               });
    return retAssets;
}

bool
AssetFrame::exists(Database& db, LedgerKey const& key)
{
    std::string issuer = PubKeyUtils::toStrKey(getIssuer(key.asset().asset));
	std::string code = getCode(key.asset().asset);
    int exists = 0;
    auto timer = db.getSelectTimer("asset-exists");
    auto prep =
        db.getPreparedStatement("SELECT EXISTS (SELECT NULL FROM asset "
                                "WHERE issuer=:id AND code=:s)");
    auto& st = prep.statement();
    st.exchange(use(issuer));
    st.exchange(use(code));
    st.exchange(into(exists));
    st.define_and_bind();
    st.execute(true);
    return exists != 0;
}

uint64_t
AssetFrame::countObjects(soci::session& sess)
{
    uint64_t count = 0;
    sess << "SELECT COUNT(*) FROM asset;", into(count);
    return count;
}

void
AssetFrame::storeDelete(LedgerDelta& delta, Database& db) const
{
    storeDelete(delta, db, getKey());
}

void
AssetFrame::storeDelete(LedgerDelta& delta, Database& db, LedgerKey const& key)
{
    auto timer = db.getDeleteTimer("asset");
    auto prep = db.getPreparedStatement("DELETE FROM asset WHERE issuer =:is AND code = :c");
    auto& st = prep.statement();
	std::string issuer = PubKeyUtils::toStrKey(getIssuer(key.asset().asset));
	std::string code = getCode(key.asset().asset);
    st.exchange(use(issuer));
	st.exchange(use(code));
    st.define_and_bind();
    st.execute(true);
    delta.deleteEntry(key);
}

void
AssetFrame::storeChange(LedgerDelta& delta, Database& db)
{
    storeUpdateHelper(delta, db, false);
}

void
AssetFrame::storeAdd(LedgerDelta& delta, Database& db)
{
    storeUpdateHelper(delta, db, true);
}

void
AssetFrame::storeUpdateHelper(LedgerDelta& delta, Database& db, bool insert)
{
    touch(delta);

    if (!isValid())
    {
        throw std::runtime_error("Invalid asset");
    }

    std::string issuer = PubKeyUtils::toStrKey(getIssuer(mAsset.asset));
	std::string code = getCode(mAsset.asset);
    unsigned int assetType = mAsset.asset.type();
	int anonymous = mAsset.isAnonymous ? 1 : 0;

    string sql;

    if (insert)
    {
        sql = "INSERT INTO asset (issuer, code, asset_type, anonymous,lastmodified) VALUES "
              "(:is,:c,:t,:an,:lm)";
    }
    else
    {
        sql = "UPDATE asset SET anonymous=:an, "
              "asset_type=:t, lastmodified=:lm WHERE issuer=:is AND code=:c";
    }

    auto prep = db.getPreparedStatement(sql);
    auto& st = prep.statement();

    st.exchange(use(issuer, "is"));
    st.exchange(use(code, "c"));
    st.exchange(use(assetType, "t"));
    st.exchange(use(anonymous, "an"));
    st.exchange(use(getLastModified(), "lm"));
    st.define_and_bind();

    auto timer =
        insert ? db.getInsertTimer("asset") : db.getUpdateTimer("asset");
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
AssetFrame::dropAll(Database& db)
{
    db.getSession() << "DROP TABLE IF EXISTS asset;";
    db.getSession() << kSQLCreateStatement1;
}
}
