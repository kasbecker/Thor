// Copyright (c) 2014-2016 The Dash developers
// Copyright (c) 2016-2017 The PIVX Developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "spork.h"
#include "base58.h"
#include "key.h"
#include "main.h"
#include "masternode-budget.h"
#include "net.h"
#include "protocol.h"
#include "sync.h"
#include "sporkdb.h"
#include "util.h"

#include <numeric>
#include <boost/algorithm/string/join.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/algorithm/string/classification.hpp>
#include <boost/lexical_cast.hpp>

CSporkManager sporkManager;

std::map<uint256, CSporkMessage> mapSporks;
static std::map<int, std::string> mapSporkDefaults = {
    {SPORK_2_SWIFTTX_ENABLED,                "0"},             // ON
    {SPORK_3_SWIFTTX_BLOCK_FILTERING,        "0"},             // ON
    {SPORK_5_INSTANTSEND_MAX_VALUE,          "1000"},          // 1000 PIVX
    {SPORK_8_MASTERNODE_PAYMENT_ENFORCEMENT, "4070908800"},    // OFF
    {SPORK_9_SUPERBLOCKS_ENABLED,            "4070908800"},    // OFF
    {SPORK_10_MASTERNODE_PAY_UPDATED_NODES,  "4070908800"},    // OFF
    {SPORK_12_RECONSIDER_BLOCKS,             "0"},             // 0 BLOCKS
};

static bool IsMultiValueSpork(int nSporkID)
{
    if(SPORK_13_BLOCK_PAYMENTS == nSporkID ||
            SPORK_14_TX_FEE == nSporkID) {
        return true;
    }

    return false;
}

void CSporkManager::AddActiveSpork(const CSporkMessage &spork)
{
    auto &sporks = mapSporksActive[spork.nSporkID];
    if(IsMultiValueSpork(spork.nSporkID)) {
        sporks.push_back(spork);
        std::sort(std::begin(sporks), std::end(sporks), [](const CSporkMessage &lhs, const CSporkMessage &rhs) {
            return lhs.nTimeSigned < rhs.nTimeSigned;
        });
    }
    else {
        sporks = { spork };
    }
}

bool CSporkManager::IsNewerSpork(const CSporkMessage &spork) const
{

    if(mapSporksActive.count(spork.nSporkID)) {
        const auto &sporks = mapSporksActive.at(spork.nSporkID);
        // at this place items have to be sorted
        if (sporks.back().nTimeSigned < spork.nTimeSigned)
            return true;
    }

    return false;
}

// DIVI: on startup load spork values from previous session if they exist in the sporkDB
void CSporkManager::LoadSporksFromDB()
{
    for (int i = SPORK_START; i <= SPORK_END; ++i) {
        // Since not all spork IDs are in use, we have to exclude undefined IDs
        std::string strSpork = sporkManager.GetSporkNameByID(i);
        if (strSpork == "Unknown") continue;

        // attempt to read spork from sporkDB
        CSporkMessage spork;
        if (!pSporkDB->ReadSpork(i, spork)) {
            LogPrintf("%s : no previous value for %s found in database\n", __func__, strSpork);
            continue;
        }

        // add spork to memory
        mapSporks[spork.GetHash()] = spork;
        //        mapSporksActive[spork.nSporkID] = spork;
        //        std::time_t result = spork.strValue;
        // If SPORK Value is greater than 1,000,000 assume it's actually a Date and then convert to a more readable format
        //        if (spork.strValue > 1000000) {
        //            LogPrintf("%s : loaded spork %s with value %d : %s", __func__,
        //                      sporkManager.GetSporkNameByID(spork.nSporkID), spork.strValue,
        //                      std::ctime(&result));
        //        } else {
        LogPrintf("%s : loaded spork %s with value %d\n", __func__,
                  sporkManager.GetSporkNameByID(spork.nSporkID), spork.strValue);
        //        }
    }
}

