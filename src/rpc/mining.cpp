// Copyright (c) 2010 Satoshi Nakamoto
// Copyright (c) 2009-2016 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// File contains modifications by: The Gulden developers
// All modifications:
// Copyright (c) 2016-2018 The Gulden developers
// Authored by: Malcolm MacLeod (mmacleod@webmail.co.za)
// Distributed under the GULDEN software license, see the accompanying
// file COPYING

#include "base58.h"
#include "amount.h"
#include "chain.h"
#include "chainparams.h"
#include "consensus/consensus.h"
#include "consensus/params.h"
#include "consensus/validation.h"
#include "validation/validation.h"
#include "validation/validationinterface.h"
#include "validation/versionbitsvalidation.h"
#include "core_io.h"
#include "init.h"
#include "versionbits.h"
#include "generation/miner.h"
#include "net.h"
#include "policy/fees.h"
#include "pow.h"
#include "rpc/blockchain.h"
#include "rpc/server.h"
#include "txmempool.h"
#include "util.h"
#include "utilstrencodings.h"
#include "arith_uint256.h"
#include "warnings.h"
#include "Gulden/util.h"

#include <memory>
#include <stdint.h>

#include <univalue.h>
#ifdef ENABLE_WALLET
#include "wallet/wallet.h"
#endif
#include "generation/generation.h"
#include "script/script.h"
#include <Gulden/Common/diff.h>
#include <Gulden/rpcgulden.h>
#include <validation/witnessvalidation.h>

/**
 * Return average network hashes per second based on the last 'lookup' blocks,
 * or from the last difficulty change if 'lookup' is nonpositive.
 * If 'height' is nonnegative, compute the estimate at the time when a given block was found.
 */
static UniValue GetNetworkHashPS(int lookup, int height) {
    CBlockIndex *pb = chainActive.Tip();

    if (height >= 0 && height < chainActive.Height())
        pb = chainActive[height];

    if (pb == NULL || !pb->nHeight)
        return 0;

    // If lookup is -1, then use blocks since last difficulty change.
    if (lookup <= 0)
        lookup = pb->nHeight % Params().GetConsensus().DifficultyAdjustmentInterval() + 1;

    // If lookup is larger than chain, then set it to chain length.
    if (lookup > pb->nHeight)
        lookup = pb->nHeight;

    CBlockIndex *pb0 = pb;
    int64_t minTime = pb0->GetBlockTime();
    int64_t maxTime = minTime;
    for (int i = 0; i < lookup; i++) {
        pb0 = pb0->pprev;
        int64_t time = pb0->GetBlockTime();
        minTime = std::min(time, minTime);
        maxTime = std::max(time, maxTime);
    }

    // In case there's a situation where minTime == maxTime, we don't want a divide by zero exception.
    if (minTime == maxTime)
        return 0;

    arith_uint256 workDiff = pb->nChainWork - pb0->nChainWork;
    int64_t timeDiff = maxTime - minTime;

    return workDiff.getdouble() / timeDiff;
}

static UniValue getnetworkhashps(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() > 2)
        throw std::runtime_error(
            "getnetworkhashps ( nblocks height )\n"
            "\nReturns the estimated network hashes per second based on the last n blocks.\n"
            "Pass in [blocks] to override # of blocks, -1 specifies since last difficulty change.\n"
            "Pass in [height] to estimate the network speed at the time when a certain block was found.\n"
            "\nArguments:\n"
            "1. nblocks     (numeric, optional, default=120) The number of blocks, or -1 for blocks since last difficulty change.\n"
            "2. height      (numeric, optional, default=-1) To estimate at the time of the given height.\n"
            "\nResult:\n"
            "x             (numeric) Hashes per second estimated\n"
            "\nExamples:\n"
            + HelpExampleCli("getnetworkhashps", "")
            + HelpExampleRpc("getnetworkhashps", "")
       );

    LOCK(cs_main);
    return GetNetworkHashPS(request.params.size() > 0 ? request.params[0].get_int() : 120, request.params.size() > 1 ? request.params[1].get_int() : -1);
}

static UniValue generateBlocks(std::shared_ptr<CReserveKeyOrScript> coinbaseScript, int nGenerate, uint64_t nMaxTries, bool keepScript)
{
    static const int nInnerLoopCount = 0x10000;
    int nHeightEnd = 0;
    int nHeight = 0;

    {   // Don't keep cs_main locked
        LOCK(cs_main);
        nHeight = chainActive.Height();
        nHeightEnd = nHeight+nGenerate;
    }
    unsigned int nExtraNonce = 0;
    UniValue blockHashes(UniValue::VARR);
    while (nHeight < nHeightEnd)
    {
        std::unique_ptr<CBlockTemplate> pblocktemplate(BlockAssembler(Params()).CreateNewBlock(chainActive.Tip(), coinbaseScript));
        if (!pblocktemplate.get())
            throw JSONRPCError(RPC_INTERNAL_ERROR, "Couldn't create new block");
        CBlock *pblock = &pblocktemplate->block;
        {
            LOCK(cs_main);
            IncrementExtraNonce(pblock, chainActive.Tip(), nExtraNonce);
        }
        while (nMaxTries > 0 && pblock->nNonce < nInnerLoopCount && !CheckProofOfWork(pblock->GetPoWHash(), pblock->nBits, Params().GetConsensus())) {
            ++pblock->nNonce;
            --nMaxTries;
        }
        if (nMaxTries == 0) {
            break;
        }
        if (pblock->nNonce == nInnerLoopCount) {
            continue;
        }
        std::shared_ptr<const CBlock> shared_pblock = std::make_shared<const CBlock>(*pblock);
        if (!ProcessNewBlock(Params(), shared_pblock, true, NULL))
            throw JSONRPCError(RPC_INTERNAL_ERROR, "ProcessNewBlock, block not accepted");
        ++nHeight;
        blockHashes.push_back(pblock->GetHashLegacy().GetHex());

        //mark script as important because it was used at least for one coinbase output if the script came from the wallet
        if (keepScript)
        {
            coinbaseScript->KeepScript();
        }
    }
    return blockHashes;
}

// Key used by getblocktemplate miners.
// Allocated in InitRPCMining, free'd in ShutdownRPCMining
#ifdef ENABLE_WALLET
static CReserveKeyOrScript* pMiningKey = NULL;
#endif

void InitRPCMining()
{
    #ifdef ENABLE_WALLET
    if (!pactiveWallet)
        return;

    // getblocktemplate mining rewards paid here:
    //fixme: (Post-2.1)
    pMiningKey = new CReserveKeyOrScript(pactiveWallet, pactiveWallet->activeAccount, KEYCHAIN_EXTERNAL);
    #else
	return;
    #endif
}

