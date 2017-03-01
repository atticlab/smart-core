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
#include "ledger/AssetFrame.h"
#include "transactions/PaymentOpFrame.h"
#include "transactions/ChangeTrustOpFrame.h"
#include "ledger/StatisticsFrame.h"
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
    Config cfg = getTestConfig(0, Config::TESTDB_POSTGRESQL);
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

    SecretKey sk = getAccount("admin_signer");

    const int64_t paymentAmount = 100;
    auto signer = Signer(sk.getPublicKey(), 100, SIGNER_ADMIN);
    SequenceNumber rootSeq = getAccountSeqNum(root, app) + 1;
    applySetOptions(app, root, rootSeq++, nullptr, nullptr, nullptr, nullptr, &signer, nullptr);
    
    // create an account
    applyCreateAccountTx(app, root, a1, rootSeq++, 0, &sk, CREATE_ACCOUNT_SUCCESS, AccountType::ACCOUNT_REGISTERED_USER);

    SequenceNumber a1Seq = getAccountSeqNum(a1, app) + 1;

    const int64_t morePayment = paymentAmount / 2;

    const int64_t assetMultiplier = 10000000;

    int64_t trustLineLimit = INT64_MAX;

    int64_t trustLineStartingBalance = 20000 * assetMultiplier;

    Asset idrCur = makeAsset(root, "IDR");
    Asset usdCur = makeAsset(root, "USD");
	Asset eurCur = makeAsset(root, "EUR");
	applyManageAssetOp(app, root, rootSeq++, sk, idrCur, false, false);
	applyManageAssetOp(app, root, rootSeq++, sk, usdCur, false, false);
	applyManageAssetOp(app, root, rootSeq++, sk, eurCur, false, false);

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

	SECTION("EUAH limits")
	{
		auto anonUser = SecretKey::random();
		applyCreateAccountTx(app, root, anonUser, rootSeq++, 0, &sk);
		SequenceNumber anonUserSeq = getAccountSeqNum(anonUser, app) + 1;
		auto anonReceiver = SecretKey::random();
		applyCreateAccountTx(app, root, anonReceiver, rootSeq++, 0, &sk);
		auto user = SecretKey::random();
		applyCreateAccountTx(app, root, user, rootSeq++, 0, &sk, CREATE_ACCOUNT_SUCCESS, AccountType::ACCOUNT_REGISTERED_USER);
		auto euah = makeAsset(root, "EUAH");
		auto storedEUAH = AssetFrame::loadAsset(euah, app.getDatabase());
		int64 maxBalance = storedEUAH->getAsset().maxBalance;
		int64 maxDailyOut = storedEUAH->getAsset().maxDailyOut;
		REQUIRE(maxBalance > -1);
		REQUIRE(maxBalance > maxDailyOut);
		applyCreditPaymentTx(app, root, anonUser, euah, rootSeq++, storedEUAH->getAsset().maxBalance, &sk);
		SECTION("account->set agent")
		{
			auto settlementAgent = SecretKey::random();
			applyCreateAccountTx(app, root, settlementAgent, rootSeq++, 0, &sk, CREATE_ACCOUNT_SUCCESS, ACCOUNT_SETTLEMENT_AGENT);
			auto settlemSeq = getAccountSeqNum(settlementAgent, app);
			applyCreditPaymentTx(app, anonUser, settlementAgent, euah, anonUserSeq++, storedEUAH->getAsset().maxDailyOut+1, nullptr, nullptr, PaymentResultCode::PAYMENT_SRC_ASSET_LIMITS_EXCEEDED);
		}
		SECTION("Balance limit exceeded")
		{
			applyCreditPaymentTx(app, root, anonUser, euah, rootSeq++, 1, &sk, nullptr, PaymentResultCode::PAYMENT_DEST_ASSET_LIMITS_EXCEEDED);
		}
		SECTION("Max daily out exceeded")
		{
			closeLedgerOn(app, app.getLedgerManager().getLastClosedLedgerNum() + 1, 20, 1, 2017);
			time_t performedAt = app.getLedgerManager().getCloseTime();
			auto localPerformedAt = *localtime(&performedAt);
			applyCreditPaymentTx(app, anonUser, anonReceiver, euah, anonUserSeq++, maxDailyOut / 2);
			applyCreditPaymentTx(app, anonUser, user, euah, anonUserSeq++, maxDailyOut / 2);
			applyCreditPaymentTx(app, anonUser, anonReceiver, euah, anonUserSeq++, 1, nullptr, nullptr, PaymentResultCode::PAYMENT_SRC_ASSET_LIMITS_EXCEEDED);
			closeLedgerOn(app, app.getLedgerManager().getLastClosedLedgerNum() + 1, 28, 1, 2017);
			time_t now = app.getLedgerManager().getCloseTime();
			auto localNow = *localtime(&now);
			REQUIRE(localPerformedAt.tm_yday != localNow.tm_yday);
			applyCreditPaymentTx(app, anonUser, anonReceiver, euah, anonUserSeq++, 1, nullptr, nullptr);
			auto anonToAnonReceiver = StatisticsFrame::loadStatistics(anonUser.getPublicKey(), euah, AccountType::ACCOUNT_ANONYMOUS_USER, app.getDatabase())->getStatistics();
			REQUIRE(anonToAnonReceiver.dailyOutcome == 1);
			REQUIRE(anonToAnonReceiver.monthlyOutcome == (maxDailyOut / 2) + 1);
			REQUIRE(anonToAnonReceiver.annualOutcome == (maxDailyOut / 2) + 1);
		}
	}
	SECTION("Anonymous account can't receive nonanonymous asset")
	{
		auto anonUser = SecretKey::random();
		applyCreateAccountTx(app, root, anonUser, rootSeq++, 0, &sk);
		applyCreditPaymentTx(app, root, anonUser, usdCur, rootSeq++, paymentAmount, &sk, nullptr, PAYMENT_NOT_AUTHORIZED);
	}
	SECTION("Can't create account on non anonymous account")
	{
		auto anonUser = getAccount("anonUser");
		OperationFee fee;
		fee.type(OperationFeeType::opFEE_CHARGED);
		fee.fee().amountToCharge = paymentAmount / 2;
		fee.fee().asset = idrCur;
		applyCreditPaymentTx(app, root, anonUser, idrCur, rootSeq++, paymentAmount, &sk, &fee, PAYMENT_NO_DESTINATION);
	}
	SECTION("success account creation") {
		auto aUAH = makeAsset(root, "AUAH");
		applyManageAssetOp(app, root, rootSeq++, sk, aUAH, true, false);
		auto anonUser = getAccount("anonUser");
		auto oldBalance = 0;
		OperationFee fee;
		fee.type(OperationFeeType::opFEE_CHARGED);
		fee.fee().amountToCharge = paymentAmount / 2;
		fee.fee().asset = aUAH;
		applyCreditPaymentTx(app, root, anonUser, aUAH, rootSeq++, paymentAmount, &sk, &fee);

		auto anonAccount = loadAccount(anonUser, app);
		REQUIRE(anonAccount->getAccount().accountType == ACCOUNT_ANONYMOUS_USER);

		auto anonLine = loadTrustLine(anonUser, aUAH, app);
		REQUIRE(anonLine->getBalance() == paymentAmount - fee.fee().amountToCharge);

		auto commLine = loadTrustLine(commissionSeed, aUAH, app);
		REQUIRE((commLine->getBalance() == fee.fee().amountToCharge));

		auto anonSeq = getAccountSeqNum(anonUser, app) + 1;
		applyCreditPaymentTx(app, anonUser, root, aUAH, anonSeq++, paymentAmount - fee.fee().amountToCharge, nullptr);
	}
	SECTION("Statistics")
	{
		auto cad = makeAsset(root, "CAD");
		applyManageAssetOp(app, root, rootSeq++, sk, cad, false, false);
		auto source = SecretKey::random();
		applyCreateAccountTx(app, root, source, rootSeq++, 0, &sk, CREATE_ACCOUNT_SUCCESS, AccountType::ACCOUNT_REGISTERED_USER);
		SequenceNumber sourceSeq = getAccountSeqNum(source, app) + 1;
		auto dest = SecretKey::random();
		applyCreateAccountTx(app, root, dest, rootSeq++, 0, &sk, CREATE_ACCOUNT_SUCCESS, AccountType::ACCOUNT_REGISTERED_USER);
		// initial payment from bank
		int64 amount = 1235000;
		OperationFee fee;
		fee.type(OperationFeeType::opFEE_NONE);
		applyCreditPaymentTx(app, root, source, cad, rootSeq++, amount, &sk, &fee);
		// check source account stats
		auto sourceBankStats = StatisticsFrame::loadStatistics(source.getPublicKey(), cad, AccountType::ACCOUNT_BANK, app.getDatabase());
		REQUIRE(sourceBankStats);
		REQUIRE(sourceBankStats->getStatistics().dailyIncome == amount);
		// payment to dest with fee
		fee.type(OperationFeeType::opFEE_CHARGED);
		fee.fee().amountToCharge = amount / 2;
		fee.fee().asset = cad;
		applyCreditPaymentTx(app, source, dest, cad, sourceSeq++, amount, nullptr, &fee);
		// check source stats
		sourceBankStats = StatisticsFrame::loadStatistics(source.getPublicKey(), cad, AccountType::ACCOUNT_BANK, app.getDatabase());
		REQUIRE(sourceBankStats);
		REQUIRE(sourceBankStats->getStatistics().dailyIncome == amount);
		auto sourceDestStats = StatisticsFrame::loadStatistics(source.getPublicKey(), cad, AccountType::ACCOUNT_REGISTERED_USER, app.getDatabase());
		REQUIRE(sourceDestStats);
		REQUIRE(sourceDestStats->getStatistics().dailyOutcome == amount);
		// check dest stats
		auto destSourceStats = StatisticsFrame::loadStatistics(dest.getPublicKey(), cad, AccountType::ACCOUNT_REGISTERED_USER, app.getDatabase());
		REQUIRE(destSourceStats);
		REQUIRE(destSourceStats->getStatistics().dailyIncome == amount / 2);
		// check commission stats
		auto comStats = StatisticsFrame::loadStatistics(commissionSeed.getPublicKey(), cad, AccountType::ACCOUNT_REGISTERED_USER, app.getDatabase());
		REQUIRE(comStats);
		REQUIRE(comStats->getStatistics().dailyIncome == amount / 2);

		// send money back
		SequenceNumber destSeq = getAccountSeqNum(dest, app) + 1;
		fee.type(OperationFeeType::opFEE_CHARGED);
		fee.fee().amountToCharge = amount / 4;
		fee.fee().asset = cad;
		applyCreditPaymentTx(app, dest, source, cad, destSeq++, amount / 2, nullptr, &fee);
		// check source stats
		sourceBankStats = StatisticsFrame::loadStatistics(source.getPublicKey(), cad, AccountType::ACCOUNT_BANK, app.getDatabase());
		REQUIRE(sourceBankStats);
		REQUIRE(sourceBankStats->getStatistics().dailyIncome == amount);
		sourceDestStats = StatisticsFrame::loadStatistics(source.getPublicKey(), cad, AccountType::ACCOUNT_REGISTERED_USER, app.getDatabase());
		REQUIRE(sourceDestStats);
		REQUIRE(sourceDestStats->getStatistics().dailyOutcome == amount);
		REQUIRE(sourceDestStats->getStatistics().dailyIncome == amount/4);
		// check dest stats
		destSourceStats = StatisticsFrame::loadStatistics(dest.getPublicKey(), cad, AccountType::ACCOUNT_REGISTERED_USER, app.getDatabase());
		REQUIRE(destSourceStats);
		REQUIRE(destSourceStats->getStatistics().dailyIncome == amount / 2);
		REQUIRE(destSourceStats->getStatistics().dailyOutcome == amount / 2);
		// check commission stats
		comStats = StatisticsFrame::loadStatistics(commissionSeed.getPublicKey(), cad, AccountType::ACCOUNT_REGISTERED_USER, app.getDatabase());
		REQUIRE(comStats);
		REQUIRE(comStats->getStatistics().dailyIncome == (amount / 2) + (amount / 4));

	}
	SECTION("payment") 
	{
		auto newAccount = getAccount("newAccount");
		OperationFee fee;
		fee.type(OperationFeeType::opFEE_CHARGED);
		fee.fee().amountToCharge = paymentAmount / 2;
		fee.fee().asset = idrCur;
		SECTION("invalid asset")
		{
			auto invalidAsset = makeAsset(root, "INA");
			fee.fee().asset = invalidAsset;
			applyCreditPaymentTx(app, a1, newAccount, invalidAsset, a1Seq++, paymentAmount, nullptr, &fee, PAYMENT_ASSET_NOT_ALLOWED);
		}
		// create an account
		applyCreateAccountTx(app, root, b1, rootSeq++, 0, &sk, CREATE_ACCOUNT_SUCCESS, AccountType::ACCOUNT_REGISTERED_USER);
		applyChangeTrust(app, a1, root, a1Seq++, "IDR", INT64_MAX);
		applyCreditPaymentTx(app, root, a1, idrCur, rootSeq++, paymentAmount, &sk);
        
        
        //TODO: due to txBAD_AUTH tx result we don't have a transaction result, and can't check using existing methods
//        SECTION("using root key to sign regular tx")
//        {
//            applyCreditPaymentTx(app, root, a1, idrCur, rootSeq++, paymentAmount, nullptr, nullptr, ???txBAD_AUTH???);
//        }
        
        SECTION("balance confirmations")
        {
            TrustFrame::pointer line;
            line = loadTrustLine(a1, idrCur, app);
            REQUIRE(line->getBalance() == paymentAmount);
            
            line = loadTrustLine(root, idrCur, app);
            REQUIRE(line->getBalance() == -paymentAmount);
            
        }
        
		auto b1Seq = getAccountSeqNum(b1, app) + 1;
		applyChangeTrust(app, b1, root, b1Seq++, "IDR", INT64_MAX);

		SECTION("underfunded")
		{
			applyCreditPaymentTx(app, a1, b1, idrCur, a1Seq++, paymentAmount*2, nullptr, nullptr, PAYMENT_UNDERFUNDED);
		}
		SECTION("PAYMENT_SRC_NO_TRUST") 
		{
			applyChangeTrust(app, b1, root, b1Seq++, "USD", INT64_MAX);
			applyCreditPaymentTx(app, a1, b1, usdCur, a1Seq++, paymentAmount, nullptr, nullptr, PAYMENT_SRC_NO_TRUST);
		}
//		SECTION("PAYMENT_NO_TRUST")
//		{
//			applyCreateAccountTx(app, root, newAccount, rootSeq++, 0, &sk);
//			applyCreditPaymentTx(app, a1, newAccount, idrCur, a1Seq++, paymentAmount, nullptr, nullptr, PAYMENT_NO_TRUST);
//		}
		SECTION("PAYMENT_LINE_FULL")
		{
			applyCreditPaymentTx(app, root, a1, idrCur, rootSeq++, INT64_MAX, &sk, &fee, PAYMENT_LINE_FULL);
		}
		
	}
    SECTION("Create account")
    {
        SECTION("Success")
        {
            applyCreateAccountTx(app, root, b1, rootSeq++, 0, &sk);
            SECTION("Account already exists")
            {
                applyCreateAccountTx(app, root, b1, rootSeq++, 0, &sk, CREATE_ACCOUNT_ALREADY_EXIST);
            }
        }
		SECTION("Scratch card")
		{
			auto distr = getAccount("distr");
			applyCreateAccountTx(app, root, distr, rootSeq++, 0, &sk, CREATE_ACCOUNT_SUCCESS, ACCOUNT_DISTRIBUTION_AGENT);
			auto distrSeq = getAccountSeqNum(distr, app) + 1;
			SECTION("Only can ACCOUNT_DISTRIBUTION_AGENT create account")
			{
				auto account = SecretKey::random();
				applyCreateAccountTx(app, root, account, rootSeq++, 100, &sk, CREATE_ACCOUNT_WRONG_TYPE, ACCOUNT_SCRATCH_CARD, &usdCur);
			}
			SECTION("Invalid amount")
			{
				auto account = SecretKey::random();
				applyCreateAccountTx(app, distr, account, distrSeq++, 0, nullptr, CREATE_ACCOUNT_MALFORMED, ACCOUNT_SCRATCH_CARD);
			}
			SECTION("Invalid asset")
			{
				auto account = SecretKey::random();
				auto invalidCur = makeAsset(distr, "USD");
				applyCreateAccountTx(app, distr, account, distrSeq++, 100, nullptr, CREATE_ACCOUNT_ASSET_NOT_ALLOWED, ACCOUNT_SCRATCH_CARD, &invalidCur);
			}
			SECTION("Scratch card can't exceed limits")
			{
				auto euah = makeAsset(root, "EUAH");
				auto storedEUAH = AssetFrame::loadAsset(euah, app.getDatabase());
				int64 amount = storedEUAH->getAsset().maxBalance + 10;
				applyCreditPaymentTx(app, root, distr, euah, rootSeq++, 200 + amount, &sk);
				auto account = SecretKey::random();
				applyCreateAccountTx(app, distr, account, distrSeq++, amount, nullptr, CREATE_ACCOUNT_DEST_ASSET_LIMITS_EXCEEDED, ACCOUNT_SCRATCH_CARD, &euah);
			}
			SECTION("Success")
			{
				int64 amount = int64(150) * int64(10000000);
				auto account = SecretKey::random();
                applyChangeTrust(app, distr, root, distrSeq++, "EUAH", INT64_MAX);
				auto euah = makeAsset(root, "EUAH");
                applyCreditPaymentTx(app, root, distr, euah, rootSeq++, 200+amount, &sk);
				applyCreateAccountTx(app, distr, account, distrSeq++, amount, nullptr, CREATE_ACCOUNT_SUCCESS, ACCOUNT_SCRATCH_CARD, &euah);
				auto loadedAccount = loadAccount(account, app);
				REQUIRE(loadedAccount);
				auto accountLine = loadTrustLine(account, euah, app, true);
				REQUIRE(accountLine);
				REQUIRE(accountLine->getBalance() == amount);
				SECTION("Can't send more to scratch card")
				{
					applyCreditPaymentTx(app, root, account, euah, rootSeq++, 100, &sk, nullptr, PAYMENT_NO_DESTINATION);
				}
				SECTION("Can spend, can't deposit")
				{
					auto accountSeq = getAccountSeqNum(account, app) + 1;
					applyCreditPaymentTx(app, account, root, euah, accountSeq++, amount / 2);
					accountLine = loadTrustLine(account, euah, app, true);
					REQUIRE(accountLine);
					REQUIRE(accountLine->getBalance() == amount / 2);
					// can't deposit
					applyCreditPaymentTx(app, root, account, euah, rootSeq++, 100, &sk, nullptr, PAYMENT_NO_DESTINATION);
				}
			}
		}
    }

    SECTION("issuer large amounts")
    {
        applyChangeTrust(app, a1, root, a1Seq++, "IDR", INT64_MAX);
        applyCreditPaymentTx(app, root, a1, idrCur, rootSeq++,
                             INT64_MAX, &sk);
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
        REQUIRE(gwLines.size() == 1);
    }
    SECTION("authorize flag")
    {
        uint32_t setFlags = AUTH_REQUIRED_FLAG | AUTH_REVOCABLE_FLAG;

        applySetOptions(app, root, rootSeq++, nullptr, &setFlags,
                        nullptr, nullptr, nullptr, nullptr);

        applyChangeTrust(app, a1, root, a1Seq++, "IDR", trustLineLimit);
		SequenceNumber commSeq = getAccountSeqNum(commissionSeed, app) + 1;
		applyChangeTrust(app, commissionSeed, root, commSeq++, "IDR", trustLineLimit);

        applyCreditPaymentTx(app, root, a1, idrCur, rootSeq++,
                             trustLineStartingBalance, &sk, nullptr, PAYMENT_NOT_AUTHORIZED);

        applyAllowTrust(app, root, a1, rootSeq++, "IDR", true, &sk);
		applyAllowTrust(app, root, commissionSeed, rootSeq++, "IDR", true, &sk);

        applyCreditPaymentTx(app, root, a1, idrCur, rootSeq++,
                             trustLineStartingBalance, &sk);

        // send it all back
        applyAllowTrust(app, root, a1, rootSeq++, "IDR", false, &sk);

        applyCreditPaymentTx(app, a1, root, idrCur, a1Seq++,
                             trustLineStartingBalance, nullptr, nullptr,
                             PAYMENT_SRC_NOT_AUTHORIZED);

        applyAllowTrust(app, root, a1, rootSeq++, "IDR", true, &sk);

        applyCreditPaymentTx(app, a1, root, idrCur, a1Seq++,
                             trustLineStartingBalance);
    }
	SECTION("Invalid issuer") {
		applyChangeTrust(app, a1, b1, a1Seq++, "USD", INT64_MAX, CHANGE_TRUST_ASSET_NOT_ALLOWED);
		auto invalidCur = makeAsset(b1, "USD");
		applyCreditPaymentTx(app, a1, b1, invalidCur, a1Seq++, 123, nullptr, nullptr, PAYMENT_ASSET_NOT_ALLOWED);
	}
    SECTION("payment through path")
    {
		applyChangeTrust(app, a1, root, a1Seq++, "EUR", trustLineLimit);
		applyPathPaymentTx(app, root, a1, idrCur, morePayment * 10,
			eurCur, morePayment, rootSeq++, &sk, nullptr,
			PATH_PAYMENT_MALFORMED);
    }
	SECTION("fee") {
		applyCreateAccountTx(app, root, b1, rootSeq++, 0, &sk, CREATE_ACCOUNT_SUCCESS, AccountType::ACCOUNT_REGISTERED_USER);
		auto b1Seq = getAccountSeqNum(b1, app) + 1;
		applyChangeTrust(app, a1, root, a1Seq++, "USD", INT64_MAX);
		applyChangeTrust(app, b1, root, b1Seq++, "USD", INT64_MAX);
		applyCreditPaymentTx(app, root, a1, usdCur, rootSeq++, paymentAmount, &sk);

		SECTION("invalid fee") {
			SECTION("fee is not set") {
				auto paymentFrame = createCreditPaymentTx(app.getNetworkID(), a1, b1, usdCur, a1Seq++, paymentAmount);
				TransactionEnvelope env = paymentFrame->getEnvelope();
				env.operationFees.clear();
				auto payment = std::make_shared<TransactionFrame>(app.getNetworkID(), env);
				applyCheck(payment, delta, app);
				REQUIRE(payment->getResultCode() == txINTERNAL_ERROR);
			}
			SECTION("Fee is too big") {
				auto paymentFrame = createCreditPaymentTx(app.getNetworkID(), a1, b1, usdCur, a1Seq++, paymentAmount);
				TransactionEnvelope env = paymentFrame->getEnvelope();
				OperationFee fee;
				fee.type(OperationFeeType::opFEE_NONE);
				env.operationFees.push_back(fee);
				auto payment = std::make_shared<TransactionFrame>(app.getNetworkID(), env);
				applyCheck(payment, delta, app);
				REQUIRE(payment->getResultCode() == txINTERNAL_ERROR);
			}
			SECTION("fee - invalid asset") {
				OperationFee fee;
				fee.type(OperationFeeType::opFEE_CHARGED);
				fee.fee().amountToCharge = paymentAmount / 2;
				applyCreditPaymentTx(app, a1, b1, usdCur, a1Seq++, paymentAmount, nullptr, &fee, PAYMENT_MALFORMED);
			}
			SECTION("negative fee") {
				OperationFee fee;
				fee.type(OperationFeeType::opFEE_CHARGED);
				fee.fee().amountToCharge = -2;
				applyCreditPaymentTx(app, a1, b1, usdCur, a1Seq++, paymentAmount, nullptr, &fee, PAYMENT_MALFORMED);
			}
			SECTION("fee greater than amount") {
				OperationFee fee;
				fee.type(OperationFeeType::opFEE_CHARGED);
				fee.fee().amountToCharge = paymentAmount * 2;
				applyCreditPaymentTx(app, a1, b1, usdCur, a1Seq++, paymentAmount, nullptr, &fee, PAYMENT_MALFORMED);
			}
			SECTION("issuer large amounts")
			{
				applyChangeTrust(app, a1, root, a1Seq++, "EUR", INT64_MAX);
				auto paymentAmount = INT64_MAX;
				OperationFee fee;
				fee.type(OperationFeeType::opFEE_CHARGED);
				fee.fee().amountToCharge = paymentAmount / 10;
				fee.fee().asset = eurCur;
				applyCreditPaymentTx(app, root, a1, eurCur, rootSeq++, INT64_MAX, &sk, &fee);
				auto line = loadTrustLine(a1, eurCur, app);
				auto balance = INT64_MAX - fee.fee().amountToCharge;
				REQUIRE(line->getBalance() == balance);

				auto commissionLine = loadTrustLine(commissionSeed, eurCur, app);
				REQUIRE(commissionLine);
				REQUIRE(commissionLine->getBalance() == fee.fee().amountToCharge);

				// send it all back
				applyCreditPaymentTx(app, a1, root, eurCur, a1Seq++, balance);
				line = loadTrustLine(a1, eurCur, app);
				REQUIRE(line->getBalance() == 0);
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