void CSporkManager::ProcessSpork(CNode* pfrom, const std::string& strCommand, CDataStream& vRecv)
{
    if(fLiteMode) return; // disable all Dash specific functionality

    if (strCommand == "spork") {

        CSporkMessage spork;
        vRecv >> spork;

        uint256 hash = spork.GetHash();

        std::string strLogMsg;
        {
            LOCK(cs_main);
            if(!chainActive.Tip()) return;
            strLogMsg = strprintf("SPORK -- hash: %s id: %d value: %10d bestHeight: %d peer=%d", hash.ToString(), spork.nSporkID, spork.strValue, chainActive.Height(), pfrom->id);
        }

        if(IsNewerSpork(spork)) {
            LogPrintf("%s new\n", strLogMsg);
        } else if(!IsMultiValueSpork(spork.nSporkID)) {
            LogPrint("spork", "%s seen\n", strLogMsg);
            pfrom->nSporksSynced++;
            return;
        }

        /*
        if(!spork.CheckSignature(sporkPubKey)) {
            LOCK(cs_main);
            LogPrintf("CSporkManager::ProcessSpork -- ERROR: invalid signature\n");
            Misbehaving(pfrom->GetId(), 100);
            return;
        }
        */

        pfrom->nSporksSynced++;

        mapSporks[hash] = spork;
        AddActiveSpork(spork);
        spork.Relay();

        //does a task if needed
        ExecuteSpork(spork.nSporkID, spork.strValue);

    } else if (strCommand == "getsporks") {

        for(auto &&entry : mapSporksActive) {
            for(auto &&spork : entry.second) {
                pfrom->PushMessage("spork", spork);
            }
        }

    } else if(strCommand == "sporkcount") {
        int nSporkCount = 0;
        vRecv >> nSporkCount;
        pfrom->SetSporkCount(nSporkCount);
        pfrom->PushMessage("getsporks");
    }

}

void ReprocessBlocks(int nBlocks)
{
    std::map<uint256, int64_t>::iterator it = mapRejectedBlocks.begin();
    while (it != mapRejectedBlocks.end()) {
        //use a window twice as large as is usual for the nBlocks we want to reset
        if ((*it).second > GetTime() - (nBlocks * 60 * 5)) {
            BlockMap::iterator mi = mapBlockIndex.find((*it).first);
            if (mi != mapBlockIndex.end() && (*mi).second) {
                LOCK(cs_main);

                CBlockIndex* pindex = (*mi).second;
                LogPrintf("ReprocessBlocks - %s\n", (*it).first.ToString());

                CValidationState state;
                ReconsiderBlock(state, pindex);
            }
        }
        ++it;
    }

    CValidationState state;
    {
        LOCK(cs_main);
        DisconnectBlocksAndReprocess(nBlocks);
    }

    if (state.IsValid()) {
        ActivateBestChain(state);
    }
}

void CSporkManager::ExecuteSpork(int nSporkID, string strValue)
{
    //correct fork via spork technology
    if(nSporkID == SPORK_12_RECONSIDER_BLOCKS) {

        int64_t nValue = 0;
        try
        {
            nValue = boost::lexical_cast<int64_t>(strValue);
        }
        catch(boost::bad_lexical_cast &)
        {
        }

        if(nValue <= 0) {
            return;
        }

        // allow to reprocess 24h of blocks max, which should be enough to resolve any issues
        int64_t nMaxBlocks = 576;
        // this potentially can be a heavy operation, so only allow this to be executed once per 10 minutes
        int64_t nTimeout = 10 * 60;

        static int64_t nTimeExecuted = 0; // i.e. it was never executed before

        if(GetTime() - nTimeExecuted < nTimeout) {
            LogPrint("spork", "CSporkManager::ExecuteSpork -- ERROR: Trying to reconsider blocks, too soon - %d/%d\n", GetTime() - nTimeExecuted, nTimeout);
            return;
        }

        if(nValue > nMaxBlocks) {
            LogPrintf("CSporkManager::ExecuteSpork -- ERROR: Trying to reconsider too many blocks %d/%d\n", strValue, nMaxBlocks);
            return;
        }


        LogPrintf("CSporkManager::ExecuteSpork -- Reconsider Last %d Blocks\n", nValue);

        ReprocessBlocks(nValue);
        nTimeExecuted = GetTime();
    }
}

bool CSporkManager::UpdateSpork(int nSporkID, string strValue)
{

    CSporkMessage spork = CSporkMessage(nSporkID, strValue, GetAdjustedTime());

    if(!IsNewerSpork(spork))
        return false;

    if(spork.Sign(sporkPrivKey, sporkPubKey)) {
        spork.Relay();
        mapSporks[spork.GetHash()] = spork;
        AddActiveSpork(spork);
        return true;
    }

    return false;
}

int CSporkManager::GetActiveSporkCount() const
{
    return std::accumulate(std::begin(mapSporksActive), std::end(mapSporksActive),  0, [](int accum, const std::pair<int, std::vector<CSporkMessage>> &item) {
        return accum + item.second.size();
    });
}