void ShutdownRPCMining()
{
    #ifdef ENABLE_WALLET
    if (!pMiningKey)
        return;

    delete pMiningKey; pMiningKey = NULL;
    #endif
}

static UniValue getgenerate(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() != 0)
        throw std::runtime_error(
            "getgenerate\n"
            "\nReturn if the server is set to generate coins or not. The default is false.\n"
            "It is set with the command line argument -gen (or " + std::string(GULDEN_CONF_FILENAME) + " setting gen)\n"
            "It can also be set with the setgenerate call.\n"
            "\nResult\n"
            "true|false      (boolean) If the server is set to generate coins or not\n"
            "\nExamples:\n"
            + HelpExampleCli("getgenerate", "")
            + HelpExampleRpc("getgenerate", "")
        );

    LOCK(cs_main);
    return GetBoolArg("-gen", DEFAULT_GENERATE);
}

static UniValue generate(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() < 1 || request.params.size() > 2)
        throw std::runtime_error(
            "generate num_blocks ( max_tries )\n"
            "\ngenerate up to n blocks immediately (before the RPC call returns)\n"
            "\nArguments:\n"
            "1. nblocks      (numeric, required) How many blocks are generated immediately.\n"
            "2. maxtries     (numeric, optional) How many iterations to try (default = 1000000).\n"
            "\nResult:\n"
            "[ blockhashes ]     (array) hashes of blocks generated\n"
            "\nExamples:\n"
            "\nGenerate 11 blocks\n"
            + HelpExampleCli("generate", "11")
        );

    int nGenerate = request.params[0].get_int();
    uint64_t nMaxTries = 1000000;
    if (request.params.size() > 1) {
        nMaxTries = request.params[1].get_int();
    }

    std::shared_ptr<CReserveKeyOrScript> coinbaseScript;
    GetMainSignals().ScriptForMining(coinbaseScript, NULL);

    // If the keypool is exhausted, no script is returned at all.  Catch this.
    if (!coinbaseScript)
        throw JSONRPCError(RPC_WALLET_KEYPOOL_RAN_OUT, "Error: Keypool ran out, please call keypoolrefill first");

    //throw an error if no script was provided
    if (coinbaseScript->reserveScript.empty())
        throw JSONRPCError(RPC_INTERNAL_ERROR, "No coinbase script available; a wallet is required");

    return generateBlocks(coinbaseScript, nGenerate, nMaxTries, true);
}

static UniValue setgenerate(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() < 1 || request.params.size() > 2)
        throw std::runtime_error(
            "setgenerate generate ( gen_proc_limit )\n"
            "\nSet 'generate' true or false to turn generation on or off.\n"
            "Generation is limited to 'gen_proc_limit' processors, -1 is unlimited.\n"
            "See the getgenerate call for the current setting.\n"
            "\nArguments:\n"
            "1. generate         (boolean, required) Set to true to turn on generation, off to turn off.\n"
            "2. gen_proc_limit   (numeric, optional) Set the processor limit for when generation is on. Can be -1 for unlimited.\n"
            "\nExamples:\n"
            "\nSet the generation on with a limit of one processor\n"
            + HelpExampleCli("setgenerate", "true 1") +
            "\nCheck the setting\n"
            + HelpExampleCli("getgenerate", "") +
            "\nTurn off generation\n"
            + HelpExampleCli("setgenerate", "false") +
            "\nUsing json rpc\n"
            + HelpExampleRpc("setgenerate", "true, 1")
        );

    if (Params().MineBlocksOnDemand())
        throw JSONRPCError(RPC_METHOD_NOT_FOUND, "Use the generate method instead of setgenerate on this network");

    #ifdef ENABLE_WALLET
    CWallet* const pwallet = GetWalletForJSONRPCRequest(request);
    if (!pwallet)
        throw std::runtime_error("Cannot use command without an active wallet");

    if (!pactiveWallet->activeAccount)
        throw std::runtime_error("No active account selected, first select an active account.");

    bool fGenerate = true;
    if (request.params.size() > 0)
        fGenerate = request.params[0].get_bool();

    if (fGenerate && pactiveWallet->activeAccount->IsPoW2Witness())
        throw std::runtime_error("Witness account selected, first select a regular account as the active account.");

    int nGenProcLimit = GetArg("-genproclimit", DEFAULT_GENERATE_THREADS);
    if (request.params.size() > 1)
    {
        nGenProcLimit = request.params[1].get_int();
        if (nGenProcLimit == 0)
            fGenerate = false;
    }

    SoftSetBoolArg("-gen", fGenerate);
    SoftSetArg("-genproclimit", itostr(nGenProcLimit));
    PoWMineGulden(fGenerate, nGenProcLimit, Params());

    if (!fGenerate)
    {
        return "Block generation disabled.";
    }
    else
    {
        return strprintf("Block generation enabled into account [%s], thread limit: [%d].", pwallet->mapAccountLabels[pwallet->activeAccount->getUUID()] ,nGenProcLimit);
    }
    #else
    throw std::runtime_error("Cannot use command without an active wallet");
    return nullptr;
    #endif
}

static UniValue generatetoaddress(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() < 2 || request.params.size() > 3)
        throw std::runtime_error(
            "generatetoaddress num_blocks address (max_tries)\n"
            "\nGenerate blocks immediately to a specified address (before the RPC call returns)\n"
            "\nArguments:\n"
            "1. nblocks      (numeric, required) How many blocks are generated immediately.\n"
            "2. address      (string, required) The address to send the newly generated Gulden to.\n"
            "3. maxtries     (numeric, optional) How many iterations to try (default = 1000000).\n"
            "\nResult:\n"
            "[ blockhashes ]     (array) hashes of blocks generated\n"
            "\nExamples:\n"
            "\nGenerate 11 blocks to myaddress\n"
            + HelpExampleCli("generatetoaddress", "11 \"myaddress\"")
        );

    int nGenerate = request.params[0].get_int();
    uint64_t nMaxTries = 1000000;
    if (request.params.size() > 2) {
        nMaxTries = request.params[2].get_int();
    }

    CGuldenAddress address(request.params[1].get_str());
    if (!address.IsValid())
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Error: Invalid address");

    std::shared_ptr<CReserveKeyOrScript> coinbaseScript = std::make_shared<CReserveKeyOrScript>();
    coinbaseScript->reserveScript = GetScriptForDestination(address.Get());

    return generateBlocks(coinbaseScript, nGenerate, nMaxTries, false);
}

