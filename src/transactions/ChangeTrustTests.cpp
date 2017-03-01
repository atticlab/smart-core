// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "main/Application.h"
#include "overlay/LoopbackPeer.h"
#include "util/make_unique.h"
#include "main/test.h"
#include "lib/catch.hpp"
#include "util/Logging.h"
#include "lib/json/json.h"
#include "TxTests.h"

using namespace stellar;
using namespace stellar::txtest;

typedef std::unique_ptr<Application> appPtr;

TEST_CASE("change trust", "[tx][changetrust]")
{
    Config const& cfg = getTestConfig();

    VirtualClock clock;
    Application::pointer appPtr = Application::create(clock, cfg);
    Application& app = *appPtr;
    Database& db = app.getDatabase();

    app.start();

    // set up world
    SecretKey root = getRoot(app.getNetworkID());
	SecretKey admin = getAccount("admin");
	auto adminSigner = Signer(admin.getPublicKey(), 100, SIGNER_ADMIN);
	SequenceNumber rootSeq = getAccountSeqNum(root, app) + 1;
	applySetOptions(app, root, rootSeq++, nullptr, nullptr, nullptr, nullptr, &adminSigner, nullptr);

    SecretKey account = getAccount("gw");

	applyCreateAccountTx(app, root, account, rootSeq++, 0, &admin, CreateAccountResultCode::CREATE_ACCOUNT_SUCCESS, AccountType::ACCOUNT_REGISTERED_USER);
	SequenceNumber accountSeq = getAccountSeqNum(account, app) + 1;
	Asset idrCur = makeAsset(root, "IDR");
	applyManageAssetOp(app, root, rootSeq++, admin, idrCur, false, false);

	SECTION("Asset not allowed")
	{
		applyChangeTrust(app, account, root, accountSeq++, "USD", 0,
			CHANGE_TRUST_ASSET_NOT_ALLOWED);
	}
	SECTION("Anonymous user can't create trust line to non anon asset")
	{
		auto anonUser = SecretKey::random();
		applyCreateAccountTx(app, root, anonUser, rootSeq++, 0, &admin, CreateAccountResultCode::CREATE_ACCOUNT_SUCCESS, AccountType::ACCOUNT_ANONYMOUS_USER);
		applyChangeTrust(app, anonUser, root, accountSeq++, "IDR", 0, CHANGE_TRUST_NOT_AUTHORIZED);
	}
    SECTION("basic tests")
	{

        // create a trustline with a limit of 0
        applyChangeTrust(app, account, root, accountSeq++, "IDR", 0,
                         CHANGE_TRUST_INVALID_LIMIT);

        // create a trustline with a limit of 100
        applyChangeTrust(app, account, root, accountSeq++, "IDR", 100);

        // fill it to 90
        applyCreditPaymentTx(app, root, account, idrCur, rootSeq++, 90, &admin);

        // can't lower the limit below balance
        applyChangeTrust(app, account, root, accountSeq++, "IDR", 89,
                         CHANGE_TRUST_INVALID_LIMIT);
        // can't delete if there is a balance
        applyChangeTrust(app, account, root, accountSeq++, "IDR", 0,
                         CHANGE_TRUST_INVALID_LIMIT);

        // lower the limit at the balance
        applyChangeTrust(app, account, root, accountSeq++, "IDR", 90);

        // clear the balance
        applyCreditPaymentTx(app, account, root, idrCur, accountSeq++, 90);
        // delete the trust line
        applyChangeTrust(app, account, root, accountSeq++, "IDR", 0);
        REQUIRE(!(TrustFrame::loadTrustLine(account.getPublicKey(), idrCur, db)));
    }
}
