// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "main/Application.h"
#include "ledger/LedgerManager.h"
#include "ledger/StatisticsFrame.h"
#include "main/Config.h"
#include "util/Timer.h"
#include "overlay/LoopbackPeer.h"
#include "util/make_unique.h"
#include "main/test.h"
#include "lib/catch.hpp"
#include "util/Logging.h"
#include "TxTests.h"
#include "ledger/LedgerDelta.h"
#include "crypto/SHA.h"

using namespace stellar;
using namespace stellar::txtest;

typedef std::unique_ptr<Application> appPtr;

// Merging when you are holding credit
// Merging when others are holding your credit
// Merging and then trying to set options in same ledger
// Merging with outstanding 0 balance trust lines
// Merging with outstanding offers
// Merge when you have outstanding data entries
TEST_CASE("Payment reversal", "[tx][payment_reversal]")
{
	Config cfg = getTestConfig();
	auto commissionPass = "(VVV)(^,,,^)(VVV)";
	auto commissionAccount = SecretKey::fromSeed(sha256(commissionPass));
	cfg.BANK_COMMISSION_KEY = commissionAccount.getPublicKey();

    VirtualClock clock;
    Application::pointer appPtr = Application::create(clock, cfg);
    Application& app = *appPtr;
    app.start();
    upgradeToCurrentLedgerVersion(app);

	const int64 paymentAmount = 1000;
	const int64 commissionAmount = 100;

    // set up world
    SecretKey root = getRoot(app.getNetworkID());
	auto rootSeq = getAccountSeqNum(root, app) + 1;

	SecretKey sk = getAccount("admin_signer");
	auto signer = Signer(sk.getPublicKey(), 100, SIGNER_ADMIN);
	applySetOptions(app, root, rootSeq++, nullptr, nullptr, nullptr, nullptr, &signer, nullptr);
	auto commissionSeq = getAccountSeqNum(commissionAccount, app) + 1;

	// create an account
	auto agent = getAccount("agent");
	applyCreateAccountTx(app, root, agent, rootSeq++, 0, &sk, CREATE_ACCOUNT_SUCCESS, AccountType::ACCOUNT_SETTLEMENT_AGENT);
	auto agentSeq = getAccountSeqNum(agent, app) + 1;

	auto account = getAccount("account");
	applyCreateAccountTx(app, root, account, rootSeq++, 0, &sk, CREATE_ACCOUNT_SUCCESS, AccountType::ACCOUNT_REGISTERED_USER);
	auto accountSeq = getAccountSeqNum(account, app) + 1;

	auto usd = makeAsset(root, "USD");
	applyManageAssetOp(app, root, rootSeq++, sk, usd, false, false);
	// send some assets to agent
	applyChangeTrust(app, agent, root, agentSeq++, "USD", INT64_MAX);
	OperationFee fee;
	fee.type(OperationFeeType::opFEE_CHARGED);
	fee.fee().asset = usd;
	fee.fee().flatFee.activate() = commissionAmount;
	fee.fee().amountToCharge = commissionAmount;
	applyCreditPaymentTx(app, root, agent, usd, rootSeq++, paymentAmount, &sk, &fee);
	auto agentLine = loadTrustLine(agent, usd, app, true);
	REQUIRE(agentLine->getBalance() == paymentAmount - commissionAmount);
	auto commissionLine = loadTrustLine(commissionAccount, usd, app, true);
	REQUIRE(commissionLine->getBalance() == commissionAmount);

	int64 paymentID = rand();

	int64 now = app.getLedgerManager().getCloseTime();

	SECTION("Malformed payment reversal")
	{
		SECTION("Invalid amount")
		{
			applyPaymentReversalOp(app, agent, agentSeq++, paymentID, account, usd, -paymentAmount, commissionAmount, now, PAYMENT_REVERSAL_MALFORMED);
		}
		SECTION("Invalid commission amount")
		{
			applyPaymentReversalOp(app, agent, agentSeq++, paymentID, account, usd, paymentAmount, -commissionAmount, now, PAYMENT_REVERSAL_MALFORMED);
		}
		SECTION("Invalid asset")
		{
			Asset native;
			native.type(ASSET_TYPE_NATIVE);
			applyPaymentReversalOp(app, agent, agentSeq++, paymentID, account, native, paymentAmount, commissionAmount, now, PAYMENT_REVERSAL_ASSET_NOT_ALLOWED);
		}
	}
	SECTION("Commission")
	{
		SECTION("Commission trust line does not exists")
		{
			auto eur = makeAsset(root, "EUR");
			applyManageAssetOp(app, root, rootSeq++, sk, eur, false, false);
			applyCreditPaymentTx(app, root, account, eur, rootSeq++, paymentAmount, &sk);
			applyCreditPaymentTx(app, account, agent, eur, accountSeq++, paymentAmount);
			applyPaymentReversalOp(app, agent, agentSeq++, paymentID, account, eur, paymentAmount, commissionAmount, now, PAYMENT_REVERSAL_COMMISSION_UNDERFUNDED);
		}
	}
	SECTION("Source")
	{
		auto eur = makeAsset(root, "EUR");
		applyManageAssetOp(app, root, rootSeq++, sk, eur, false, false);
		SECTION("Trust line does not exists")
		{
			applyChangeTrust(app, commissionAccount, root, commissionSeq++, "EUR", INT64_MAX);
			applyCreditPaymentTx(app, root, commissionAccount, eur, rootSeq++, commissionAmount, &sk);
			applyCreditPaymentTx(app, root, account, eur, rootSeq++, paymentAmount, &sk);
			applyCreditPaymentTx(app, account, agent, eur, accountSeq++, paymentAmount);
			applyCreditPaymentTx(app, agent, root, eur, agentSeq++, paymentAmount);
			applyChangeTrust(app, agent, root, agentSeq++, "EUR", 0);
			applyPaymentReversalOp(app, agent, agentSeq++, paymentID, account, eur, paymentAmount, commissionAmount, now, PAYMENT_REVERSAL_SRC_NO_TRUST);

		}
		SECTION("Source line underfunded")
		{
			applyCreditPaymentTx(app, root, account, eur, rootSeq++, paymentAmount+1, &sk);
			applyCreditPaymentTx(app, account, agent, eur, accountSeq++, paymentAmount+1);
			applyCreditPaymentTx(app, agent, root, eur, agentSeq++, paymentAmount+1);
			applyPaymentReversalOp(app, agent, agentSeq++, paymentID, account, eur, paymentAmount+1, commissionAmount, now, PAYMENT_REVERSAL_UNDERFUNDED);
		}
	}
	SECTION("Payment sender")
	{
		SECTION("Does not exists")
		{
			auto randomAccount = SecretKey::random();
			applyPaymentReversalOp(app, agent, agentSeq++, paymentID, randomAccount, usd, paymentAmount, commissionAmount, now, PAYMENT_REVERSAL_NO_PAYMENT_SENDER);
		}
		SECTION("Line is full")
		{
			applyChangeTrust(app, account, root, accountSeq++, "USD", paymentAmount - 1);
			applyPaymentReversalOp(app, agent, agentSeq++, paymentID, account, usd, paymentAmount, commissionAmount, now, PAYMENT_REVERSAL_PAYMENT_SENDER_LINE_FULL);
		}
	}
	SECTION("Success")
	{
		// check prebalances
		auto uah = makeAsset(root, "UAH");
		applyManageAssetOp(app, root, rootSeq++, sk, uah, false, false);
		fee.type(OperationFeeType::opFEE_CHARGED);
		fee.fee().amountToCharge = commissionAmount;
		fee.fee().asset = uah;
		// init account
		applyCreditPaymentTx(app, root, account, uah, rootSeq++, paymentAmount, &sk);
		// make payment to be reversed
		closeLedgerOn(app, app.getLedgerManager().getLastClosedLedgerNum() + 1, 20, 1, 2017);
		time_t performedAt = app.getLedgerManager().getCloseTime();
		applyCreditPaymentTx(app, account, agent, uah, accountSeq++, paymentAmount, nullptr, &fee);
		// check stats
		auto accountAgent = StatisticsFrame::loadStatistics(account.getPublicKey(), uah, ACCOUNT_SETTLEMENT_AGENT, app.getDatabase())->getStatistics();
		REQUIRE(accountAgent.dailyOutcome == paymentAmount);
		REQUIRE(accountAgent.monthlyOutcome == paymentAmount);
		REQUIRE(accountAgent.annualOutcome == paymentAmount);
		REQUIRE(loadTrustLine(account, uah, app)->getBalance() == 0);
		auto agetAccount = StatisticsFrame::loadStatistics(agent.getPublicKey(), uah, ACCOUNT_REGISTERED_USER, app.getDatabase())->getStatistics();
		REQUIRE(agetAccount.dailyIncome == paymentAmount - commissionAmount);
		REQUIRE(agetAccount.monthlyIncome == paymentAmount - commissionAmount);
		REQUIRE(agetAccount.annualIncome == paymentAmount - commissionAmount);
		REQUIRE(loadTrustLine(agent, uah, app)->getBalance() == paymentAmount - commissionAmount);
		auto comAccount = StatisticsFrame::loadStatistics(commissionAccount.getPublicKey(), uah, ACCOUNT_REGISTERED_USER, app.getDatabase())->getStatistics();
		REQUIRE(comAccount.dailyIncome == commissionAmount);
		REQUIRE(comAccount.monthlyIncome == commissionAmount);
		REQUIRE(comAccount.annualIncome == commissionAmount);
		REQUIRE(loadTrustLine(commissionAccount, uah, app)->getBalance() == commissionAmount);
		SECTION("Reversed with full stats")
		{
			applyPaymentReversalOp(app, agent, agentSeq++, 221, account, uah, paymentAmount, commissionAmount, performedAt);
			// check stats
			accountAgent = StatisticsFrame::loadStatistics(account.getPublicKey(), uah, ACCOUNT_SETTLEMENT_AGENT, app.getDatabase())->getStatistics();
			REQUIRE(accountAgent.dailyOutcome == 0);
			REQUIRE(accountAgent.monthlyOutcome == 0);
			REQUIRE(accountAgent.annualOutcome == 0);
			REQUIRE(loadTrustLine(account, uah, app)->getBalance() == paymentAmount);
			agetAccount = StatisticsFrame::loadStatistics(agent.getPublicKey(), uah, ACCOUNT_REGISTERED_USER, app.getDatabase())->getStatistics();
			REQUIRE(agetAccount.dailyIncome == 0);
			REQUIRE(agetAccount.monthlyIncome == 0);
			REQUIRE(agetAccount.annualIncome == 0);
			REQUIRE(loadTrustLine(agent, uah, app)->getBalance() == 0);
			comAccount = StatisticsFrame::loadStatistics(commissionAccount.getPublicKey(), uah, ACCOUNT_REGISTERED_USER, app.getDatabase())->getStatistics();
			REQUIRE(comAccount.dailyIncome == 0);
			REQUIRE(comAccount.monthlyIncome == 0);
			REQUIRE(comAccount.annualIncome == 0);
			REQUIRE(loadTrustLine(commissionAccount, uah, app)->getBalance() == 0);
		}
		SECTION("Reversed on next day")
		{
			closeLedgerOn(app, app.getLedgerManager().getLastClosedLedgerNum() + 1, 22, 1, 2017);
			time_t now = app.getLedgerManager().getCloseTime();
			auto localPerformedAt = localtime(&performedAt);
			auto localNow = localtime(&now);
			REQUIRE(localPerformedAt->tm_yday != localNow->tm_mday);
			applyPaymentReversalOp(app, agent, agentSeq++, 221, account, uah, paymentAmount, commissionAmount, performedAt);
			// check stats
			accountAgent = StatisticsFrame::loadStatistics(account.getPublicKey(), uah, ACCOUNT_SETTLEMENT_AGENT, app.getDatabase())->getStatistics();
			REQUIRE(accountAgent.dailyOutcome == 0);
			REQUIRE(accountAgent.monthlyOutcome == 0);
			REQUIRE(accountAgent.annualOutcome == 0);
			REQUIRE(loadTrustLine(account, uah, app)->getBalance() == paymentAmount);
			agetAccount = StatisticsFrame::loadStatistics(agent.getPublicKey(), uah, ACCOUNT_REGISTERED_USER, app.getDatabase())->getStatistics();
			REQUIRE(agetAccount.dailyIncome == 0);
			REQUIRE(agetAccount.monthlyIncome == 0);
			REQUIRE(agetAccount.annualIncome == 0);
			REQUIRE(loadTrustLine(agent, uah, app)->getBalance() == 0);
			comAccount = StatisticsFrame::loadStatistics(commissionAccount.getPublicKey(), uah, ACCOUNT_REGISTERED_USER, app.getDatabase())->getStatistics();
			REQUIRE(comAccount.dailyIncome == 0);
			REQUIRE(comAccount.monthlyIncome == 0);
			REQUIRE(comAccount.annualIncome == 0);
			REQUIRE(loadTrustLine(commissionAccount, uah, app)->getBalance() == 0);
		}
	}
}