static UniValue getmininginfo(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() != 0)
        throw std::runtime_error(
            "getmininginfo\n"
            "\nReturns a json object containing information about block generation."
            "\nResult:\n"
            "{\n"
            "  \"blocks\": nnn,             (numeric) The current block\n"
            "  \"currentblocksize\": nnn,   (numeric) The last block size\n"
            "  \"currentblockweight\": nnn, (numeric) The last block weight\n"
            "  \"currentblocktx\": nnn,     (numeric) The last block transaction\n"
            "  \"difficulty\": xxx.xxxxx    (numeric) The current difficulty\n"
            "  \"errors\": \"...\"            (string) Current errors\n"
            "  \"networkhashps\": nnn,      (numeric) The network hashes per second\n"
            "  \"generate\": true|false     (boolean) If the generation is on or off (see getgenerate or setgenerate calls)\n"
            "  \"genproclimit\": n          (numeric) The processor limit for generation. -1 if no generation. (see getgenerate or setgenerate calls)\n"
            "  \"pooledtx\": n              (numeric) The size of the mempool\n"
            "  \"chain\": \"xxxx\",           (string) current network name as defined in BIP70 (main, test, regtest)\n"
            "}\n"
            "\nExamples:\n"
            + HelpExampleCli("getmininginfo", "")
            + HelpExampleRpc("getmininginfo", "")
        );


    LOCK(cs_main);

    UniValue obj(UniValue::VOBJ);
    obj.push_back(Pair("blocks",           (int)chainActive.Height()));
    obj.push_back(Pair("currentblocksize", (uint64_t)nLastBlockSize));
    obj.push_back(Pair("currentblockweight", (uint64_t)nLastBlockWeight));
    obj.push_back(Pair("currentblocktx",   (uint64_t)nLastBlockTx));
    obj.push_back(Pair("difficulty",       (double)GetDifficulty()));
    obj.push_back(Pair("errors",           GetWarnings("statusbar")));
    obj.push_back(Pair("genproclimit",     (int)GetArg("-genproclimit", DEFAULT_GENERATE_THREADS)));
    obj.push_back(Pair("networkhashps",    getnetworkhashps(request)));
    obj.push_back(Pair("pooledtx",         (uint64_t)mempool.size()));
    obj.push_back(Pair("chain",            Params().NetworkIDString()));
    obj.push_back(Pair("generate",         getgenerate(request)));
    return obj;
}


// NOTE: Unlike wallet RPC (which use NLG values), mining RPCs follow GBT (BIP 22) in using satoshi amounts
static UniValue prioritisetransaction(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() != 3)
        throw std::runtime_error(
            "prioritisetransaction <txid> <dummy_value> <fee_delta>\n"
            "Accepts the transaction into generated blocks at a higher (or lower) priority\n"
            "\nArguments:\n"
            "1. \"txid\"         (string, required) The transaction id.\n"
            "2. dummy_value    (numeric, optional) API-Compatibility for previous API. Must be zero or null.\n"
            "                  DEPRECATED. For forward compatibility use named arguments and omit this parameter.\n"
            "3. fee_delta      (numeric, required) The fee value (in satoshis) to add (or subtract, if negative).\n"
            "                  The fee is not actually paid, only the algorithm for selecting transactions into a block\n"
            "                  considers the transaction as it would have paid a higher (or lower) fee.\n"
            "\nResult:\n"
            "true              (boolean) Returns true\n"
            "\nExamples:\n"
            + HelpExampleCli("prioritisetransaction", "\"txid\" 0.0 10000")
            + HelpExampleRpc("prioritisetransaction", "\"txid\", 0.0, 10000")
        );

    LOCK(cs_main);

    uint256 hash = ParseHashStr(request.params[0].get_str(), "txid");
    CAmount nAmount = request.params[2].get_int64();

    if (!(request.params[1].isNull() || request.params[1].get_real() == 0)) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Priority is no longer supported, dummy argument to prioritisetransaction must be 0.");
    }

    mempool.PrioritiseTransaction(hash, nAmount);
    return true;
}


// NOTE: Assumes a conclusive result; if result is inconclusive, it must be handled by caller
static UniValue BIP22ValidationResult(const CValidationState& state)
{
    if (state.IsValid())
        return NullUniValue;

    std::string strRejectReason = state.GetRejectReason();
    if (state.IsError())
        throw JSONRPCError(RPC_VERIFY_ERROR, strRejectReason);
    if (state.IsInvalid())
    {
        if (strRejectReason.empty())
            return "rejected";
        return strRejectReason;
    }
    // Should be impossible
    return "valid?";
}

static std::string gbt_vb_name(const Consensus::DeploymentPos pos) {
    const struct VBDeploymentInfo& vbinfo = VersionBitsDeploymentInfo[pos];
    std::string s = vbinfo.name;
    if (!vbinfo.gbt_force) {
        s.insert(s.begin(), '!');
    }
    return s;
}

