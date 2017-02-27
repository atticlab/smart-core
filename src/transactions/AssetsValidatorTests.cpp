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
#include "AssetsValidator.h"

using namespace stellar;
using namespace stellar::txtest;

typedef std::unique_ptr<Application> appPtr;

TEST_CASE("Assets Validator", "[validator][asset]")
{
    Config const& cfg = getTestConfig();

    VirtualClock clock;
    Application::pointer appPtr = Application::create(clock, cfg);
    Application& app = *appPtr;

    app.start();

    // set up world
    SecretKey root = getRoot(app.getNetworkID());
	AssetsValidator validator(app, app.getDatabase());

	SECTION("Native")
	{
		Asset native;
		native.type(ASSET_TYPE_NATIVE);
		REQUIRE(!validator.isAssetValid(native));
	}
	SECTION("Invalid issuer")
	{
		auto randomAccount = SecretKey::random();
		Asset asset = makeAsset(randomAccount, "USD");
		REQUIRE(!validator.isAssetValid(asset));
	}
	SECTION("Asset")
	{
		Asset validAsset = makeAsset(root, "UAH");
		REQUIRE(validator.isAssetValid(validAsset));

		SecretKey admin = getAccount("admin");
		auto adminSigner = Signer(admin.getPublicKey(), 100, SIGNER_ADMIN);
		auto rootSeq = getAccountSeqNum(root, app) + 1;
		applySetOptions(app, root, rootSeq++, nullptr, nullptr, nullptr, nullptr, &adminSigner, nullptr);

		// asset not allowed
		REQUIRE(!validator.isAssetAllowed(validAsset));
		// allow asset
		applyManageAssetOp(app, root, rootSeq++, admin, validAsset, false, false);
		REQUIRE(validator.isAssetAllowed(validAsset));
	}
}