// grab the spork, otherwise say it's off
bool CSporkManager::IsSporkActive(int nSporkID)
{
    // Multi value sporks cannot be active, but they can store value
    bool fMultiValue = IsMultiValueSpork(nSporkID);

    if(fMultiValue) {
        return mapSporksActive.count(nSporkID);
    }

    std::string r;

    if(mapSporksActive.count(nSporkID)){
        r = mapSporksActive[nSporkID].front().strValue; // always one value
    } else if (mapSporkDefaults.count(nSporkID)) {
        r = mapSporkDefaults[nSporkID];
    } else {
        LogPrint("spork", "CSporkManager::IsSporkActive -- Unknown Spork ID %d\n", nSporkID);
        r = "4070908800"; // 2099-1-1 i.e. off by default
    }

    return boost::lexical_cast<int64_t>(r) < GetAdjustedTime();
}

std::vector<string> CSporkManager::GetMultiValueSporkValues(int nSporkID) const
{
    std::vector<std::string> result;
    if (IsMultiValueSpork(nSporkID) && mapSporksActive.count(nSporkID)) {

        for(auto &&spork : mapSporksActive.at(nSporkID)) {
            result.push_back(spork.strValue);
        }

        return result;
    }

    return result;
}

// grab the value of the spork on the network, or the default
string CSporkManager::GetSporkValue(int nSporkID) const
{
    if(IsMultiValueSpork(nSporkID)) {
        return std::string();
    }

    if (mapSporksActive.count(nSporkID)) {
        return mapSporksActive.at(nSporkID).front().strValue;
    }

    if (mapSporkDefaults.count(nSporkID)) {
        return mapSporkDefaults[nSporkID];
    }

    LogPrint("spork", "CSporkManager::GetSporkValue -- Unknown Spork ID %d\n", nSporkID);
    return std::string();
}

int CSporkManager::GetSporkIDByName(const std::string& strName)
{
    if (strName == "SPORK_2_SWIFTTX_ENABLED")               return SPORK_2_SWIFTTX_ENABLED;
    if (strName == "SPORK_3_SWIFTTX_BLOCK_FILTERING")       return SPORK_3_SWIFTTX_BLOCK_FILTERING;
    if (strName == "SPORK_5_INSTANTSEND_MAX_VALUE")             return SPORK_5_INSTANTSEND_MAX_VALUE;
    if (strName == "SPORK_8_MASTERNODE_PAYMENT_ENFORCEMENT")    return SPORK_8_MASTERNODE_PAYMENT_ENFORCEMENT;
    if (strName == "SPORK_9_SUPERBLOCKS_ENABLED")               return SPORK_9_SUPERBLOCKS_ENABLED;
    if (strName == "SPORK_10_MASTERNODE_PAY_UPDATED_NODES")     return SPORK_10_MASTERNODE_PAY_UPDATED_NODES;
    if (strName == "SPORK_12_RECONSIDER_BLOCKS")                return SPORK_12_RECONSIDER_BLOCKS;

    LogPrint("spork", "CSporkManager::GetSporkIDByName -- Unknown Spork name '%s'\n", strName);
    return -1;
}

std::string CSporkManager::GetSporkNameByID(int nSporkID)
{
    switch (nSporkID) {
    case SPORK_2_SWIFTTX_ENABLED:               return "SPORK_2_SWIFTTX_ENABLED";
    case SPORK_3_SWIFTTX_BLOCK_FILTERING:       return "SPORK_3_SWIFTTX_BLOCK_FILTERING";
    case SPORK_5_INSTANTSEND_MAX_VALUE:             return "SPORK_5_INSTANTSEND_MAX_VALUE";
    case SPORK_8_MASTERNODE_PAYMENT_ENFORCEMENT:    return "SPORK_8_MASTERNODE_PAYMENT_ENFORCEMENT";
    case SPORK_9_SUPERBLOCKS_ENABLED:               return "SPORK_9_SUPERBLOCKS_ENABLED";
    case SPORK_10_MASTERNODE_PAY_UPDATED_NODES:     return "SPORK_10_MASTERNODE_PAY_UPDATED_NODES";
    case SPORK_12_RECONSIDER_BLOCKS:                return "SPORK_12_RECONSIDER_BLOCKS";
    default:
        LogPrint("spork", "CSporkManager::GetSporkNameByID -- Unknown Spork ID %d\n", nSporkID);
        return "Unknown";
    }
}

bool CSporkManager::SetSporkAddress(const std::string& strPubKey) {
    CPubKey pubkeynew(ParseHex(strPubKey));
    if (!pubkeynew.IsValid()) {
        LogPrintf("CSporkManager::SetSporkAddress -- Failed to parse spork address\n");
        return false;
    }

    sporkPubKey = pubkeynew;

    return true;
}