static UniValue getblocktemplate(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() > 1)
        throw std::runtime_error(
            "getblocktemplate ( TemplateRequest )\n"
            "\nIf the request parameters include a 'mode' key, that is used to explicitly select between the default 'template' request or a 'proposal'.\n"
            "It returns data needed to construct a block to work on.\n"
            "For full specification, see BIPs 22, 23, 9, and 145:\n"
            "    https://github.com/bitcoin/bips/blob/master/bip-0022.mediawiki\n"
            "    https://github.com/bitcoin/bips/blob/master/bip-0023.mediawiki\n"
            "    https://github.com/bitcoin/bips/blob/master/bip-0009.mediawiki#getblocktemplate_changes\n"
            "    https://github.com/bitcoin/bips/blob/master/bip-0145.mediawiki\n"

            "\nArguments:\n"
            "1. template_request         (json object, optional) A json object in the following spec\n"
            "     {\n"
            "       \"mode\":\"template\"    (string, optional) This must be set to \"template\", \"proposal\" (see BIP 23), or omitted\n"
            "       \"capabilities\":[     (array, optional) A list of strings\n"
            "           \"support\"          (string) client side supported feature, 'longpoll', 'coinbasetxn', 'coinbasevalue', 'proposal', 'serverlist', 'workid'\n"
            "           ,...\n"
            "       ],\n"
            "       \"rules\":[            (array, optional) A list of strings\n"
            "           \"support\"          (string) client side supported softfork deployment\n"
            "           ,...\n"
            "       ]\n"
            "     }\n"
            "\n"

            "\nResult:\n"
            "{\n"
            "  \"version\" : n,                    (numeric) The preferred block version\n"
            "  \"rules\" : [ \"rulename\", ... ],    (array of strings) specific block rules that are to be enforced\n"
            "  \"vbavailable\" : {                 (json object) set of pending, supported versionbit (BIP 9) softfork deployments\n"
            "      \"rulename\" : bitnumber          (numeric) identifies the bit number as indicating acceptance and readiness for the named softfork rule\n"
            "      ,...\n"
            "  },\n"
            "  \"vbrequired\" : n,                 (numeric) bit mask of versionbits the server requires set in submissions\n"
            "  \"previousblockhash\" : \"xxxx\",     (string) The hash of current highest block\n"
            "  \"transactions\" : [                (array) contents of non-coinbase transactions that should be included in the next block\n"
            "      {\n"
            "         \"data\" : \"xxxx\",             (string) transaction data encoded in hexadecimal (byte-for-byte)\n"
            "         \"txid\" : \"xxxx\",             (string) transaction id encoded in little-endian hexadecimal\n"
            "         \"hash\" : \"xxxx\",             (string) hash encoded in little-endian hexadecimal (including witness data)\n"
            "         \"depends\" : [                (array) array of numbers \n"
            "             n                          (numeric) transactions before this one (by 1-based index in 'transactions' list) that must be present in the final block if this one is\n"
            "             ,...\n"
            "         ],\n"
            "         \"fee\": n,                    (numeric) difference in value between transaction inputs and outputs (in Satoshis); for coinbase transactions, this is a negative Number of the total collected block fees (ie, not including the block subsidy); if key is not present, fee is unknown and clients MUST NOT assume there isn't one\n"
            "         \"sigops\" : n,                (numeric) total SigOps cost, as counted for purposes of block limits; if key is not present, sigop cost is unknown and clients MUST NOT assume it is zero\n"
            "         \"weight\" : n,                (numeric) total transaction weight, as counted for purposes of block limits\n"
            "         \"required\" : true|false      (boolean) if provided and true, this transaction must be in the final block\n"
            "      }\n"
            "      ,...\n"
            "  ],\n"
            "  \"coinbaseaux\" : {                 (json object) data that should be included in the coinbase's scriptSig content\n"
            "      \"flags\" : \"xx\"                  (string) key name is to be ignored, and value included in scriptSig\n"
            "  },\n"
            "  \"coinbasevalue\" : n,              (numeric) maximum allowable input to coinbase transaction, including the generation award and transaction fees (in Satoshis)\n"
            "  \"coinbasetxn\" : { ... },          (json object) information for coinbase transaction\n"
            "  \"target\" : \"xxxx\",                (string) The hash target\n"
            "  \"mintime\" : xxx,                  (numeric) The minimum timestamp appropriate for next block time in seconds since epoch (Jan 1 1970 GMT)\n"
            "  \"mutable\" : [                     (array of string) list of ways the block template may be changed \n"
            "     \"value\"                          (string) A way the block template may be changed, e.g. 'time', 'transactions', 'prevblock'\n"
            "     ,...\n"
            "  ],\n"
            "  \"noncerange\" : \"00000000ffffffff\",(string) A range of valid nonces\n"
            "  \"sigoplimit\" : n,                 (numeric) limit of sigops in blocks\n"
            "  \"sizelimit\" : n,                  (numeric) limit of block size\n"
            "  \"weightlimit\" : n,                (numeric) limit of block weight\n"
            "  \"curtime\" : ttt,                  (numeric) current timestamp in seconds since epoch (Jan 1 1970 GMT)\n"
            "  \"bits\" : \"xxxxxxxx\",              (string) compressed target of next block\n"
            "  \"height\" : n                      (numeric) The height of the next block\n"
            "}\n"

            "\nExamples:\n"
            + HelpExampleCli("getblocktemplate", "")
            + HelpExampleRpc("getblocktemplate", "")
         );

    LOCK(cs_main);

    std::string strMode = "template";
    UniValue lpval = NullUniValue;
    std::set<std::string> setClientRules;
    int64_t nMaxVersionPreVB = -1;
    if (request.params.size() > 0)
    {
        const UniValue& oparam = request.params[0].get_obj();
        const UniValue& modeval = find_value(oparam, "mode");
        if (modeval.isStr())
            strMode = modeval.get_str();
        else if (modeval.isNull())
        {
            /* Do nothing */
        }
        else
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid mode");
        lpval = find_value(oparam, "longpollid");

        if (strMode == "proposal")
        {
            const UniValue& dataval = find_value(oparam, "data");
            if (!dataval.isStr())
                throw JSONRPCError(RPC_TYPE_ERROR, "Missing data String key for proposal");

            CBlock block;
            if (!DecodeHexBlk(block, dataval.get_str()))
                throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "Block decode failed");

            uint256 hash = block.GetHashPoW2();
            BlockMap::iterator mi = mapBlockIndex.find(hash);
            if (mi != mapBlockIndex.end()) {
                CBlockIndex *pindex = mi->second;
                if (pindex->IsValid(BLOCK_VALID_SCRIPTS))
                    return "duplicate";
                if (pindex->nStatus & BLOCK_FAILED_MASK)
                    return "duplicate-invalid";
                return "duplicate-inconclusive";
            }

            // Phase 3 onward no longer necessarily mine on the chain tip
            // We use 'FindMiningTip' to find the block that we should be mining on
            // Then run 'TestBlockValidity' on a clone chain to check the result.
            std::string strError;
            CBlockIndex* pWitnessBlockToEmbed = nullptr;
            CBlockIndex* pIndexMiningTip = FindMiningTip(chainActive.Tip(), Params(), strError, pWitnessBlockToEmbed);

            if (block.hashPrevBlock != pIndexMiningTip->GetBlockHashPoW2())
                return "inconclusive-not-best-prevblk";

            CValidationState state;
            if (pIndexMiningTip->nHeight < GetPhase2ActivationHeight())
            {
                return "rejected";
            }
            else
            {
                CCoinsViewCache viewNew(pcoinsTip);
                CBlockIndex* pindexPrev_ = nullptr;
                CCloneChain tempChain(chainActive, GetPow2ValidationCloneHeight(chainActive, pIndexMiningTip, 1), pIndexMiningTip, pindexPrev_);
                //fixme: (2.0.1) error handling.
                assert(pindexPrev_);
                ForceActivateChain(pindexPrev_, nullptr, state, Params(), tempChain, viewNew);

                TestBlockValidity(tempChain, state, Params(), block, pindexPrev_, false, true, &viewNew);
            }

            return BIP22ValidationResult(state);
        }

        const UniValue& aClientRules = find_value(oparam, "rules");
        if (aClientRules.isArray()) {
            for (unsigned int i = 0; i < aClientRules.size(); ++i) {
                const UniValue& v = aClientRules[i];
                setClientRules.insert(v.get_str());
            }
        } else {
            // NOTE: It is important that this NOT be read if versionbits is supported
            const UniValue& uvMaxVersion = find_value(oparam, "maxversion");
            if (uvMaxVersion.isNum()) {
                nMaxVersionPreVB = uvMaxVersion.get_int64();
            }
        }
    }

    if (strMode != "template")
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid mode");

    if(!g_connman)
        throw JSONRPCError(RPC_CLIENT_P2P_DISABLED, "Error: Peer-to-peer functionality missing or disabled");

    /*if (g_connman->GetNodeCount(CConnman::CONNECTIONS_ALL) == 0)
        throw JSONRPCError(RPC_CLIENT_NOT_CONNECTED, "Gulden is not connected!");

    if (IsInitialBlockDownload())
        throw JSONRPCError(RPC_CLIENT_IN_INITIAL_DOWNLOAD, "Gulden is downloading blocks...");*/

    static unsigned int nTransactionsUpdatedLast;

    bool forceBlockUpdate = false;
    static int64_t nStart;
    if (!lpval.isNull())
    {
        // Wait to respond until either the best block changes, OR a minute has passed and there are more transactions
        uint256 hashWatchedChain;
        boost::system_time checktxtime;
        unsigned int nTransactionsUpdatedLastLP;
        CBlockHeader blockUpdatedLastLP;

        if (lpval.isStr())
        {
            // Format: <hashBestChain><nTransactionsUpdatedLast>
            std::string lpstr = lpval.get_str();

            hashWatchedChain.SetHex(lpstr.substr(0, 64));
            nTransactionsUpdatedLastLP = atoi64(lpstr.substr(64));
        }
        else
        {
            // NOTE: Spec does not specify behaviour for non-string longpollid, but this makes testing easier
            hashWatchedChain = chainActive.Tip()->GetBlockHashPoW2();
            nTransactionsUpdatedLastLP = nTransactionsUpdatedLast;
        }
        UpdateTime(&blockUpdatedLastLP, Params().GetConsensus(), chainActive.Tip());
        blockUpdatedLastLP.nBits = GetNextWorkRequired(chainActive.Tip(), &blockUpdatedLastLP, Params().GetConsensus());


        forceBlockUpdate = false;
        // Release the wallet and main lock while waiting
        LEAVE_CRITICAL_SECTION(cs_main);
        {
            checktxtime = boost::get_system_time() + boost::posix_time::minutes(1);

            boost::unique_lock<boost::mutex> lock(csBestBlock);
            while (chainActive.Tip()->GetBlockHashPoW2() == hashWatchedChain && IsRPCRunning())
            {
                if (!cvBlockChange.timed_wait(lock, checktxtime))
                {
                    // Timeout: Check transactions for update
                    if (mempool.GetTransactionsUpdated() != nTransactionsUpdatedLastLP)
                    {
                        forceBlockUpdate = true;
                        break;
                    }
                    UpdateTime(&blockUpdatedLastLP, Params().GetConsensus(), chainActive.Tip());
                    if (GetNextWorkRequired(chainActive.Tip(), &blockUpdatedLastLP, Params().GetConsensus()) != blockUpdatedLastLP.nBits)
                    {
                        forceBlockUpdate = true;
                        break;
                    }
                    checktxtime += boost::posix_time::seconds(10);
                }
            }
        }
        ENTER_CRITICAL_SECTION(cs_main);

        if (!IsRPCRunning())
            throw JSONRPCError(RPC_CLIENT_NOT_CONNECTED, "Shutting down");
        // TODO: Maybe recheck connections/IBD and (if something wrong) send an expires-immediately template to stop miners?
    }
    else
    {
        if (GetTime() - nStart > 20)
        {
            forceBlockUpdate = true;
        }
    }
	
    forceBlockUpdate = true;

    static std::vector<unsigned char> witnessCoinbaseHex;
    static std::vector<unsigned char> witnessSubsidyHex;
    static CAmount amountPoW2Subsidy = 0;

    // Update block
    static CBlockIndex* pindexPrevChainTip=nullptr;
    static CBlockIndex* pindexPrev=nullptr;
    static std::unique_ptr<CBlockTemplate> pblocktemplate;
    if (forceBlockUpdate || pindexPrevChainTip != chainActive.Tip() || (mempool.GetTransactionsUpdated() != nTransactionsUpdatedLast && GetTime() - nStart > 5))
    {
        // Clear pindexPrev so future calls make a new block, despite any failures from here on
        pindexPrev = nullptr;
        pindexPrevChainTip = nullptr;

         // Phase 3 onward no longer necessarily mine on the chain tip
        // We use 'FindMiningTip' to find the block that we should be mining on
        // Then run 'TestBlockValidity' on a clone chain to check the result.
        std::string strError;
        CBlockIndex* pWitnessBlockToEmbed = nullptr;
        CBlockIndex* pIndexMiningTip = FindMiningTip(chainActive.Tip(), Params(), strError, pWitnessBlockToEmbed);

        if (!pIndexMiningTip)
        {
            throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "Failed to determine chain tip");
        }

        // Store the pindexBest used before CreateNewBlock, to avoid races
        nTransactionsUpdatedLast = mempool.GetTransactionsUpdated();
        CBlockIndex* pindexPrevNew = pIndexMiningTip;
        CBlockIndex* pChainActiveTipNew = chainActive.Tip();
        nStart = GetTime();

        // Create new block
        CScript scriptDummy = CScript() << OP_TRUE;
        std::shared_ptr<CReserveKeyOrScript> reservedScript = std::make_shared<CReserveKeyOrScript>(scriptDummy);
        pblocktemplate = BlockAssembler(Params()).CreateNewBlock(pIndexMiningTip, reservedScript, true, pWitnessBlockToEmbed, false, &witnessCoinbaseHex, &witnessSubsidyHex, &amountPoW2Subsidy);
        if (!pblocktemplate)
            throw JSONRPCError(RPC_INTERNAL_ERROR, "Failed to create new block, read GuldenD debug.log for more information");

        // Need to update only after we know CreateNewBlock succeeded
        pindexPrev = pindexPrevNew;
        pindexPrevChainTip = pChainActiveTipNew;
    }
    CBlock* pblock = &pblocktemplate->block; // pointer for convenience
    const Consensus::Params& consensusParams = Params().GetConsensus();

    // Update nTime
    UpdateTime(pblock, consensusParams, pindexPrev);
    pblock->nNonce = 0;

    UniValue aCaps(UniValue::VARR); aCaps.push_back("proposal");

    UniValue transactions(UniValue::VARR);
    std::map<uint256, int64_t> setTxIndex;
    unsigned int i = 0;
    for (const auto& it : pblock->vtx) {
        const CTransaction& tx = *it;
        uint256 txHash = tx.GetHash();
        setTxIndex[txHash] = i++;

        if (tx.IsCoinBase())
            continue;

        UniValue entry(UniValue::VOBJ);

        entry.push_back(Pair("data", EncodeHexTx(tx)));
        entry.push_back(Pair("txid", txHash.GetHex()));
        entry.push_back(Pair("hash", tx.GetWitnessHash().GetHex()));

        UniValue deps(UniValue::VARR);
        for (const CTxIn &in : tx.vin)
        {
            if (setTxIndex.count(in.prevout.getHash()))
                deps.push_back(setTxIndex[in.prevout.getHash()]);
        }
        entry.push_back(Pair("depends", deps));

        unsigned int index_in_template = i - 1; // i >= 1 guaranteed here
        if (index_in_template >= pblocktemplate->vTxFees.size())
        {
            //fixme: (2.1) remove
            entry.push_back(Pair("fee", 0));
        }
        else
        {
            entry.push_back(Pair("fee", pblocktemplate->vTxFees[index_in_template]));
        }
        if (index_in_template >= pblocktemplate->vTxSigOpsCost.size())
        {
            //fixme: (2.1) remove
            entry.push_back(Pair("sigops", 0));
            entry.push_back(Pair("weight", 0));
        }
        else
        {
            int64_t nTxSigOps = pblocktemplate->vTxSigOpsCost[index_in_template];
            entry.push_back(Pair("sigops", nTxSigOps));
            entry.push_back(Pair("weight", GetTransactionWeight(tx)));
        }

        transactions.push_back(entry);
    }

    UniValue aux(UniValue::VOBJ);
    aux.push_back(Pair("flags", HexStr(COINBASE_FLAGS.begin(), COINBASE_FLAGS.end())));

    arith_uint256 hashTarget = arith_uint256().SetCompact(pblock->nBits);

    UniValue aMutable(UniValue::VARR);
    aMutable.push_back("time");
    aMutable.push_back("transactions");
    aMutable.push_back("prevblock");

    UniValue result(UniValue::VOBJ);
    result.push_back(Pair("capabilities", aCaps));

    // In phase 3 we are restricted to using the same version bits as the witness; we aren't allowed to modify these bits.
    bool versionBitsLockedToWitness = IsPow2Phase3Active(pindexPrev->nHeight) || (pindexPrev->pprev && IsPow2Phase3Active(pindexPrev->pprev->nHeight));
    UniValue vbavailable(UniValue::VOBJ);
    UniValue aRules(UniValue::VARR);
    for (int j = 0; j < (int)Consensus::MAX_VERSION_BITS_DEPLOYMENTS; ++j) {
        Consensus::DeploymentPos pos = Consensus::DeploymentPos(j);
        ThresholdState state = VersionBitsState(pindexPrev, consensusParams, pos, versionbitscache);
        switch (state) {
            case THRESHOLD_DEFINED:
            case THRESHOLD_FAILED:
                // Not exposed to GBT at all
                break;
            case THRESHOLD_LOCKED_IN:
                // Ensure bit is set in block version
                if (!versionBitsLockedToWitness)
                {
                    pblock->nVersion |= VersionBitsMask(consensusParams, pos);
                }
                // FALL THROUGH to get vbavailable set...
            case THRESHOLD_STARTED:
            {
                const struct VBDeploymentInfo& vbinfo = VersionBitsDeploymentInfo[pos];
                vbavailable.push_back(Pair(gbt_vb_name(pos), consensusParams.vDeployments[pos].bit));
                if (setClientRules.find(vbinfo.name) == setClientRules.end()) {
                    if (!vbinfo.gbt_force)
                    {
                        if (!versionBitsLockedToWitness)
                        {
                            // If the client doesn't support this, don't indicate it in the [default] version
                            pblock->nVersion &= ~VersionBitsMask(consensusParams, pos);
                        }
                    }
                }
                break;
            }
            case THRESHOLD_ACTIVE:
            {
                // Add to rules only
                const struct VBDeploymentInfo& vbinfo = VersionBitsDeploymentInfo[pos];
                aRules.push_back(gbt_vb_name(pos));
                if (setClientRules.find(vbinfo.name) == setClientRules.end()) {
                    // Not supported by the client; make sure it's safe to proceed
                    if (!vbinfo.gbt_force) {
                        // If we do anything other than throw an exception here, be sure version/force isn't sent to old clients
                        throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Support for '%s' rule requires explicit client support", vbinfo.name));
                    }
                }
                break;
            }
        }
    }
    result.push_back(Pair("version", pblock->nVersion));
    result.push_back(Pair("rules", aRules));
    result.push_back(Pair("vbavailable", vbavailable));
    result.push_back(Pair("vbrequired", int(0)));

    if (nMaxVersionPreVB >= 2) {
        // If VB is supported by the client, nMaxVersionPreVB is -1, so we won't get here
        // Because BIP 34 changed how the generation transaction is serialized, we can only use version/force back to v2 blocks
        // This is safe to do [otherwise-]unconditionally only because we are throwing an exception above if a non-force deployment gets activated
        // Note that this can probably also be removed entirely after the first BIP9 non-force deployment (ie, probably segwit) gets activated
        aMutable.push_back("version/force");
    }

    result.push_back(Pair("previousblockhash", pblock->hashPrevBlock.GetHex()));
    result.push_back(Pair("transactions", transactions));
    result.push_back(Pair("coinbaseaux", aux));
    result.push_back(Pair("coinbasevalue", (int64_t)pblock->vtx[0]->vout[0].nValue));
    result.push_back(Pair("longpollid", chainActive.Tip()->GetBlockHashPoW2().GetHex() + i64tostr(nTransactionsUpdatedLast)));
    result.push_back(Pair("target", hashTarget.GetHex()));
    result.push_back(Pair("mintime", (int64_t)std::max(pindexPrev->GetMedianTimePastWitness()+1, GetTime())));
    //fixme: (Post-2.1) - Implement 'maxtime' here?
    result.push_back(Pair("mutable", aMutable));
    result.push_back(Pair("noncerange", "00000000ffffffff"));
    int64_t nSigOpLimit = MAX_BLOCK_SIGOPS_COST;
    result.push_back(Pair("sigoplimit", nSigOpLimit));

    //fixme: (2.1) Double check this doesn't result in miners mining smaller blocks or anything strange
    result.push_back(Pair("sizelimit", (int64_t)MAX_BLOCK_SERIALIZED_SIZE));
    result.push_back(Pair("weightlimit", (int64_t)MAX_BLOCK_WEIGHT));

    //fixme: (2.1) remove
    result.push_back(Pair("pow2_aux1", HexStr(witnessCoinbaseHex)));
    result.push_back(Pair("pow2_aux2", HexStr(witnessSubsidyHex)));
    result.push_back(Pair("pow2_subsidy", amountPoW2Subsidy));

    result.push_back(Pair("curtime", pblock->GetBlockTime()));
    result.push_back(Pair("bits", strprintf("%08x", pblock->nBits)));
    result.push_back(Pair("height", (int64_t)(pindexPrev->nHeight+1)));

    return result;
}

