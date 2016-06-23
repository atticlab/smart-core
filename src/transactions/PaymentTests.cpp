// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0
#include "main/Application.h"
#include "util/Timer.h"
#include "main/Config.h"
#include "overlay/LoopbackPeer.h"
#include "util/make_unique.h"
#include "main/test.h"
#include "lib/catch.hpp"
#include "util/Logging.h"
#include "TxTests.h"
#include "database/Database.h"
#include "ledger/LedgerManager.h"
#include "ledger/LedgerDelta.h"
#include "transactions/PaymentOpFrame.h"
#include "transactions/ChangeTrustOpFrame.h"
#include "crypto/SHA.h"

using namespace stellar;
using namespace stellar::txtest;

typedef std::unique_ptr<Application> appPtr;

// *XLM Payment
// *Credit Payment
// XLM -> Credit Payment
// Credit -> XLM Payment
// Credit -> XLM -> Credit Payment
// Credit -> Credit -> Credit -> Credit Payment
// path payment where there isn't enough in the path
// path payment with a transfer rate
TEST_CASE("payment", "[tx][payment]")
{
    Config cfg = getTestConfig();
	auto commissionPass = "(V)(^,,,^)(V)";
	auto commissionSeed = SecretKey::fromSeed(sha256(commissionPass));
	cfg.BANK_COMMISSION_KEY = commissionSeed.getPublicKey();

    VirtualClock clock;
    Application::pointer appPtr = Application::create(clock, cfg);
    Application& app = *appPtr;
    app.start();

    // set up world
    SecretKey root = getRoot(app.getNetworkID());
    SecretKey a1 = getAccount("A");
    SecretKey b1 = getAccount("B");

    Asset xlmCur;

    const int64_t paymentAmount = 100;

    SequenceNumber rootSeq = getAccountSeqNum(root, app) + 1;
    // create an account
    applyCreateAccountTx(app, root, a1, rootSeq++, 0);

    SequenceNumber a1Seq = getAccountSeqNum(a1, app) + 1;

    const int64_t morePayment = paymentAmount / 2;

    const int64_t assetMultiplier = 10000000;

    int64_t trustLineLimit = INT64_MAX;

    int64_t trustLineStartingBalance = 20000 * assetMultiplier;

    Asset idrCur = makeAsset(root, "IDR");
    Asset usdCur = makeAsset(root, "USD");

    AccountFrame::pointer a1Account, rootAccount;
    rootAccount = loadAccount(root, app);
    a1Account = loadAccount(a1, app);
    REQUIRE(rootAccount->getMasterWeight() == 1);
    REQUIRE(rootAccount->getHighThreshold() == 0);
    REQUIRE(rootAccount->getLowThreshold() == 0);
    REQUIRE(rootAccount->getMediumThreshold() == 0);
    REQUIRE(a1Account->getMasterWeight() == 1);
    REQUIRE(a1Account->getHighThreshold() == 0);
    REQUIRE(a1Account->getLowThreshold() == 0);
    REQUIRE(a1Account->getMediumThreshold() == 0);

    LedgerDelta delta(app.getLedgerManager().getCurrentLedgerHeader(),
                      app.getDatabase());

    SECTION("Create account")
    {
        SECTION("Success")
        {
            applyCreateAccountTx(app, root, b1, rootSeq++, 0);
            SECTION("Account already exists")
            {
                applyCreateAccountTx(app, root, b1, rootSeq++, 0, CREATE_ACCOUNT_ALREADY_EXIST);
            }
        }
    }

    SECTION("issuer large amounts")
    {
        applyChangeTrust(app, a1, root, a1Seq++, "IDR", INT64_MAX);
        applyCreditPaymentTx(app, root, a1, idrCur, rootSeq++,
                             INT64_MAX);
        TrustFrame::pointer line;
        line = loadTrustLine(a1, idrCur, app);
        REQUIRE(line->getBalance() == INT64_MAX);

        // send it all back
        applyCreditPaymentTx(app, a1, root, idrCur, a1Seq++, INT64_MAX);
        line = loadTrustLine(a1, idrCur, app);
        REQUIRE(line->getBalance() == 0);

        std::vector<TrustFrame::pointer> gwLines;
        TrustFrame::loadLines(root.getPublicKey(), gwLines,
                              app.getDatabase());
        REQUIRE(gwLines.size() == 0);
    }
    SECTION("authorize flag")
    {
        uint32_t setFlags = AUTH_REQUIRED_FLAG | AUTH_REVOCABLE_FLAG;

        applySetOptions(app, root, rootSeq++, nullptr, &setFlags,
                        nullptr, nullptr, nullptr, nullptr);

        applyChangeTrust(app, a1, root, a1Seq++, "IDR", trustLineLimit);

        applyCreditPaymentTx(app, root, a1, idrCur, rootSeq++,
                             trustLineStartingBalance, PAYMENT_NOT_AUTHORIZED);

        applyAllowTrust(app, root, a1, rootSeq++, "IDR", true);

        applyCreditPaymentTx(app, root, a1, idrCur, rootSeq++,
                             trustLineStartingBalance);

        // send it all back
        applyAllowTrust(app, root, a1, rootSeq++, "IDR", false);

        applyCreditPaymentTx(app, a1, root, idrCur, a1Seq++,
                             trustLineStartingBalance,
                             PAYMENT_SRC_NOT_AUTHORIZED);

        applyAllowTrust(app, root, a1, rootSeq++, "IDR", true);

        applyCreditPaymentTx(app, a1, root, idrCur, a1Seq++,
                             trustLineStartingBalance);
    }
    SECTION("payment through path")
    {
        SECTION("send XLM with path (not enough offers)")
        {
            applyPathPaymentTx(app, root, a1, idrCur, morePayment * 10,
                               xlmCur, morePayment, rootSeq++,
                               PATH_PAYMENT_TOO_FEW_OFFERS);
        }

        // setup a1
        applyChangeTrust(app, a1, root, a1Seq++, "USD", trustLineLimit);
        applyChangeTrust(app, a1, root, a1Seq++, "IDR", trustLineLimit);

        applyCreditPaymentTx(app, root, a1, usdCur, rootSeq++,
                             trustLineStartingBalance);

        // add a couple offers in the order book

        OfferFrame::pointer offer;

        const Price usdPriceOffer(2, 1);

        applyCreateAccountTx(app, root, b1, rootSeq++, 0);
        SequenceNumber b1Seq = getAccountSeqNum(b1, app) + 1;
        applyChangeTrust(app, b1, root, b1Seq++, "USD", trustLineLimit);
        applyChangeTrust(app, b1, root, b1Seq++, "IDR", trustLineLimit);

        applyCreditPaymentTx(app, root, b1, idrCur, rootSeq++,
                             trustLineStartingBalance);

        uint64_t offerB1 =
            applyCreateOffer(app, delta, 0, b1, idrCur, usdCur, usdPriceOffer,
                             100 * assetMultiplier, b1Seq++);

        // setup "c1"
        SecretKey c1 = getAccount("C");

        applyCreateAccountTx(app, root, c1, rootSeq++, 0);
        SequenceNumber c1Seq = getAccountSeqNum(c1, app) + 1;

        applyChangeTrust(app, c1, root, c1Seq++, "USD", trustLineLimit);
        applyChangeTrust(app, c1, root, c1Seq++, "IDR", trustLineLimit);

        applyCreditPaymentTx(app, root, c1, idrCur, rootSeq++,
                             trustLineStartingBalance);

        // offer is sell 100 IDR for 150 USD ; buy USD @ 1.5 = sell IRD @ 0.66
        uint64_t offerC1 =
            applyCreateOffer(app, delta, 0, c1, idrCur, usdCur, Price(3, 2),
                             100 * assetMultiplier, c1Seq++);

        // at this point:
        // a1 holds (0, IDR) (trustLineStartingBalance, USD)
        // b1 holds (trustLineStartingBalance, IDR) (0, USD)
        // c1 holds (trustLineStartingBalance, IDR) (0, USD)
        SECTION("send with path (over sendmax)")
        {
            // A1: try to send 100 IDR to B1
            // using 149 USD

            auto res = applyPathPaymentTx(
                app, a1, b1, usdCur, 149 * assetMultiplier, idrCur,
                100 * assetMultiplier, a1Seq++, PATH_PAYMENT_OVER_SENDMAX);
        }

        SECTION("send with path (success)")
        {
            // A1: try to send 125 IDR to B1 using USD
            // should cost 150 (C's offer taken entirely) +
            //  50 (1/4 of B's offer)=200 USD

            auto res = applyPathPaymentTx(
                app, a1, b1, usdCur, 250 * assetMultiplier, idrCur,
                125 * assetMultiplier, a1Seq++, PATH_PAYMENT_SUCCESS);

            auto const& multi = res.success();

            REQUIRE(multi.offers.size() == 2);

            TrustFrame::pointer line;

            // C1
            // offer was taken
            REQUIRE(multi.offers[0].offerID == offerC1);
            REQUIRE(!loadOffer(c1, offerC1, app, false));
            line = loadTrustLine(c1, idrCur, app);
            checkAmounts(line->getBalance(),
                         trustLineStartingBalance - 100 * assetMultiplier);
            line = loadTrustLine(c1, usdCur, app);
            checkAmounts(line->getBalance(), 150 * assetMultiplier);

            // B1
            auto const& b1Res = multi.offers[1];
            REQUIRE(b1Res.offerID == offerB1);
            offer = loadOffer(b1, offerB1, app);
            OfferEntry const& oe = offer->getOffer();
            REQUIRE(b1Res.sellerID == b1.getPublicKey());
            checkAmounts(b1Res.amountSold, 25 * assetMultiplier);
            checkAmounts(oe.amount, 75 * assetMultiplier);
            line = loadTrustLine(b1, idrCur, app);
            // 125 where sent, 25 were consumed by B's offer
            checkAmounts(line->getBalance(), trustLineStartingBalance +
                                                 (125 - 25) * assetMultiplier);
            line = loadTrustLine(b1, usdCur, app);
            checkAmounts(line->getBalance(), 50 * assetMultiplier);

            // A1
            line = loadTrustLine(a1, idrCur, app);
            checkAmounts(line->getBalance(), 0);
            line = loadTrustLine(a1, usdCur, app);
            checkAmounts(line->getBalance(),
                         trustLineStartingBalance - 200 * assetMultiplier);
        }

        SECTION("missing issuer")
        {
            SECTION("dest is standard account")
            {
                SECTION("last")
                {
                    // root issued idrCur
                    applyAccountMerge(app, root, root, rootSeq++);

                    auto res = applyPathPaymentTx(
                        app, a1, b1, usdCur, 250 * assetMultiplier, idrCur,
                        125 * assetMultiplier, a1Seq++, PATH_PAYMENT_NO_ISSUER);
                    REQUIRE(res.noIssuer() == idrCur);
                }
                SECTION("first")
                {
                    // root issued usdCur
                    applyAccountMerge(app, root, root, rootSeq++);

                    auto res = applyPathPaymentTx(
                        app, a1, b1, usdCur, 250 * assetMultiplier, idrCur,
                        125 * assetMultiplier, a1Seq++, PATH_PAYMENT_NO_ISSUER);
                    REQUIRE(res.noIssuer() == usdCur);
                }
                SECTION("mid")
                {
                    std::vector<Asset> path;
                    SecretKey missing = getAccount("missing");
                    Asset btcCur = makeAsset(missing, "BTC");
                    path.emplace_back(btcCur);
                    auto res = applyPathPaymentTx(
                        app, a1, b1, usdCur, 250 * assetMultiplier, idrCur,
                        125 * assetMultiplier, a1Seq++, PATH_PAYMENT_NO_ISSUER,
                        &path);
                    REQUIRE(res.noIssuer() == btcCur);
                }
            }
            SECTION("dest is issuer")
            {
                // single currency payment already covered elsewhere
                // only one negative test:
                SECTION("cannot take offers on the way")
                {
                    // root issued idrCur
                    applyAccountMerge(app, root, root, rootSeq++);

                    auto res = applyPathPaymentTx(
                        app, a1, root, usdCur, 250 * assetMultiplier, idrCur,
                        125 * assetMultiplier, a1Seq++,
                        PATH_PAYMENT_NO_DESTINATION);
                }
            }
        }

        SECTION("send with path (takes own offer)")
        {
            // raise A1's balance by what we're trying to send
            applyPaymentTx(app, root, a1, rootSeq++, 100 * assetMultiplier);

            // offer is sell 100 USD for 100 XLM
            applyCreateOffer(app, delta, 0, a1, usdCur, xlmCur, Price(1, 1),
                             100 * assetMultiplier, a1Seq++);

            // A1: try to send 100 USD to B1 using XLM

            applyPathPaymentTx(app, a1, b1, xlmCur, 100 * assetMultiplier,
                               usdCur, 100 * assetMultiplier, a1Seq++,
                               PATH_PAYMENT_OFFER_CROSS_SELF);
        }

        SECTION("send with path (offer participant reaching limit)")
        {
            // make it such that C can only receive 120 USD (4/5th of offerC)
            applyChangeTrust(app, c1, root, c1Seq++, "USD",
                             120 * assetMultiplier);

            // A1: try to send 105 IDR to B1 using USD
            // cost 120 (C's offer maxed out at 4/5th of published amount)
            //  50 (1/4 of B's offer)=170 USD

            auto res = applyPathPaymentTx(
                app, a1, b1, usdCur, 400 * assetMultiplier, idrCur,
                105 * assetMultiplier, a1Seq++, PATH_PAYMENT_SUCCESS);

            auto& multi = res.success();

            REQUIRE(multi.offers.size() == 2);

            TrustFrame::pointer line;

            // C1
            // offer was taken
            REQUIRE(multi.offers[0].offerID == offerC1);
            REQUIRE(!loadOffer(c1, offerC1, app, false));
            line = loadTrustLine(c1, idrCur, app);
            checkAmounts(line->getBalance(),
                         trustLineStartingBalance - 80 * assetMultiplier);
            line = loadTrustLine(c1, usdCur, app);
            checkAmounts(line->getBalance(), line->getTrustLine().limit);

            // B1
            auto const& b1Res = multi.offers[1];
            REQUIRE(b1Res.offerID == offerB1);
            offer = loadOffer(b1, offerB1, app);
            OfferEntry const& oe = offer->getOffer();
            REQUIRE(b1Res.sellerID == b1.getPublicKey());
            checkAmounts(b1Res.amountSold, 25 * assetMultiplier);
            checkAmounts(oe.amount, 75 * assetMultiplier);
            line = loadTrustLine(b1, idrCur, app);
            // 105 where sent, 25 were consumed by B's offer
            checkAmounts(line->getBalance(), trustLineStartingBalance +
                                                 (105 - 25) * assetMultiplier);
            line = loadTrustLine(b1, usdCur, app);
            checkAmounts(line->getBalance(), 50 * assetMultiplier);

            // A1
            line = loadTrustLine(a1, idrCur, app);
            checkAmounts(line->getBalance(), 0);
            line = loadTrustLine(a1, usdCur, app);
            checkAmounts(line->getBalance(),
                         trustLineStartingBalance - 170 * assetMultiplier);
        }
        SECTION("missing trust line")
        {
            // modify C's trustlines to invalidate C's offer
            // * C's offer should be deleted
            // sell 100 IDR for 200 USD
            // * B's offer 25 IDR by 50 USD

            auto checkBalances = [&]()
            {
                auto res = applyPathPaymentTx(
                    app, a1, b1, usdCur, 200 * assetMultiplier, idrCur,
                    25 * assetMultiplier, a1Seq++, PATH_PAYMENT_SUCCESS);

                auto& multi = res.success();

                REQUIRE(multi.offers.size() == 2);

                TrustFrame::pointer line;

                // C1
                // offer was deleted
                REQUIRE(multi.offers[0].offerID == offerC1);
                REQUIRE(multi.offers[0].amountSold == 0);
                REQUIRE(multi.offers[0].amountBought == 0);
                REQUIRE(!loadOffer(c1, offerC1, app, false));

                // B1
                auto const& b1Res = multi.offers[1];
                REQUIRE(b1Res.offerID == offerB1);
                offer = loadOffer(b1, offerB1, app);
                OfferEntry const& oe = offer->getOffer();
                REQUIRE(b1Res.sellerID == b1.getPublicKey());
                checkAmounts(b1Res.amountSold, 25 * assetMultiplier);
                checkAmounts(oe.amount, 75 * assetMultiplier);
                line = loadTrustLine(b1, idrCur, app);
                // As B was the sole participant in the exchange, the IDR
                // balance should not have changed
                checkAmounts(line->getBalance(), trustLineStartingBalance);
                line = loadTrustLine(b1, usdCur, app);
                // but 25 USD cost 50 USD to send
                checkAmounts(line->getBalance(), 50 * assetMultiplier);

                // A1
                line = loadTrustLine(a1, idrCur, app);
                checkAmounts(line->getBalance(), 0);
                line = loadTrustLine(a1, usdCur, app);
                checkAmounts(line->getBalance(),
                             trustLineStartingBalance - 50 * assetMultiplier);
            };

            SECTION("deleted selling line")
            {
                applyCreditPaymentTx(app, c1, root, idrCur, c1Seq++,
                                     trustLineStartingBalance);

                applyChangeTrust(app, c1, root, c1Seq++, "IDR", 0);

                checkBalances();
            }

            SECTION("deleted buying line")
            {
                applyChangeTrust(app, c1, root, c1Seq++, "USD", 0);
                checkBalances();
            }
        }
    }
}

TEST_CASE("single create account SQL", "[singlesql][paymentsql][hide]")
{
    Config::TestDbMode mode = Config::TESTDB_ON_DISK_SQLITE;
#ifdef USE_POSTGRES
    if (!force_sqlite)
        mode = Config::TESTDB_POSTGRESQL;
#endif

    VirtualClock clock;
    Application::pointer app =
        Application::create(clock, getTestConfig(0, mode));
    app->start();

    SecretKey root = getRoot(app->getNetworkID());
    SecretKey a1 = getAccount("A");
    int64_t txfee = app->getLedgerManager().getTxFee();
    const int64_t paymentAmount =
        app->getLedgerManager().getMinBalance(1) + txfee * 10;

    SequenceNumber rootSeq = getAccountSeqNum(root, *app) + 1;

    {
        auto ctx = app->getDatabase().captureAndLogSQL("createAccount");
        applyCreateAccountTx(*app, root, a1, rootSeq++, paymentAmount);
    }
}