bool CSporkManager::SetPrivKey(const std::string& strPrivKey)
{
    CKey key;
    CPubKey pubKey;
    if(!CObfuScationSigner::GetKeysFromSecret(strPrivKey, key, pubKey)) {
        LogPrintf("CSporkManager::SetPrivKey -- Failed to parse private key\n");
        return false;
    }

    if (pubKey.GetID() != sporkPubKey.GetID()) {
        LogPrintf("CSporkManager::SetPrivKey -- New private key does not belong to spork address\n");
        return false;
    }

    CSporkMessage spork;
    if (spork.Sign(key, sporkPubKey)) {
        // Test signing successful, proceed
        LogPrintf("CSporkManager::SetPrivKey -- Successfully initialized as spork signer\n");

        sporkPrivKey = key;
        return true;
    } else {
        LogPrintf("CSporkManager::SetPrivKey -- Test signing failed\n");
        return false;
    }
}

uint256 CSporkMessage::GetHash() const
{
    return SerializeHash(*this);
}

uint256 CSporkMessage::GetSignatureHash() const
{
    return GetHash();
}

bool CSporkMessage::Sign(const CKey& key, const CPubKey &sporkPubKey)
{
    if (!key.IsValid()) {
        LogPrintf("CSporkMessage::Sign -- signing key is not valid\n");
        return false;
    }

    std::string strError = "";

    std::string strMessage = boost::lexical_cast<std::string>(nSporkID) + strValue + boost::lexical_cast<std::string>(nTimeSigned);

    if(!CObfuScationSigner::SignMessage(strMessage, strError, vchSig, key)) {
        LogPrintf("CSporkMessage::Sign -- SignMessage() failed\n");
        return false;
    }

    if(!CObfuScationSigner::VerifyMessage(sporkPubKey, vchSig, strMessage, strError)) {
        LogPrintf("CSporkMessage::Sign -- VerifyMessage() failed, error: %s\n", strError);
        return false;
    }

    return true;
}

bool CSporkMessage::CheckSignature(const CPubKey& pubKey) const
{
    std::string strError = "";


    std::string strMessage = boost::lexical_cast<std::string>(nSporkID) + boost::lexical_cast<std::string>(strValue) + boost::lexical_cast<std::string>(nTimeSigned);

    if (!CObfuScationSigner::VerifyMessage(pubKey, vchSig, strMessage, strError)){
        LogPrintf("CSporkMessage::CheckSignature -- VerifyHash() failed, error: %s\n", strError);
        return false;
    }

    return true;
}

void CSporkMessage::Relay()
{
    CInv inv(MSG_SPORK, GetHash());
    RelayInv(inv);
}

BlockPayment::BlockPayment() :
    nStakeReward(0),
    nMasternodeReward(0),
    nTreasuryReward(0),
    nProposalsReward(0),
    nCharityReward(0),
    nActivationBlockHeight(0)
{
}

BlockPayment::BlockPayment(int nStakeRewardIn, int nMasternodeRewardIn, int nTreasuryRewardIn,
                           int nProposalsRewardIn, int nCharityRewardIn, int nActivationBlockHeightIn) :
    nStakeReward(nStakeRewardIn),
    nMasternodeReward(nMasternodeRewardIn),
    nTreasuryReward(nTreasuryRewardIn),
    nProposalsReward(nProposalsRewardIn),
    nCharityReward(nCharityRewardIn),
    nActivationBlockHeight(nActivationBlockHeightIn)
{

}

BlockPayment BlockPayment::FromString(string strData)
{
    std::vector<std::string> vecTokens;
    boost::algorithm::split(vecTokens, strData, boost::is_any_of(";"));
    std::vector<int> vecParsedValues;

    try
    {
        for(auto &&token : vecTokens) {
            vecParsedValues.emplace_back(boost::lexical_cast<int>(token));
        }
    }
    catch(boost::bad_lexical_cast &)
    {

    }

    if(vecParsedValues.size() != 6) {
        return BlockPayment();
    }

    return BlockPayment(vecParsedValues.at(0), vecParsedValues.at(1), vecParsedValues.at(2),
                        vecParsedValues.at(3), vecParsedValues.at(4), vecParsedValues.at(5));
}

bool BlockPayment::IsValid() const
{
    auto values = {
        nStakeReward,
        nMasternodeReward,
        nTreasuryReward,
        nProposalsReward,
        nCharityReward
    };

    return nActivationBlockHeight > 0 &&
            std::accumulate(std::begin(values), std::end(values), 0) == 100;
}

string BlockPayment::ToString() const
{
    auto values = {
        nStakeReward,
        nMasternodeReward,
        nTreasuryReward,
        nProposalsReward,
        nCharityReward,
        nActivationBlockHeight
    };

    std::vector<std::string> strValues;

    std::transform(std::begin(values), std::end(values), std::back_inserter(strValues), [](int value) {
        return boost::lexical_cast<std::string>(value);
    });

    return boost::algorithm::join(strValues, ";");
}