class submitblock_StateCatcher : public CValidationInterface
{
public:
    uint256 hash;
    bool found;
    CValidationState state;

    submitblock_StateCatcher(const uint256 &hashIn) : hash(hashIn), found(false), state() {}

protected:
    void BlockChecked(const CBlock& block, const CValidationState& stateIn) override {
        if (block.GetHashPoW2() != hash)
            return;
        found = true;
        state = stateIn;
    }
};

static UniValue submitblock(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() < 1 || request.params.size() > 2) {
        throw std::runtime_error(
            "submitblock \"hexdata\" ( \"jsonparametersobject\" )\n"
            "\nAttempts to submit new block to network.\n"
            "The 'jsonparametersobject' parameter is currently ignored.\n"
            "See https://en.bitcoin.it/wiki/BIP_0022 for full specification.\n"

            "\nArguments\n"
            "1. \"hexdata\"        (string, required) the hex-encoded block data to submit\n"
            "2. \"parameters\"     (string, optional) object of optional parameters\n"
            "    {\n"
            "      \"workid\" : \"id\"    (string, optional) if the server provided a workid, it MUST be included with submissions\n"
            "    }\n"
            "\nResult:\n"
            "\nExamples:\n"
            + HelpExampleCli("submitblock", "\"mydata\"")
            + HelpExampleRpc("submitblock", "\"mydata\"")
        );
    }

    std::shared_ptr<CBlock> blockptr = std::make_shared<CBlock>();
    CBlock& block = *blockptr;
    if (!DecodeHexBlk(block, request.params[0].get_str())) {
        throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "Block decode failed");
    }

    if (block.vtx.empty() || !block.vtx[0]->IsCoinBase()) {
        throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "Block does not start with a coinbase");
    }

    uint256 hash = block.GetHashPoW2();
    bool fBlockPresent = false;
    {
        LOCK(cs_main);
        BlockMap::iterator mi = mapBlockIndex.find(hash);
        if (mi != mapBlockIndex.end()) {
            CBlockIndex *pindex = mi->second;
            if (pindex->IsValid(BLOCK_VALID_SCRIPTS)) {
                return "duplicate";
            }
            if (pindex->nStatus & BLOCK_FAILED_MASK) {
                return "duplicate-invalid";
            }
            // Otherwise, we might only have the header - process the block before returning
            fBlockPresent = true;
        }
    }

    submitblock_StateCatcher sc(block.GetHashPoW2());
    RegisterValidationInterface(&sc);
    bool fAccepted = ProcessNewBlock(Params(), blockptr, true, NULL);
    UnregisterValidationInterface(&sc);


    if (fBlockPresent) {
        if (fAccepted && !sc.found) {
            return "duplicate-inconclusive";
        }
        return "duplicate";
    }
    if (!fAccepted)
        return "invalid";
    if (!sc.found) {
        return "inconclusive";
    }
    return BIP22ValidationResult(sc.state);
}

