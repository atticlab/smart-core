// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "main/Application.h"
#include "ledger/LedgerManager.h"
#include "main/Config.h"
#include "util/Timer.h"
#include "overlay/LoopbackPeer.h"
#include "util/make_unique.h"
#include "main/test.h"
#include "lib/catch.hpp"
#include "util/Logging.h"
#include "TxTests.h"
#include "ledger/LedgerDelta.h"

using namespace stellar;
using namespace stellar::txtest;

typedef std::unique_ptr<Application> appPtr;

// Merging when you are holding credit
// Merging when others are holding your credit
// Merging and then trying to set options in same ledger
// Merging with outstanding 0 balance trust lines
// Merging with outstanding offers
// Merge when you have outstanding data entries
TEST_CASE("manage asset operation", "[tx][manage_asset]")
{
    Config cfg(getTestConfig());

    VirtualClock clock;
    Application::pointer appPtr = Application::create(clock, cfg);
    Application& app = *appPtr;
    app.start();
    upgradeToCurrentLedgerVersion(app);

    // set up world
    SecretKey root = getRoot(app.getNetworkID());
    SecretKey admin = getAccount("admin");
	auto rootSeq = getAccountSeqNum(root, app) + 1;
	auto adminSigner = Signer(admin.getPublicKey(), 100, SIGNER_ADMIN);
	applySetOptions(app, root, rootSeq++, nullptr, nullptr, nullptr, nullptr, &adminSigner, nullptr);

	Asset asset = makeAsset(root, "UAH");

	SECTION("EUAH in genesis ledger")
	{
		Asset euah = makeAsset(root, "EUAH");
		AssetEntry entry;
		entry.asset = euah;
		entry.isAnonymous = true;
		AssetFrame::pointer stored = AssetFrame::loadAsset(euah, app.getDatabase());
		REQUIRE(stored);
		REQUIRE(stored->getAsset() == entry);
	}
	SECTION("Only admin signer of bank can manage asset")
	{
		SECTION("General signer can't manage")
		{
			auto generalAdmin = getAccount("general_admin");
			auto generalAdminSigner = Signer(generalAdmin.getPublicKey(), 100, SIGNER_GENERAL);
			applySetOptions(app, root, rootSeq++, nullptr, nullptr, nullptr, nullptr, &generalAdminSigner, nullptr);
			applyManageAssetOp(app, root, rootSeq++, generalAdmin, asset, false, false, MANAGE_ASSET_NOT_AUTHORIZED);
		}
		SECTION("Random account can't manage assets")
		{
			auto randomAccount = getAccount("random_account");
			applyCreateAccountTx(app, root, randomAccount, rootSeq++, 0, &admin);
			SequenceNumber randomAccountSeq = getAccountSeqNum(randomAccount, app) + 1;
			applyManageAssetOp(app, randomAccount, randomAccountSeq++, randomAccount, asset, false, false, MANAGE_ASSET_NOT_AUTHORIZED);
		}
	}
	SECTION("Trying to delete asset, that not exists")
	{
		applyManageAssetOp(app, root, rootSeq++, admin, asset, false, true, MANAGE_ASSET_NOT_EXIST);
	}
	SECTION("Trying to add asset with invalid issuer")
	{
		auto randomAccount = SecretKey::random();
		auto randomAsset = makeAsset(randomAccount, "USD");
		applyManageAssetOp(app, root, rootSeq++, admin, randomAsset, false, false, MANAGE_ASSET_INVALID_ISSUER);
	}
	SECTION("Success")
	{
		// create
		applyManageAssetOp(app, root, rootSeq++, admin, asset, false, false);
		// update
		applyManageAssetOp(app, root, rootSeq++, admin, asset, true, false);
		// delete
		applyManageAssetOp(app, root, rootSeq++, admin, asset, true, true);
	}
}