static UniValue estimatefee(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() != 1)
        throw std::runtime_error(
            "estimatefee num_blocks\n"
            "\nDEPRECATED. Please use estimatesmartfee for more intelligent estimates."
            "\nEstimates the approximate fee per kilobyte needed for a transaction to begin\n"
            "confirmation within num_blocks blocks. Uses virtual transaction size of transaction\n"
            "as defined in BIP 141 (witness data is discounted).\n"
            "\nArguments:\n"
            "1. num_blocks  (numeric, required)\n"
            "\nResult:\n"
            "n              (numeric) estimated fee-per-kilobyte\n"
            "\n"
            "A negative value is returned if not enough transactions and blocks\n"
            "have been observed to make an estimate.\n"
            "-1 is always returned for num_blocks == 1 as it is impossible to calculate\n"
            "a fee that is high enough to get reliably included in the next block.\n"
            "\nExample:\n"
            + HelpExampleCli("estimatefee", "6")
            );

    RPCTypeCheck(request.params, {UniValue::VNUM});

    int nBlocks = request.params[0].get_int();
    if (nBlocks < 1)
        nBlocks = 1;

    CFeeRate feeRate = ::feeEstimator.estimateFee(nBlocks);
    if (feeRate == CFeeRate(0))
        return -1.0;

    return ValueFromAmount(feeRate.GetFeePerK());
}

static UniValue estimatesmartfee(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() < 1 || request.params.size() > 2)
        throw std::runtime_error(
            "estimatesmartfee num_blocks (conservative)\n"
            "\nEstimates the approximate fee per kilobyte needed for a transaction to begin\n"
            "confirmation within num_blocks blocks if possible and return the number of blocks\n"
            "for which the estimate is valid. Uses virtual transaction size as defined\n"
            "in BIP 141 (witness data is discounted).\n"
            "\nArguments:\n"
            "1. num_blocks    (numeric)\n"
            "2. conservative  (bool, optional, default=true) Whether to return a more conservative estimate which\n"
            "                 also satisfies a longer history. A conservative estimate potentially returns a higher\n"
            "                 feerate and is more likely to be sufficient for the desired target, but is not as\n"
            "                 responsive to short term drops in the prevailing fee market\n"
            "\nResult:\n"
            "{\n"
            "  \"feerate\" : x.x,     (numeric) estimate fee-per-kilobyte (in NLG)\n"
            "  \"blocks\" : n         (numeric) block number where estimate was found\n"
            "}\n"
            "\n"
            "A negative value is returned if not enough transactions and blocks\n"
            "have been observed to make an estimate for any number of blocks.\n"
            "However it will not return a value below the mempool reject fee.\n"
            "\nExample:\n"
            + HelpExampleCli("estimatesmartfee", "6")
            );

    RPCTypeCheck(request.params, {UniValue::VNUM});

    int nBlocks = request.params[0].get_int();
    bool conservative = true;
    if (request.params.size() > 1 && !request.params[1].isNull()) {
        RPCTypeCheckArgument(request.params[1], UniValue::VBOOL);
        conservative = request.params[1].get_bool();
    }

    UniValue result(UniValue::VOBJ);
    int answerFound;
    CFeeRate feeRate = ::feeEstimator.estimateSmartFee(nBlocks, &answerFound, ::mempool, conservative);
    result.push_back(Pair("feerate", feeRate == CFeeRate(0) ? -1.0 : ValueFromAmount(feeRate.GetFeePerK())));
    result.push_back(Pair("blocks", answerFound));
    return result;
}

static UniValue estimaterawfee(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() < 1|| request.params.size() > 3)
        throw std::runtime_error(
            "estimaterawfee num_blocks (threshold horizon)\n"
            "\nWARNING: This interface is unstable and may disappear or change!\n"
            "\nWARNING: This is an advanced API call that is tightly coupled to the specific\n"
            "         implementation of fee estimation. The parameters it can be called with\n"
            "         and the results it returns will change if the internal implementation changes.\n"
            "\nEstimates the approximate fee per kilobyte needed for a transaction to begin\n"
            "confirmation within num_blocks blocks if possible. Uses virtual transaction size as defined\n"
            "in BIP 141 (witness data is discounted).\n"
            "\nArguments:\n"
            "1. num_blocks  (numeric)\n"
            "2. threshold   (numeric, optional) The proportion of transactions in a given feerate range that must have been\n"
            "               confirmed within num_blocks in order to consider those feerates as high enough and proceed to check\n"
            "               lower buckets.  Default: 0.95\n"
            "3. horizon     (numeric, optional) How long a history of estimates to consider. 0=short, 1=medium, 2=long.\n"
            "               Default: 1\n"
            "\nResult:\n"
            "{\n"
            "  \"feerate\" : x.x,        (numeric) estimate fee-per-kilobyte (in NLG)\n"
            "  \"decay\" : x.x,          (numeric) exponential decay (per block) for historical moving average of confirmation data\n"
            "  \"scale\" : x,            (numeric) The resolution of confirmation targets at this time horizon\n"
            "  \"pass\" : {              (json object) information about the lowest range of feerates to succeed in meeting the threshold\n"
            "      \"startrange\" : x.x,     (numeric) start of feerate range\n"
            "      \"endrange\" : x.x,       (numeric) end of feerate range\n"
            "      \"withintarget\" : x.x,   (numeric) number of txs over history horizon in the feerate range that were confirmed within target\n"
            "      \"totalconfirmed\" : x.x, (numeric) number of txs over history horizon in the feerate range that were confirmed at any point\n"
            "      \"inmempool\" : x.x,      (numeric) current number of txs in mempool in the feerate range unconfirmed for at least target blocks\n"
            "      \"leftmempool\" : x.x,    (numeric) number of txs over history horizon in the feerate range that left mempool unconfirmed after target\n"
            "  }\n"
            "  \"fail\" : { ... }        (json object) information about the highest range of feerates to fail to meet the threshold\n"
            "}\n"
            "\n"
            "A negative feerate is returned if no answer can be given.\n"
            "\nExample:\n"
            + HelpExampleCli("estimaterawfee", "6 0.9 1")
            );

    RPCTypeCheck(request.params, {UniValue::VNUM, UniValue::VNUM, UniValue::VNUM}, true);
    RPCTypeCheckArgument(request.params[0], UniValue::VNUM);
    int nBlocks = request.params[0].get_int();
    double threshold = 0.95;
    if (!request.params[1].isNull())
        threshold = request.params[1].get_real();
    FeeEstimateHorizon horizon = FeeEstimateHorizon::MED_HALFLIFE;
    if (!request.params[2].isNull()) {
        int horizonInt = request.params[2].get_int();
        if (horizonInt < 0 || horizonInt > 2) {
            throw JSONRPCError(RPC_TYPE_ERROR, "Invalid horizon for fee estimates");
        } else {
            horizon = (FeeEstimateHorizon)horizonInt;
        }
    }
    UniValue result(UniValue::VOBJ);
    CFeeRate feeRate;
    EstimationResult buckets;
    feeRate = ::feeEstimator.estimateRawFee(nBlocks, threshold, horizon, &buckets);

    result.push_back(Pair("feerate", feeRate == CFeeRate(0) ? -1.0 : ValueFromAmount(feeRate.GetFeePerK())));
    result.push_back(Pair("decay", buckets.decay));
    result.push_back(Pair("scale", (int)buckets.scale));
    UniValue passbucket(UniValue::VOBJ);
    passbucket.push_back(Pair("startrange", round(buckets.pass.start)));
    passbucket.push_back(Pair("endrange", round(buckets.pass.end)));
    passbucket.push_back(Pair("withintarget", round(buckets.pass.withinTarget * 100.0) / 100.0));
    passbucket.push_back(Pair("totalconfirmed", round(buckets.pass.totalConfirmed * 100.0) / 100.0));
    passbucket.push_back(Pair("inmempool", round(buckets.pass.inMempool * 100.0) / 100.0));
    passbucket.push_back(Pair("leftmempool", round(buckets.pass.leftMempool * 100.0) / 100.0));
    result.push_back(Pair("pass", passbucket));
    UniValue failbucket(UniValue::VOBJ);
    failbucket.push_back(Pair("startrange", round(buckets.fail.start)));
    failbucket.push_back(Pair("endrange", round(buckets.fail.end)));
    failbucket.push_back(Pair("withintarget", round(buckets.fail.withinTarget * 100.0) / 100.0));
    failbucket.push_back(Pair("totalconfirmed", round(buckets.fail.totalConfirmed * 100.0) / 100.0));
    failbucket.push_back(Pair("inmempool", round(buckets.fail.inMempool * 100.0) / 100.0));
    failbucket.push_back(Pair("leftmempool", round(buckets.fail.leftMempool * 100.0) / 100.0));
    result.push_back(Pair("fail", failbucket));
    return result;
}

static const CRPCCommand commands[] =
{ //  category              name                      actor (function)         okSafeMode
  //  --------------------- ------------------------  -----------------------  ----------
    { "block_generation",   "getnetworkhashps",       &getnetworkhashps,       true,  {"num_blocks","height"} },
    { "block_generation",   "getmininginfo",          &getmininginfo,          true,  {} },
    { "block_generation",   "prioritisetransaction",  &prioritisetransaction,  true,  {"txid","dummy_value","fee_delta"} },
    { "block_generation",   "getblocktemplate",       &getblocktemplate,       true,  {"template_request"} },
    { "block_generation",   "submitblock",            &submitblock,            true,  {"hexdata","parameters"} },

    { "generating",         "generate",               &generate,               true,  {"num_blocks","max_tries"} },
    { "generating",         "generatetoaddress",      &generatetoaddress,      true,  {"num_blocks","address","max_tries"} },
    { "generating",         "getgenerate",            &getgenerate,            true,  {}  },
    { "generating",         "setgenerate",            &setgenerate,            true,  {"generate", "gen_proc_limit"}  },

    { "util",               "estimatefee",            &estimatefee,            true,  {"num_blocks"} },
    { "util",               "estimatesmartfee",       &estimatesmartfee,       true,  {"num_blocks", "conservative"} },

    { "hidden",             "estimaterawfee",         &estimaterawfee,         true,  {"num_blocks", "threshold", "horizon"} },
};

void RegisterMiningRPCCommands(CRPCTable &t)
{
    for (unsigned int vcidx = 0; vcidx < ARRAYLEN(commands); vcidx++)
        t.appendCommand(commands[vcidx].name, &commands[vcidx]);
}
