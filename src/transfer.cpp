/**
 * @file transfer.cpp
 * @brief Pending/active up/download ordered by file fingerprint
 *
 * (c) 2013-2014 by Mega Limited, Auckland, New Zealand
 *
 * This file is part of the MEGA SDK - Client Access Engine.
 *
 * Applications using the MEGA API must present a valid application key
 * and comply with the the rules set forth in the Terms of Service.
 *
 * The MEGA SDK is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * @copyright Simplified (2-clause) BSD License.
 *
 * You should have received a copy of the license along with this
 * program.
 */

#include "mega/transfer.h"

#include "mega/base64.h"
#include "mega/logging.h"
#include "mega/mediafileattribute.h"
#include "mega/megaapp.h"
#include "mega/megaclient.h"
#include "mega/sync.h"
#include "mega/testhooks.h"
#include "mega/transferslot.h"
#include "mega/types.h"
#include "mega/utils.h"
#include "megawaiter.h"

namespace mega {

TransferCategory::TransferCategory(direction_t d, filesizetype_t s)
    : direction(d)
    , sizetype(s)
{
}

TransferCategory::TransferCategory(Transfer* t)
    : direction(t->type)
    , sizetype(t->size > 131072 ? LARGEFILE : SMALLFILE)  // Conservative starting point: 131072 is the smallest chunk, we will certainly only use one socket to upload/download
{
}

unsigned TransferCategory::index()
{
    assert(direction == GET || direction == PUT);
    assert(sizetype == LARGEFILE || sizetype == SMALLFILE);
    return 2 + direction * 2 + sizetype;
}

unsigned TransferCategory::directionIndex()
{
    assert(direction == GET || direction == PUT);
    return direction;
}

Transfer::Transfer(MegaClient* cclient, direction_t ctype)
    : bt(cclient->rng, cclient->transferRetryBackoffs[ctype])
{
    type = ctype;
    client = cclient;
    size = 0;
    failcount = 0;
    pos = 0;
    ctriv = 0;
    metamac = 0;
    tag = 0;
    slot = NULL;
    asyncopencontext = NULL;
    progresscompleted = 0;
    finished = false;
    lastaccesstime = 0;
    ultoken = NULL;

    priority = 0;
    state = TRANSFERSTATE_NONE;

    skipserialization = false;

    transfers_it = client->multi_transfers[type].end();
}

// delete transfer with underlying slot, notify files
Transfer::~Transfer()
{
    auto keepDownloadTarget = false;

    TransferDbCommitter* committer = nullptr;
    if (client->tctable && client->tctable->getTransactionCommitter())
    {
        committer = dynamic_cast<TransferDbCommitter*>(client->tctable->getTransactionCommitter());
        assert(committer);
    }

    if (!uploadhandle.isUndef() && !mIsSyncUpload) // For sync uploads, we will delete the attributes upon SyncUpload_inClient destruction
    {
        client->fileAttributesUploading.erase(uploadhandle);
    }

    for (file_list::iterator it = files.begin(); it != files.end(); it++)
    {
        if (finished)
        {
            client->filecachedel(*it, nullptr);
        }

        (*it)->transfer = NULL;

        auto terminatedErrCode = API_OK;
        if (type == GET)
        {
#ifdef ENABLE_SYNC
            if (auto dl = dynamic_cast<SyncDownload_inClient*>(*it))
            {
                assert((*it)->syncxfer);
                // Let's forward the error from previous possible failed() calls to the terminated
                // command instad of terminating with API_OK
                terminatedErrCode = dl->mError;

                // Keep sync downloads whose Mac failed, so the user can decide to keep them or not
                if (dl->mError == API_EKEY)
                {
                    keepDownloadTarget = true;
                    dl->setLocalname(localfilename);
                }
            }
            else
#endif
            {
                assert(!(*it)->syncxfer);
                if (downloadDistributor)
                    downloadDistributor->removeTarget();
            }
        }

        // this File may be deleted by this call.  So call after the tests above
        (*it)->terminated(terminatedErrCode);
    }

    if (!mOptimizedDelete)
    {
        if (transfers_it != client->multi_transfers[type].end())
        {
            client->multi_transfers[type].erase(transfers_it);
        }

        client->transferlist.removetransfer(this);
    }

    if (slot)
    {
        delete slot;
    }

    if (asyncopencontext)
    {
        asyncopencontext.reset();
        client->asyncfopens--;
    }

    if (finished)
    {
        if (type == GET && !localfilename.empty())
        {
            if (!keepDownloadTarget)
                client->fsaccess->unlinklocal(localfilename);
        }
        client->transfercachedel(this, committer);
    }
}

bool Transfer::serialize(string *d) const
{
    assert(localfilename.empty() || localfilename.isAbsolute());

    unsigned short ll;

    d->append((const char*)&type, sizeof(type));

    const auto& tmpstr = localfilename.serialize();
    ll = (unsigned short)tmpstr.size();
    d->append((char*)&ll, sizeof(ll));
    d->append(tmpstr.data(), ll);

    d->append((const char*)&filekey.bytes, sizeof(filekey.bytes));
    d->append((const char*)&ctriv, sizeof(ctriv));
    d->append((const char*)&metamac, sizeof(metamac));
    d->append((const char*)transferkey.data(), sizeof (transferkey));

    chunkmacs.serialize(*d);

    if (!FileFingerprint::serialize(d))
    {
        LOG_err << "Error serializing Transfer: Unable to serialize FileFingerprint";
        return false;
    }

    if (!badfp.serialize(d))
    {
        LOG_err << "Error serializing Transfer: Unable to serialize badfp";
        return false;
    }

    d->append((const char*)&lastaccesstime, sizeof(lastaccesstime));

    char hasUltoken;
    if (ultoken)
    {
        hasUltoken = 2;
        d->append((const char*)&hasUltoken, sizeof(char));
        d->append((const char*)ultoken.get(), UPLOADTOKENLEN);
    }
    else
    {
        hasUltoken = 0;
        d->append((const char*)&hasUltoken, sizeof(char));
    }

    // store raid URL string(s) in the same record as non-raid, 0-delimited in the case of raid
    std::string combinedUrls;
    for (std::vector<std::string>::const_iterator i = tempurls.begin(); i != tempurls.end(); ++i)
    {
        combinedUrls.append("", i == tempurls.begin() ? 0 : 1); // '\0' separator
        combinedUrls.append(*i);
    }
    ll = (unsigned short)combinedUrls.size();
    d->append((char*)&ll, sizeof(ll));
    d->append(combinedUrls.data(), ll);

    char s = static_cast<char>(state);
    d->append((const char*)&s, sizeof(s));
    d->append((const char*)&priority, sizeof(priority));

    CacheableWriter cw(*d);
    // version. Originally, 0.  Version 1 adds expansion flags, which then work in the usual way
    cw.serializeu8(1);

    // 8 expansion flags, in the normal manner. First flag is for whether downloadFileHandle is
    // present. Second flag is for amount of discarded temp URLs. Third Flag is for marking if
    // localfilename is serialized as LocalPath
    cw.serializeexpansionflags(downloadFileHandle.isUndef() ? 0 : 1, 1, 1);

    if (!downloadFileHandle.isUndef())
    {
        cw.serializeNodeHandle(downloadFileHandle);
    }

    cw.serializeu8(discardedTempUrlsSize);

#ifdef DEBUG
    // very quick debug only double check
    string tempstr = *d;
    transfer_multimap tempmap[2];
    unique_ptr<Transfer> t(unserialize(client, &tempstr, tempmap));
    assert(t);
    assert(t->localfilename == localfilename);
    assert(t->tempurls == tempurls);
    assert(t->state == (state == TRANSFERSTATE_PAUSED ? TRANSFERSTATE_PAUSED : TRANSFERSTATE_NONE));
    assert(t->priority == priority);
    assert(t->fingerprint() == fingerprint() || (!t->fingerprint().isvalid && !fingerprint().isvalid));
    assert(t->badfp == badfp || (!t->badfp.isvalid && !badfp.isvalid));
    assert(t->downloadFileHandle == downloadFileHandle);
    assert(t->discardedTempUrlsSize == discardedTempUrlsSize);
#endif


    return true;
}

Transfer *Transfer::unserialize(MegaClient *client, string *d, transfer_multimap* multi_transfers)
{
    CacheableReader r(*d);

    direction_t type;
    string filepath;
    if (!r.unserializedirection(type) ||
        (type != GET && type != PUT) ||
        !r.unserializestring(filepath))
    {
        assert(false);
        LOG_err << "Transfer unserialization failed at field " << r.fieldnum;
        return nullptr;
    }

    unique_ptr<Transfer> t(new Transfer(client, type));

    int8_t hasUltoken;  // value 1 was for OLDUPLOADTOKENLEN, but that was from 2016

    if (!r.unserializebinary(t->filekey.bytes.data(), sizeof(t->filekey)) ||
        !r.unserializei64(t->ctriv) ||
        !r.unserializei64(t->metamac) ||
        !r.unserializebinary(t->transferkey.data(), SymmCipher::KEYLENGTH) ||
        !r.unserializechunkmacs(t->chunkmacs) ||
        !r.unserializefingerprint(*t) ||
        !r.unserializefingerprint(t->badfp) ||
        !r.unserializei64(t->lastaccesstime) ||
        !r.unserializei8(hasUltoken) ||
        (hasUltoken && hasUltoken != 2))
    {
        LOG_err << "Transfer unserialization failed at field " << r.fieldnum;
        return nullptr;
    }

    if (hasUltoken)
    {
        t->ultoken.reset(new UploadToken);
    }

    unsigned char expansionflags[8] = { 0 };
    std::string combinedUrls;
    int8_t state;
    int8_t version;
    if ((hasUltoken && !r.unserializebinary(t->ultoken->data(), UPLOADTOKENLEN)) ||
        !r.unserializestring(combinedUrls) || !r.unserializei8(state) ||
        !r.unserializeu64(t->priority) || !r.unserializei8(version) ||
        (version > 0 && !r.unserializeexpansionflags(expansionflags, 3)) ||
        (expansionflags[0] && !r.unserializeNodeHandle(t->downloadFileHandle)) ||
        (expansionflags[1] && !r.unserializeu8(t->discardedTempUrlsSize)))
    {
        LOG_err << "Transfer unserialization failed at field " << r.fieldnum;
        return nullptr;
    }
    assert(!r.hasdataleft());

    if (!filepath.empty())
    {
        if (const bool isLocalPath = expansionflags[2] == 1; isLocalPath)
        {
            auto localPath = LocalPath::unserialize(filepath);
            if (localPath.has_value())
            {
                t->localfilename = localPath.value();
            }
        }
        else
        {
            t->localfilename = LocalPath::fromPlatformEncodedAbsolute(filepath);
        }
    }

    size_t ll = combinedUrls.size();
    for (size_t p = 0; p < ll; )
    {
        size_t n = combinedUrls.find('\0');
        t->tempurls.push_back(combinedUrls.substr(p, n));
        assert(!t->tempurls.back().empty());
        p += (n == std::string::npos) ? ll : (n + 1);
    }

    assert(!t->discardedTempUrlsSize || !t->tempurls.empty());
    if (!t->tempurls.empty() && t->tempurls.size() != 1 && t->tempurls.size() != RAIDPARTS)
    {
        LOG_err << "Transfer unserialization failed - temp URL incorrect components";
        return nullptr;
    }

    if (state == TRANSFERSTATE_PAUSED)
    {
        LOG_debug << "Unserializing paused transfer";
        t->state = TRANSFERSTATE_PAUSED;
    }

    t->chunkmacs.calcprogress(t->size, t->pos, t->progresscompleted);

    multi_transfers[type].insert(pair<FileFingerprint*, Transfer*>(t.get(), t.get()));
    return t.release();
}

SymmCipher *Transfer::transfercipher()
{
    return client->getRecycledTemporaryTransferCipher(transferkey.data());
}

void Transfer::removeCancelledTransferFiles(TransferDbCommitter* committer)
{
    // remove transfer files whose MegaTransfer associated has been cancelled (via cancel token)
    for (file_list::iterator it = files.begin(); it != files.end();)
    {
        file_list::iterator auxit = it++;
        if ((*auxit)->cancelToken.isCancelled())
        {
            removeTransferFile(API_EINCOMPLETE, *auxit, committer);
        }
    }
}

void Transfer::removeTransferFile(error e, File* f, TransferDbCommitter* committer)
{
    Transfer *transfer = f->transfer;
    client->filecachedel(f, committer);
    assert(*f->file_it == f);
    transfer->files.erase(f->file_it);
    client->app->file_removed(f, e);
    f->transfer = NULL;
    f->terminated(e);
}

void Transfer::removeAndDeleteSelf(transferstate_t finalState)
{
    finished = true;
    state = finalState;
    client->app->transfer_removed(this);

    // this will also remove the transfer from internal lists etc.
    // those use a lazy delete (ie mark for later deletion) so that we don't invalidate iterators.
    delete this;
}

// transfer attempt failed, notify all related files, collect request on
// whether to abort the transfer, kill transfer if unanimous
void Transfer::failed(const Error& e, TransferDbCommitter& committer, dstime timeleft)
{
    bool defer = false;

    LOG_debug << "Transfer failed with error " << e;

    DEBUG_TEST_HOOK_DOWNLOAD_FAILED(e);

    if (e == API_EOVERQUOTA || e == API_EPAYWALL)
    {
        assert((e == API_EPAYWALL && !timeleft) || (type == PUT && !timeleft) || (type == GET && timeleft)); // overstorage only possible for uploads, overbandwidth for downloads
        if (!slot)
        {
            bt.backoff(timeleft ? timeleft : NEVER);
            client->activateoverquota(timeleft, (e == API_EPAYWALL));
            client->app->transfer_failed(this, e, timeleft);
            ++client->performanceStats.transferTempErrors;
        }
        else
        {
            bool allForeignTargets = true;
            for (auto &file : files)
            {
                if (client->isPrivateNode(file->h))
                {
                    allForeignTargets = false;
                    break;
                }
            }

            /* If all targets are foreign and there's not a bandwidth overquota, transfer must fail.
             * Otherwise we need to activate overquota.
             */
            if (!timeleft && allForeignTargets)
            {
                client->app->transfer_failed(this, e);
            }
            else
            {
                bt.backoff(timeleft ? timeleft : NEVER);
                client->activateoverquota(timeleft, (e == API_EPAYWALL));
            }
        }
    }
    else if (e == API_EARGS || (e == API_EBLOCKED && type == GET) ||
             (e == API_ETOOMANY && type == GET && e.hasExtraInfo()) ||
             (e == API_ESUBUSERKEYMISSING))
    {
        assert(e != API_ESUBUSERKEYMISSING || type == PUT);
        client->app->transfer_failed(this, e);
    }
    else if (e != API_EBUSINESSPASTDUE)
    {
        bt.backoff();
        state = TRANSFERSTATE_RETRYING;
        client->app->transfer_failed(this, e, timeleft);
        ++client->performanceStats.transferTempErrors;
    }

    for (file_list::iterator it = files.begin(); it != files.end();)
    {
        // Remove files with foreign targets, if transfer failed with a (foreign) storage overquota
        if (e == API_EOVERQUOTA
            && ((*it)->isFuseTransfer()
                || (!timeleft && client->isForeignNode((*it)->h))))
        {
            removeTransferFile(e, *it++, &committer);
            continue;
        }

        /*
         * If the transfer failed with API_EARGS, the target handle is invalid. For a sync-transfer,
         * the actionpacket will eventually remove the target and the sync-engine will force to
         * disable the synchronization of the folder. For non-sync-transfers, remove the file directly.
         */
        if (e == API_EARGS || (e == API_EBLOCKED && type == GET) ||
            (e == API_ETOOMANY && type == GET && e.hasExtraInfo()) || (e == API_ESUBUSERKEYMISSING))
        {
             File *f = (*it++);
             if (f->syncxfer && e == API_EARGS)
             {
                defer = true;
             }
             else
             {
                removeTransferFile(e, f, &committer);
             }
             continue;
        }

        if (((*it)->failed(e, client) && (e != API_EBUSINESSPASTDUE))
                || (e == API_ENOENT // putnodes returned -9, file-storage server unavailable
                    && type == PUT
                    && tempurls.empty()
                    && failcount < 16) )
        {
            defer = true;
        }

        it++;
    }

    tempurls.clear();
    if (type == PUT)
    {
        chunkmacs.clear();
        progresscompleted = 0;
        ultoken.reset();
        pos = 0;

        if (slot && slot->fa)
        {
            if (!slot->fa->fopenSucceeded)
            {
                LOG_warn << "fopen failed for upload.";
                defer = false;
            }
            else if (slot->fa->mtime != mtime || slot->fa->size != size)
            {
                LOG_warn << "Modification detected during active upload. Size: " << size << "  Mtime: " << mtime
                         << "    FaSize: " << slot->fa->size << "  FaMtime: " << slot->fa->mtime;
                defer = false;
            }
        }
    }

    if (defer)
    {
        failcount++;
        delete slot;
        slot = NULL;
        client->transfercacheadd(this, &committer);

        LOG_debug << "Deferring transfer " << failcount << " during " << (bt.retryin() * 100) << " ms" << " [this = " << this << "]";
    }
    else
    {
        LOG_debug << "Removing transfer" << " [this = " << this << "]";
        state = TRANSFERSTATE_FAILED;
        finished = true;

#ifdef ENABLE_SYNC
        if (e == API_EBUSINESSPASTDUE)
        {
            LOG_debug << "Disabling syncs on account of API_EBUSINESSPASTDUE error on transfer";
            client->syncs.disableSyncs(ACCOUNT_EXPIRED, false, true);
        }
#endif

        for (file_list::iterator it = files.begin(); it != files.end(); it++)
        {
#ifdef ENABLE_SYNC
            if((*it)->syncxfer
                && e != API_EBUSINESSPASTDUE
                && e != API_EOVERQUOTA
                && e != API_EPAYWALL)
            {
                // Get the sync to check that folder again so it doesn't just recreate that same transfer
                LOG_debug << "Trigger sync parent path scan for failed transfer of " << (*it)->getLocalname();
                client->syncs.triggerSync((*it)->getLocalname().parentPath(), type == PUT);
            }
#endif


            client->app->file_removed(*it, e);
        }
        client->app->transfer_removed(this);
        ++client->performanceStats.transferFails;
        delete this;
    }
}

#ifdef USE_MEDIAINFO
static uint32_t* fileAttributeKeyPtr(byte filekey[FILENODEKEYLENGTH])
{
    // returns the last half, beyond the actual key, ie the nonce+crc
    return (uint32_t*)(filekey + FILENODEKEYLENGTH / 2);
}
#endif

void Transfer::addAnyMissingMediaFileAttributes(Node* node, /*const*/ LocalPath& localpath)
{
    assert(type == PUT || (node && node->type == FILENODE));

#ifdef USE_MEDIAINFO
    string ext;
    if (((type == PUT && size >= 16) || (node && node->nodekey().size() == FILENODEKEYLENGTH && node->size >= 16)) &&
        client->fsaccess->getextension(localpath, ext) &&
        MediaProperties::isMediaFilenameExt(ext) &&
        !client->mediaFileInfo.mediaCodecsFailed)
    {
        // for upload, the key is in the transfer.  for download, the key is in the node.
        uint32_t* attrKey = fileAttributeKeyPtr((type == PUT) ? filekey.bytes.data() : (byte*)node->nodekey().data());

        if (type == PUT || !node->hasfileattribute(fa_media) || client->mediaFileInfo.timeToRetryMediaPropertyExtraction(node->fileattrstring, attrKey))
        {
            // if we don't have the codec id mappings yet, send the request
            client->mediaFileInfo.requestCodecMappingsOneTime(client, LocalPath());

            // always get the attribute string; it may indicate this version of the mediaInfo library was unable to interpret the file
            MediaProperties vp;
            vp.extractMediaPropertyFileAttributes(localpath, client->fsaccess.get());

            if (type == PUT)
            {
                client->mediaFileInfo.queueMediaPropertiesFileAttributesForUpload(vp, attrKey, client, uploadhandle, this);
            }
            else
            {
                client->mediaFileInfo.sendOrQueueMediaPropertiesFileAttributesForExistingFile(vp, attrKey, client, node->nodeHandle());
            }
        }
    }
#endif
}

bool Transfer::isForSupport() const
{
    return type == PUT && !files.empty() && files.back()->targetuser == MegaClient::SUPPORT_USER_HANDLE;
}

bool Transfer::addTransferStats()
{
    if (!client)
    {
        LOG_err << "[Transfer::addTransferStats] called with a NULL MEGAclient";
        assert(false && "[Transfer::addTransferStats] called with a NULL MEGAclient");
        return false;
    }
    return client->mTransferStatsManager.addTransferStats(this);
}

void Transfer::collectAndPrintTransferStatsIfLimitReached()
{
    if (!client)
    {
        LOG_err << "[Transfer::collectAndPrintTransferStatsIfLimitReached] called with a NULL "
                   "MEGAclient";
        assert(
            false &&
            "[Transfer::collectAndPrintTransferStatsIfLimitReached] called with a NULL MEGAclient");
        return;
    }
    client->mTransferStatsManager.collectAndPrintTransferStatsIfLimitReached(type);
}

void Transfer::discardTempUrlsIfNoDataDownloadedOrTimeoutReached(
    const direction_t transferDirection,
    const m_time_t currentTime)
{
    DEBUG_TEST_HOOK_RESET_TRANSFER_LASTACCESSTIME(lastaccesstime)

    if (const auto discardTempURLs = (transferDirection == GET && !pos) ||
                                     ((currentTime - lastaccesstime) >= TEMPURL_TIMEOUT_TS);
        !discardTempURLs)
        return;

    LOG_warn << "[Transfer::discardTempUrlsIfNoTransferDataOrTimeoutReached] Discarding temporary "
                "URL (pos = "
             << pos << ", lastaccesstime = " << lastaccesstime << ", currentTime = " << currentTime
             << ", diff (" << (currentTime - lastaccesstime) << ") >= TEMPURL_TIMEOUT_TS ("
             << TEMPURL_TIMEOUT_TS << ")";

    switch (transferDirection)
    {
        case GET:
        {
            discardedTempUrlsSize = static_cast<uint8_t>(tempurls.size());
            break;
        }
        case PUT:
        {
            chunkmacs.clear();
            progresscompleted = 0;
            ultoken.reset();
            pos = 0;
            break;
        }
        default:
            break;
    }

    tempurls.clear();
}

void Transfer::adjustNonRaidedProgressIfNowIsRaided()
{
    static constexpr auto logPre = "[Transfer::adjustNonRaidedProgressIfNowIsRaided] ";

    if (const auto fromNonRaidToRaidResumption =
            (discardedTempUrlsSize == 1 && tempurls.size() == RAIDPARTS);
        !fromNonRaidToRaidResumption)
    {
        return;
    }

    if (!slot)
    {
        LOG_warn << logPre << "Call with no TransferSlot!";
        assert(false &&
               "Call to Transfer::adjustNonRaidedProgressIfNowIsRaided with invalid TransferSlot!");
        return;
    }

    LOG_debug << logPre
              << "Adjusting chunkmacs and transfer progress to discard non-contiguous data, as "
                 "well as the contiguous data reminder to RAIDLINE";
    chunkmac_map newChunkmacs;
    pos = chunkmacs.copyEntriesToUntilRaidlineBeforePos(pos, newChunkmacs);
    chunkmacs.swap(newChunkmacs);

    m_off_t sumOfPartialChunks{0};
    chunkmacs.calcprogress(size, pos, progresscompleted, &sumOfPartialChunks);

    if (progresscompleted > size)
    {
        LOG_err << logPre << "Invalid transfer progress!";
        pos = size;
        progresscompleted = size;
    }

    const auto progressContiguous = slot->updatecontiguousprogress();

    LOG_debug << logPre << "Adjusted resumed transfer at " << pos
              << " Completed: " << progresscompleted << " Contiguous: " << progressContiguous
              << " Partial: " << sumOfPartialChunks << " Size: " << size
              << " ultoken: " << (ultoken != NULL);

    discardedTempUrlsSize = 0;
}

FileDistributor::TargetNameExistsResolution Transfer::toTargetNameExistsResolution(CollisionResolution resolution)
{
    switch (resolution) {
        case CollisionResolution::Overwrite:
            return FileDistributor::TargetNameExistsResolution::OverwriteTarget;
        case CollisionResolution::RenameExistingToOldN:
            return FileDistributor::TargetNameExistsResolution::RenameExistingToOldN;
        case CollisionResolution::RenameNewWithN: // fall through
        default:
            return FileDistributor::TargetNameExistsResolution::RenameWithBracketedNumber;
    }
}

// transfer completion: copy received file locally, set timestamp(s), verify
// fingerprint, notify app, notify files
void Transfer::complete(TransferDbCommitter& committer)
{
    CodeCounter::ScopeTimer ccst(client->performanceStats.transferComplete);
    static constexpr std::string_view fingerprintIssue = "[Fingerprint Issue] ";

    state = TRANSFERSTATE_COMPLETING;
    client->app->transfer_update(this);

    if (type == GET)
    {

        LOG_debug << client->clientname << "Download complete: " << (files.size() ? LOG_NODEHANDLE(files.front()->h) : "NO_FILES") << " " << files.size() << (files.size() ? files.front()->name : "");

        bool transient_error = false;
        bool success;

        // disconnect temp file from slot...
        slot->fa.reset();

        // FIXME: multiple overwrite race conditions below (make copies
        // from open file instead of closing/reopening!)

        // set timestamp (subsequent moves & copies are assumed not to alter mtime)
        success = client->fsaccess->setmtimelocal(localfilename, mtime);

        if (!success)
        {
            transient_error = client->fsaccess->transient_error;
            LOG_debug << fingerprintIssue << "setmtimelocal failed " << transient_error;
        }

        // try to catch failing cases in the debugger (seen on synology SMB drive after the file was moved to final destination)
        assert(FSNode::debugConfirmOnDiskFingerprintOrLogWhy(*client->fsaccess, localfilename, *this));

        // verify integrity of file
        auto fa = client->fsaccess->newfileaccess();
        FileFingerprint fingerprint;
        std::shared_ptr<Node> n;
        bool fixfingerprint = false;
        bool fixedfingerprint = false;
        bool syncxfer = false;

        for (file_list::iterator it = files.begin(); it != files.end(); it++)
        {
            if ((*it)->syncxfer)
            {
                syncxfer = true;
            }

            if (!fixedfingerprint && (n = client->nodeByHandle((*it)->h))
                 && !(this->EqualExceptValidFlag(*n)))
            {
                LOG_debug << fingerprintIssue << "Wrong fingerprint already fixed"
                          << ". Transfer fingerprint: " << fingerprintDebugString()
                          << ". Node fingerprint: " << n->fingerprintDebugString();
                fixedfingerprint = true;
            }

            if (syncxfer && fixedfingerprint)
            {
                break;
            }
        }

        if (!fixedfingerprint && success && fa->fopen(localfilename, true, false, FSLogging::logOnError))
        {
            fingerprint.genfingerprint(fa.get());
            bool sameFingerprint = (fingerprint == *(FileFingerprint*)this);
#ifdef __ANDROID__
            // In Android maybe we can't set mtime at download
            sameFingerprint =
                sameFingerprint || (fingerprint.size == size &&
                                    memcmp(fingerprint.crc.data(), crc.data(), sizeof crc) == 0);
#endif
            if (isvalid && !sameFingerprint)
            {
                LOG_err << fingerprintIssue << "Fingerprint mismatch! Transfer fingerprint: "
                        << fingerprintDebugString()
                        << ". FA fingerprint: " << fingerprint.fingerprintDebugString()
                        << ". badfp: " << badfp.fingerprintDebugString();

                // enforce the verification of the fingerprint for sync transfers only
                if (syncxfer && (!badfp.isvalid || !(badfp == fingerprint)))
                {
                    badfp = fingerprint;
                    fa.reset();
                    chunkmacs.clear();
                    client->fsaccess->unlinklocal(localfilename);
                    return failed(API_EWRITE, committer);
                }
                else
                {
                    // We consider that mtime is different if the difference is >2
                    // due to the resolution of mtime in some filesystems (like FAT).
                    // This check prevents changes in the fingerprint due to silent
                    // errors in setmtimelocal (returning success but not setting the
                    // modification time) that seem to happen in some Android devices.
                    if (abs(mtime - fingerprint.mtime) <= 2)
                    {
                        LOG_debug << fingerprintIssue << "Fixfingerprint set to true";
                        fixfingerprint = true;
                    }
                    else
                    {
                        LOG_warn << fingerprintIssue << "Silent failure in setmtimelocal";
                    }
                }
            }
        }
        else
        {
            if (syncxfer && !fixedfingerprint && success)
            {
                transient_error = fa->retry;
                LOG_debug << fingerprintIssue << "Unable to validate fingerprint "
                          << transient_error;
            }
        }
        fa.reset();

        char me64[12];
        Base64::btoa((const byte*)&client->me, MegaClient::USERHANDLE, me64);

        if (!transient_error)
        {
            if (fingerprint.isvalid)
            {
                // set FileFingerprint on source node(s) if missing or invalid
                set<handle> nodes;
                for (file_list::iterator it = files.begin(); it != files.end(); it++)
                {
                    if ((*it)->hprivate && !(*it)->hforeign && (n = client->nodeByHandle((*it)->h))
                            && nodes.find(n->nodehandle) == nodes.end())
                    {
                        nodes.insert(n->nodehandle);

                        if ((!n->isvalid || fixfingerprint)
                                && !(fingerprint == *(FileFingerprint*)n.get())
                                && fingerprint.size == this->size)
                        {
                            attr_map attrUpdate;
                            fingerprint.serializefingerprint(&attrUpdate['c']);

                            // the fingerprint is still wrong, but....
                            // is it already being fixed?
                            AttrMap pendingAttrs;
                            if (!n->mPendingChanges.empty())
                            {
                                pendingAttrs = n->attrs;
                                n->mPendingChanges.forEachCommand([&pendingAttrs](Command* cmd)
                                {
                                    if (auto cmdSetAttr = dynamic_cast<CommandSetAttr*>(cmd))
                                    {
                                        cmdSetAttr->applyUpdatesTo(pendingAttrs);
                                    }
                                });
                            }

                            LOG_debug
                                << fingerprintIssue
                                << "Fixing fingerprint values -> fixfingerprint = "
                                << fixfingerprint
                                << ". Transfer fingerprint: " << fingerprintDebugString()
                                << ". FA fingerprint: " << fingerprint.fingerprintDebugString()
                                << ". Node fingerprint: " << n->fingerprintDebugString();

                            if (pendingAttrs.hasDifferentValue('c', attrUpdate))
                            {
                                LOG_debug << fingerprintIssue << "Fixing fingerprint";
                                client->setattr(n, std::move(attrUpdate), nullptr, false);
                            }
                            else
                            {
                                LOG_debug << fingerprintIssue << "Fingerprint already being fixed";
                            }
                        }
                    }
                }
            }

            // ...and place it in all target locations. first, update the files'
            // local target filenames, in case they have changed during the upload
            for (file_list::iterator it = files.begin(); it != files.end(); it++)
            {
                (*it)->updatelocalname();
            }

            if (!downloadDistributor)
            {
                // we keep the old one in case there was a temporary_error previously
                downloadDistributor.reset(new FileDistributor(localfilename, files.size(), mtime, *this));
            }

            set<string> keys;
            // place file in all target locations - use up to one renames, copy
            // operations for the rest
            // remove and complete successfully completed files
            for (file_list::iterator it = files.begin(); it != files.end(); )
            {
                if ((*it)->syncxfer)
                {
                    // leave sync items for later, they will be passed to the sync thread
                    ++it;
                    continue;
                }

                transient_error = false;
                success = false;

                auto finalpath = (*it)->getLocalname();

                // it may update the path to include (n) if there is a clash
                bool name_too_long = false;
                auto r = toTargetNameExistsResolution((*it)->getCollisionResolution());
                success = downloadDistributor->distributeTo(finalpath, *client->fsaccess, r, transient_error, name_too_long, nullptr);

                if (success)
                {
                    (*it)->setLocalname(finalpath);  // so the app may report an accurate final name
                }
                else if (transient_error)
                {
                    it++;
                    continue;
                }

                if (success)
                {
                    // set missing node attributes
                    if ((*it)->hprivate && !(*it)->hforeign && (n = client->nodeByHandle((*it)->h)))
                    {
                        auto localname = (*it)->getLocalname();
                        if (!client->gfxdisabled && client->gfx && client->gfx->isgfx(localname) &&
                            keys.find(n->nodekey()) == keys.end() &&    // this file hasn't been processed yet
                            client->checkaccess(n.get(), OWNER))
                        {
                            keys.insert(n->nodekey());

                            // check if restoration of missing attributes failed in the past (no access)
                            if (n->attrs.map.find('f') == n->attrs.map.end() || n->attrs.map['f'] != me64)
                            {
                                // check for missing imagery
                                int missingattr = 0;
                                if (!n->hasfileattribute(GfxProc::THUMBNAIL)) missingattr |= 1 << GfxProc::THUMBNAIL;
                                if (!n->hasfileattribute(GfxProc::PREVIEW)) missingattr |= 1 << GfxProc::PREVIEW;

                                if (missingattr)
                                {
                                    client->gfx->gendimensionsputfa(localname, NodeOrUploadHandle(n->nodeHandle()), n->nodecipher(), missingattr);
                                }

                                addAnyMissingMediaFileAttributes(n.get(), localname);
                            }
                        }
                    }
                }

                if (success)
                {
                    // prevent deletion of associated Transfer object in completed()
                    client->filecachedel(*it, &committer);
                    client->app->file_complete(*it);
                    (*it)->transfer = NULL;
                    (*it)->completed(this, (*it)->syncxfer ? PUTNODES_SYNC : PUTNODES_APP);
                    files.erase(it++);
                }
                else if (transient_error)
                {
                    LOG_debug << "Transient error completing file";
                    it++;
                }
                else if (!(*it)->failed(API_EAGAIN, client))
                {
                    File* f = (*it);
                    files.erase(it++);

                    LOG_warn << "Unable to complete transfer due to a persistent error";
                    client->filecachedel(f, &committer);
#ifdef ENABLE_SYNC
                    if (f->syncxfer)
                    {
                        client->syncs.setSyncsNeedFullSync(false, false, UNDEF);
                    }
                    else
#endif
                    {
                        downloadDistributor->removeTarget();
                    }

                    client->app->file_removed(f, API_EWRITE);
                    f->transfer = NULL;
                    f->terminated(API_EWRITE);
                }
                else
                {
                    failcount++;
                    LOG_debug << "Persistent error completing file. Failcount: " << failcount;
                    if (name_too_long)
                    {
                        LOG_warn << "Error is: name too long";
                    }
                    it++;
                }
            }

#ifdef ENABLE_SYNC
            for (file_list::iterator it = files.begin(); it != files.end(); )
            {
                // now that the file itself is moved (if started as a manual download),
                // we can let the sync copy (or move) for the sync cases
                File* f = *it;

                // pass the distribution responsibility to the sync, for sync requested downloads
                if (f->syncxfer)
                {
                    auto dl = dynamic_cast<SyncDownload_inClient*>(f);
                    assert(dl);
                    dl->downloadDistributor = downloadDistributor;

                    client->filecachedel(f, &committer);
                    client->app->file_complete(f);
                    f->transfer = NULL;
                    f->completed(this, PUTNODES_SYNC);  // sets wasCompleted == true, and the sync thread can then call the distributor
                    it = files.erase(it);
                }
                else it++;
            }
#endif // ENABLE_SYNC

            if (!files.size())
            {
                // check if we should delete the download at downloaded path
                downloadDistributor.reset();
            }
        }

        if (files.empty()) // all processed
        {
            state = TRANSFERSTATE_COMPLETED;
            assert(localfilename.isAbsolute());
            finished = true;

            client->app->transfer_complete(this);
            localfilename.clear();
            delete this;
        }
        else
        {
            // some files are still pending completion, close fa and set retry timer
            slot->fa.reset();

            LOG_debug << "Files pending completion: " << files.size() << ". Waiting for a retry.";
            LOG_debug << "First pending file: " << files.front()->name;

            slot->retrying = true;
            slot->retrybt.backoff(11);
        }
    }
    else // type == PUT
    {
        LOG_debug << client->clientname << "Upload complete: " << (files.size() ? files.front()->name : "NO_FILES") << " " << files.size();

        if (slot->fa)
        {
            slot->fa.reset();
        }

        // files must not change during a PUT transfer
        for (file_list::iterator it = files.begin(); it != files.end(); )
        {
            File *f = (*it);
            LocalPath localpath = f->getLocalname();

            LOG_debug << "Verifying upload: " << localpath.toPath(false);

            auto fa = client->fsaccess->newfileaccess();
            bool isOpen = fa->fopen(localpath, FSLogging::logOnError);
            if (!isOpen)
            {
                if (client->fsaccess->transient_error)
                {
                    LOG_warn << "Retrying upload completion due to a transient error";
                    slot->retrying = true;
                    slot->retrybt.backoff(11);
                    return;
                }
            }

            if (const bool isNotOpenAndIsNotSyncxfer =
                    (!f->syncxfer &&
                     !isOpen), // for syncs, it's ok if the file moved/renamed elsewhere since
                fingerprintChanged = isOpen && f->genfingerprint(fa.get());
                isNotOpenAndIsNotSyncxfer || fingerprintChanged)
            {
                if (isNotOpenAndIsNotSyncxfer)
                {
                    LOG_warn << "Deletion detected after upload";
                }
                else
                {
                    LOG_warn << fingerprintIssue << "Modification detected after upload! Path: "
                             << localpath.toPath(false)
                             << ". Transfer fingerprint: " << fingerprintDebugString()
                             << ". FA fingerprint: " << f->fingerprintDebugString();
                }

                it++; // the next line will remove the current item and invalidate that iterator
                removeTransferFile(API_EREAD, f, &committer);
            }
            else
            {
                it++;
            }
        }

        if (!files.size())
        {
            return failed(API_EREAD, committer);
        }


        if (!client->gfxdisabled)
        {
            // prepare file attributes for video/audio files if the file is suitable
            addAnyMissingMediaFileAttributes(NULL, localfilename);
        }

        // if this transfer is put on hold, do not complete
        client->checkfacompletion(uploadhandle, this, true);
    }
}

void Transfer::completefiles()
{
    // notify all files and give them an opportunity to self-destruct
    vector<uint32_t> &ids = client->pendingtcids[tag];
    vector<LocalPath> *pfs = NULL;
#ifdef ENABLE_SYNC
    bool wakeSyncs = false;
#endif // ENABLE_SYNC

    for (file_list::iterator it = files.begin(); it != files.end(); )
    {
        File *f = (*it);
        ids.push_back(f->dbid);
        if (f->temporaryfile)
        {
            if (!pfs)
            {
                pfs = &client->pendingfiles[tag];
            }
            pfs->push_back(f->getLocalname());
        }

        client->app->file_complete(f);

#ifdef ENABLE_SYNC
        if (f->syncxfer && type == PUT)
        {
            if (SyncUpload_inClient* put = dynamic_cast<SyncUpload_inClient*>(f))
            {
                // We are about to hand over responsibility for putnodes to the sync
                // However, if the sync gets shut down before that is sent, or the
                // operation turns out to be invalidated (eg. uploaded file deleted before putnodes)
                // then we must inform the app of the final transfer outcome.
                client->transferBackstop.remember(put->tag, put->selfKeepAlive);
                wakeSyncs = true;
                mIsSyncUpload = true; // This will prevent the deletion of file attributes upon Transfer destruction
            }
        }
#endif // ENABLE_SYNC

        f->transfer = NULL;
        f->completed(this, f->syncxfer ? PUTNODES_SYNC : PUTNODES_APP);
        files.erase(it++);
    }
    ids.push_back(dbid);

#ifdef ENABLE_SYNC
    if (wakeSyncs)
    {
        // for a sync that is only uploading, there's no other mechansim to wake it up early between tree recursions
        client->syncs.skipWait = true;
        client->syncs.waiter->notify();
    }
#endif // ENABLE_SYNC
}

DirectReadNode::DirectReadNode(MegaClient* cclient, handle ch, bool cp, SymmCipher* csymmcipher, int64_t cctriv, const char *privauth, const char *pubauth, const char *cauth)
{
    client = cclient;

    p = cp;
    h = ch;

    if (privauth)
    {
        privateauth = privauth;
    }

    if (pubauth)
    {
        publicauth = pubauth;
    }

    if (cauth)
    {
        chatauth = cauth;
    }

    symmcipher = *csymmcipher;
    ctriv = cctriv;

    retries = 0;
    size = 0;

    pendingcmd = NULL;

    dsdrn_it = client->dsdrns.end();
}

DirectReadNode::~DirectReadNode()
{
    schedule(NEVER);

    if (pendingcmd)
    {
        pendingcmd->cancel();
    }

    for (dr_list::iterator it = reads.begin(); it != reads.end(); )
    {
        delete *(it++);
    }

    client->hdrns.erase(hdrn_it);
}

void DirectReadNode::dispatch()
{
    if (reads.empty())
    {
        LOG_debug << "Removing DirectReadNode" << " [this = " << this << "]";
        delete this;
    }
    else
    {
        for (dr_list::iterator it = reads.begin(); it != reads.end(); it++)
        {
            assert((*it)->drq_it == client->drq.end());
            assert(!(*it)->drs);
        }

        schedule(DirectReadSlot::TIMEOUT_DS);
        if (!pendingcmd)
        {
            pendingcmd = new CommandDirectRead(client, this);
            client->reqs.add(pendingcmd);
        }
    }
}

// abort all active reads, remove pending reads and reschedule with app-supplied backoff
void DirectReadNode::retry(const Error& e, dstime timeleft)
{
    if (reads.empty())
    {
        LOG_warn << "Removing DirectReadNode. No reads to retry" << " [this = " << this << "]";
        delete this;
        return;
    }

    dstime minretryds = NEVER;

    retries++;

    LOG_warn << "[DirectReadNode::retry] Streaming transfer retry due to error " << e << " [this = " << this << "]";
    if (client->autodownport)
    {
        client->usealtdownport = !client->usealtdownport;
    }

    // signal failure to app, obtain minimum desired retry time
    for (dr_list::iterator it = reads.begin(); it != reads.end(); )
    {
        if ((*it)->hasValidCallback())
        {
            (*it)->abort();

            if (e)
            {
                LOG_debug << "[DirectReadNode::retry] Calling onFailure for DirectRead ("
                          << (void*)(*it) << ")"
                          << " [this = " << this << "]";
                dstime retryds = (*it)->onFailure(e, retries, timeleft);

                if (retryds < minretryds && !(e == API_ETOOMANY && e.hasExtraInfo()))
                {
                    minretryds = retryds;
                }
            }
        }
        else
        {
            // This situation should never happen
            client->sendevent(99472, "DirectRead detected with a null transfer");
        }
        if (!(*it)->hasValidCallback()) // It may have been deleted after onFailure
        {
            // Transfer is deleted
            LOG_warn << "[DirectReadNode::retry] No appdata (transfer has been deleted) for this DirectRead (" << (void*)(*it) << "). Deleting affected DirectRead" << " [this = " << this << "]";
            delete *(it++);
        }
        else it++;
    }

    if (reads.empty()) // Check again if there are DirectReads left to retry
    {
        LOG_warn << "Removing DirectReadNode. No reads left to retry" << " [this = " << this << "]";
        delete this;
        return;
    }

    if (e == API_EOVERQUOTA && timeleft)
    {
        // don't retry at least until the end of the overquota state
        client->overquotauntil = Waiter::ds + timeleft;
        if (minretryds < timeleft)
        {
            minretryds = timeleft;
        }
    }
    else if (e == API_EPAYWALL)
    {
        minretryds = NEVER;
    }

    tempurls.clear();

    if (!e || !minretryds)
    {
        // immediate retry desired
        dispatch();
    }
    else
    {
        if (EVER(minretryds))
        {
            // delayed retry desired
            schedule(minretryds);
        }
        else
        {
            // cancellation desired
            LOG_debug << "[DirectReadNode::retry] Removing DirectReadNode. Too many errors" << " [this = " << this << "]";
            delete this;
        }
    }
}

void DirectReadNode::cmdresult(const Error &e, dstime timeleft)
{
    pendingcmd = NULL;

    if (e == API_OK)
    {
        // feed all pending reads to the global read queue
        for (dr_list::iterator it = reads.begin(); it != reads.end(); it++)
        {
            DirectRead* dr = *it;
            assert(dr->drq_it == client->drq.end());

            if (dr->drbuf.tempUrlVector().empty())
            {
                // DirectRead starting
                m_off_t streamingMaxReqSize = dr->drMaxReqSize();
                LOG_debug << "Direct read node size = " << dr->drn->size << ", streaming max request size: " << streamingMaxReqSize;
                dr->drbuf.setIsRaid(dr->drn->tempurls, dr->offset, dr->offset + dr->count, dr->drn->size, streamingMaxReqSize, false);
            }
            else
            {
                // URLs have been re-requested, eg. due to temp URL expiry.  Keep any parts downloaded already
                dr->drbuf.updateUrlsAndResetPos(dr->drn->tempurls);
            }

            dr->drq_it = client->drq.insert(client->drq.end(), *it);
        }

        schedule(DirectReadSlot::TIMEOUT_DS);
    }
    else
    {
        retry(e, timeleft);
    }
}

void DirectReadNode::schedule(dstime deltads)
{
    WAIT_CLASS::bumpds();
    if (dsdrn_it != client->dsdrns.end())
    {
        client->dsdrns.erase(dsdrn_it);
    }

    if (EVER(deltads))
    {
        dsdrn_it = client->dsdrns.insert(pair<dstime, DirectReadNode*>(Waiter::ds + deltads, this));
    }
    else
    {
        dsdrn_it = client->dsdrns.end();
    }
}

DirectRead* DirectReadNode::enqueue(m_off_t offset,
                                    m_off_t count,
                                    int reqtag,
                                    DirectRead::Callback&& callback)
{
    return new DirectRead(this, count, offset, reqtag, std::move(callback));
}

size_t UnusedConn::getNum() const
{
    return mNum;
}

bool UnusedConn::CanBeReused() const
{
    return mReason == UN_NOT_ERR;
}

bool UnusedConn::setUnused(const size_t num, const UnusedReason reason)
{
    if (!isValidUnusedReason(reason))
    {
        LOG_err << "setUnused: Invalid reason: " << reason;
        assert(false);
        return false;
    }

    if (num == mNum)
    {
        return false;
    }

    mNum = num;
    mReason = reason;
    return true;
}

void UnusedConn::clear()
{
    mNum = 0;
    mReason = UN_NOT_ERR;
}

bool DirectReadSlot::processAnyOutputPieces()
{
    bool continueDirectRead = true;

    std::shared_ptr<TransferBufferManager::FilePiece> outputPiece;
    while (continueDirectRead && (outputPiece = mDr->drbuf.getAsyncOutputBufferPointer(0)))
    {
        size_t len = outputPiece->buf.datalen();
        mSpeed = mSpeedController.calculateSpeed(static_cast<m_off_t>(len));
        mMeanSpeed = mSpeedController.getMeanSpeed();
        mDr->drn->client->httpio->updatedownloadspeed(static_cast<m_off_t>(len));

        if (mDr->hasValidCallback())
        {
            mSlotThroughput.first += static_cast<m_off_t>(len);
            auto lastDataTime = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - mSlotStartTime).count();
            mSlotThroughput.second = static_cast<m_off_t>(lastDataTime);
            LOG_verbose << "DirectReadSlot -> Delivering assembled part ->"
                        << "len = " << len << ", speed = " << mSpeed << ", meanSpeed = " << (mMeanSpeed / 1024) << " KB/s"
                        << ", slotThroughput = " << ((calcThroughput(mSlotThroughput.first, mSlotThroughput.second) * 1000) / 1024) << " KB/s]" << " [this = " << this << "]";
            continueDirectRead = mDr->onData(outputPiece->buf.datastart(),
                                             static_cast<m_off_t>(len),
                                             mPos,
                                             mSpeed,
                                             mMeanSpeed);
        }
        else
        {
            LOG_err << "DirectReadSlot tried to deliver an assembled part, but the transfer doesn't exist anymore. Aborting" << " [this = " << this << "]";
            mDr->drn->client->sendevent(99472, "DirectRead detected with a null transfer");
            continueDirectRead = false;
        }
        mDr->drbuf.bufferWriteCompleted(0, true);

        if (continueDirectRead)
        {
            mPos += len;
            mDr->drn->partiallen += len;
            mDr->progress += len;
            mMinComparableThroughput = static_cast<m_off_t>(len);
        }
    }
    return continueDirectRead;
}

bool DirectReadSlot::waitForPartsInFlight() const
{
    return DirectReadSlot::WAIT_FOR_PARTS_IN_FLIGHT && isRaidedTransfer() && mWaitForParts;
}

unsigned DirectReadSlot::usedConnections() const
{
    assert(isRaidedTransfer());
    if (!isRaidedTransfer() || mReqs.empty())
    {
        LOG_err << "DirectReadSlot -> usedConnections() being used when it shouldn't"
                << " [this = " << this << "]";
        return 0;
    }
    return static_cast<unsigned>(mReqs.size()) -
           ((mUnusedConn.getNum() != static_cast<unsigned>(mReqs.size())) ? 1 : 0);
}

bool DirectReadSlot::resetConnection(size_t connectionNum)
{
    LOG_debug << "DirectReadSlot [conn " << connectionNum << "] -> resetConnection" << " [this = " << this << "]";
    assert(connectionNum < mReqs.size());
    if (connectionNum >= mReqs.size())
    {
        return false;
    }
    if (mReqs[connectionNum])
    {
        mReqs[connectionNum]->disconnect();
        mReqs[connectionNum]->status = REQ_READY;
        mThroughput[connectionNum].first = 0;
        mThroughput[connectionNum].second = 0;
    }
    mDr->drbuf.resetPart(static_cast<unsigned>(connectionNum));
    return true;
}

unsigned DirectReadSlot::getMinSpeedPerConnBytesPerSec() const
{
    const unsigned numParts = isRaidedTransfer() ? EFFECTIVE_RAIDPARTS : 1;
    const int minstrate = mDr->drn->client->minstreamingrate; // (0 == no limit, -1 == use default)
    if (minstrate < 0)
    {
        return (MIN_BYTES_PER_SECOND / numParts);
    }
    else if (minstrate == 0)
    {
        return 0;
    }
    else if (static_cast<unsigned>(minstrate) < numParts)
    {
        return 1;
    }
    else
    {
        return (static_cast<unsigned>(minstrate) / numParts);
    }
}

m_off_t DirectReadSlot::getThroughput(size_t connectionNum) const
{
    assert(connectionNum < mReqs.size());
    m_off_t connectionThroughPut = calcThroughput(mThroughput[connectionNum].first, mThroughput[connectionNum].second);
    return connectionThroughPut;
}

m_off_t DirectReadSlot::calcThroughput(m_off_t numBytes, m_off_t timeCount) const
{
    m_off_t throughput = (numBytes && timeCount) ?
                            numBytes / timeCount :
                            0;
    return throughput;
}

void DirectReadSlot::retryOnError(const size_t connectionNum, const int httpstatus)
{
    if (!isRaidedTransfer())
    {
        LOG_debug << "DirectReadSlot::retryOnError: Retrying non-raid transfer";
        retryEntireTransfer(API_EREAD);
        return;
    }

    assert(mReqs.size() == RAIDPARTS);
    const std::string logPre =
        "DirectReadSlot::retryOnError [Raided] [conn " + std::to_string(connectionNum) + "]: ";
    if (connectionNum >= mReqs.size())
    {
        LOG_err << logPre << "invalid connectionNum (out of bounds)";
        assert(false && "DirectReadSlot::retryOnError: invalid connectionNum (out of bounds)");
        retryEntireTransfer(API_EREAD);
        return;
    }

    if (const auto& failedReq = mReqs[connectionNum]; !failedReq)
    {
        LOG_err << logPre << "invalid connectionNum (not found)";
        assert(false && "DirectReadSlot::retryOnError: Cannot get Http req");
        retryEntireTransfer(API_EREAD);
        return;
    }

    if (connectionNum == mUnusedConn.getNum())
    {
        LOG_err << logPre
                << "DirectReadSlot::connectionNum provided matches the unused connectionNum.";
        assert(false && "DirectReadSlot::retryOnError: connectionNum is equal to unused");
        retryEntireTransfer(API_EREAD);
        return;
    }

    const auto reason = UnusedConn::getReasonFromHttpStatus(httpstatus);
    if (reason != UnusedConn::UN_DEFINITIVE_ERR)
    {
        LOG_err << logPre << "unexpected reason: " << reason << " httpstatus: " << httpstatus;
        assert(false && "DirectReadSlot::retryOnError: unexpected Httpstatus");
        retryEntireTransfer(API_EREAD);
        return;
    }

    if (!unusedConnectionCanBeReused())
    {
        LOG_debug << logPre
                  << "we cannot replace failed part by unused one, as it's also failed. Retrying "
                     "entire transfer";
        retryEntireTransfer(API_EREAD);
        return;
    }
    replaceConnectionByUnusedInflight(connectionNum, UnusedConn::ON_RAIDED_ERROR, reason);
}

bool DirectReadSlot::isRaidedTransfer() const
{
    return mDr->drbuf.isRaid();
}

void DirectReadSlot::retryEntireTransfer(const Error& e, const dstime timeleft)
{
    resetConnSwitchesCounters(std::chrono::steady_clock::now());
    mUnusedConn.clear();
    mDr->drn->retry(e, timeleft);
}

std::pair<std::set<size_t>, size_t> DirectReadSlot::searchSlowConnsUnderThreshold()
{
    const auto minSpeedPerConnBytesPerSec = getMinSpeedPerConnBytesPerSec();
    if (!minSpeedPerConnBytesPerSec || !isRaidedTransfer())
    {
        // if minstreamingrate == 0, no StreamingMinimumRate has been set
        // if non raided transfer there's just 1 connection
        return {{}, mReqs.size()};
    }

    std::set<size_t> slowConns;
    size_t slowestConnectionIndex = mReqs.size(); // init to `invalid` conn num
    m_off_t slowestThroughput = 0; // init slowest throughput
    for (size_t i = 0; i < mReqs.size(); ++i)
    {
        if (!mReqs[i] || i == mUnusedConn.getNum() || mReqs[i]->status != REQ_INFLIGHT)
        {
            continue;
        }

        LOG_debug << "searchSlowConnsUnderThreshold [" << i
                  << "] Throughput: " << getThroughput(i) * 1000
                  << " B/s, minSpeedPerConn: " << minSpeedPerConnBytesPerSec << " B/s";

        if (const auto isConnSpeedBelowMinThreshold =
                getThroughput(i) * 1000 < minSpeedPerConnBytesPerSec;
            isConnSpeedBelowMinThreshold)
        {
            slowConns.emplace(i);
            if (const m_off_t currentThroughput = getThroughput(i);
                !slowestThroughput || currentThroughput < slowestThroughput)
            {
                slowestConnectionIndex = i;
                slowestThroughput = currentThroughput;
            }
        }
    }

    return {slowConns, slowestConnectionIndex};
}

bool DirectReadSlot::exitDueReqsOnFlight() const
{
    // If there is any `valid` connection inflight we don't switch (we only switch when the
    // status is REQ_READY for all reqs to avoid disconnections)

    if (mNumReqsInflight == 0)
        return false;

    if (mNumReqsInflight > 1)
        return true;

    const auto it = std::find_if(std::begin(mReqs),
                                 std::end(mReqs),
                                 [](const auto& req) -> bool
                                 {
                                     return !req || req->status != REQ_READY;
                                 });

    return it != std::end(mReqs);
}

bool DirectReadSlot::unusedConnectionCanBeReused()
{
    return mUnusedConn.CanBeReused();
}

void DirectReadSlot::replaceConnectionByUnusedInflight(
    const size_t newUnusedConnection,
    const UnusedConn::ConnReplacementReason replamecementReason,
    const UnusedConn::UnusedReason unusedReason)
{
    if (!replaceConnectionByUnused(newUnusedConnection, replamecementReason, unusedReason))
        return;

    decreaseReqsInflight();

    if (mUnusedConnIncrementedInFlightReqs)
    {
        if (mNumReqsInflight > 0)
            decreaseReqsInflight();
        mUnusedConnIncrementedInFlightReqs = false;
    }
}

bool DirectReadSlot::replaceConnectionByUnused(
    const size_t newUnusedConnection,
    const UnusedConn::ConnReplacementReason replamecementReason,
    const UnusedConn::UnusedReason unusedReason)
{
    if (!isRaidedTransfer() || !unusedConnectionCanBeReused() ||
        maxUnusedConnSwitchesReached(replamecementReason) || newUnusedConnection >= mReqs.size() ||
        !mReqs[newUnusedConnection])
    {
        LOG_err << "DirectReadSlot::replaceConnectionByUnused [conn " << newUnusedConnection
                << "]: "
                << " Cannot replace unused connection by " << newUnusedConnection;
        assert(false);
        return false;
    }

    const auto prevUnusedConnection = mUnusedConn.getNum();
    LOG_debug << "DirectReadSlot::replaceConnectionByUnused: Replace conn [" << newUnusedConnection
              << "]"
              << " by unused conn [" << prevUnusedConnection << "]"
              << ". Replacement reason [" << replamecementReason << "], unused reason ["
              << unusedReason << "] [this = " << this << "]";

    increaseUnusedConnSwitches(replamecementReason);
    mDr->drbuf.setUnusedRaidConnection(static_cast<unsigned>(newUnusedConnection));
    resetConnection(prevUnusedConnection);
    mUnusedConn.setUnused(newUnusedConnection, unusedReason);
    resetConnection(newUnusedConnection);
    return true;
}

std::pair<size_t, size_t>
    DirectReadSlot::searchSlowestAndFastestConns(const size_t connectionNum) const
{
    assert(isMinComparableThroughputForThisConnection(connectionNum));
    const size_t numReqs = mReqs.size();
    size_t slowestConnection = connectionNum;
    size_t fastestConnection = connectionNum;
    bool minComparableThroughputForOtherConnection = true;

    for (size_t otherConnection = numReqs;
         (otherConnection-- > 0) && minComparableThroughputForOtherConnection;)
    {
        if ((otherConnection != connectionNum) && (otherConnection != mUnusedConn.getNum()))
        {
            const auto otherConnectionIsDone = isConnectionDone(otherConnection);
            const auto otherConnectionHasEnoughDataToCompare =
                isMinComparableThroughputForThisConnection(otherConnection);
            const auto compareCondition =
                otherConnectionHasEnoughDataToCompare && !otherConnectionIsDone;
            if (compareCondition)
            {
                m_off_t otherConnectionThroughput = getThroughput(otherConnection);
                m_off_t slowestConnectionThroughput = getThroughput(slowestConnection);
                m_off_t fastestConnectionThroughput = getThroughput(fastestConnection);
                if (otherConnectionThroughput < slowestConnectionThroughput)
                {
                    slowestConnection = otherConnection;
                }
                if (otherConnectionThroughput > fastestConnectionThroughput)
                {
                    fastestConnection = otherConnection;
                }
            }
            else // If we don't have enough throughput data for one connection, or any connection is
                 // done (we shouldn't reset it at that point), then we just skip this
            {
                // Cannot compare... will need to wait
                slowestConnection = numReqs;
                fastestConnection = numReqs;
                minComparableThroughputForOtherConnection = false;
            }
        }
    }

    LOG_debug << "DirectReadSlot [conn " << connectionNum << "]"
              << " Test slow connection -> slowest connection = " << slowestConnection
              << ", fastest connection = " << fastestConnection
              << ", unused raid connection = " << mUnusedConn.getNum()
              << ", mMinComparableThroughput = " << (mMinComparableThroughput / 1024) << " KB/s"
              << " [this = " << this << "]";

    return {slowestConnection, fastestConnection};
}

bool DirectReadSlot::slowestConnTooSlowVsFastest(const size_t connectionNum,
                                                 const size_t slowestConnection,
                                                 const size_t fastestConnection) const
{
    if (((slowestConnection == connectionNum) ||
         ((slowestConnection != mReqs.size()) &&
          (mReqs[slowestConnection]->status == REQ_READY))) &&
        (fastestConnection != slowestConnection))
    {
        m_off_t slowestConnectionThroughput = getThroughput(slowestConnection);
        m_off_t fastestConnectionThroughput = getThroughput(fastestConnection);
        if (fastestConnectionThroughput * SLOWEST_TO_FASTEST_THROUGHPUT_RATIO[0] >
            slowestConnectionThroughput * SLOWEST_TO_FASTEST_THROUGHPUT_RATIO[1])
        {
            LOG_warn << "DirectReadSlot [conn " << connectionNum << "]"
                     << " Connection " << slowestConnection << " is slow"
                     << " [slowest speed = " << ((slowestConnectionThroughput * 1000 / 1024))
                     << " KB/s"
                     << ", fastest speed = " << ((fastestConnectionThroughput * 1000 / 1024))
                     << " KB/s"
                     << ", mMinComparableThroughput = " << (mMinComparableThroughput / 1024)
                     << " KB/s]"
                     << " [total slow connections switches = " << mNumConnSwitchesSlowestPart << "]"
                     << " [current unused raid connection = " << mUnusedConn.getNum() << "]"
                     << " [this = " << this << "]";

            return true;
        }
    }
    return false;
}

bool DirectReadSlot::searchAndDisconnectSlowestConnection(const size_t connectionNum)
{
    // If mUnusedConn has failed due to an error, we won't consider it valid for replacement
    assert(connectionNum < mReqs.size());
    if (!isRaidedTransfer() || !unusedConnectionCanBeReused() || exitDueReqsOnFlight() ||
        !mReqs[connectionNum] || (connectionNum == mUnusedConn.getNum()) ||
        !isMinComparableThroughputForThisConnection(connectionNum))
    {
        return false;
    }

    if (maxUnusedConnSwitchesReached(UnusedConn::CONN_SPEED_SLOWEST_PART))
    {
        return false;
    }

    const auto [slowestConnection, fastestConnection] = searchSlowestAndFastestConns(connectionNum);
    if (slowestConnTooSlowVsFastest(connectionNum, slowestConnection, fastestConnection))
    {
        if (replaceConnectionByUnused(slowestConnection,
                                      UnusedConn::CONN_SPEED_SLOWEST_PART,
                                      UnusedConn::UN_NOT_ERR))
        {
            return true;
        }
    }
    return false;
}

bool DirectReadSlot::areAllReqsReadyOrInFlight()
{
    return std::all_of(mReqs.begin(),
                       mReqs.end(),
                       [](const std::unique_ptr<HttpReq>& req)
                       {
                           return req != nullptr &&
                                  (req->status == REQ_READY || req->status == REQ_INFLIGHT);
                       });
}

bool DirectReadSlot::decreaseReqsInflight()
{
    if (isRaidedTransfer())
    {
        LOG_verbose << "Decreasing counter of total requests inflight: " << mNumReqsInflight << " - 1" << " [this = " << this << "]";
        assert(mNumReqsInflight > 0);
        --mNumReqsInflight;
        if ((mUnusedConn.getNum() < mReqs.size()) &&
            (mReqs[mUnusedConn.getNum()]->status != REQ_DONE) &&
            (mNumReqsInflight == (static_cast<unsigned>(mReqs.size()) - usedConnections())))
        {
            mNumReqsInflight = 0;
            mUnusedConnIncrementedInFlightReqs = false;
        }
        if (mNumReqsInflight == 0)
        {
            LOG_verbose << "Wait for parts set to false" << " [this = " << this << "]";
            // waitForParts could be true at this point if there were connections with REQ_DONE status which didn't increase the inflight counter
            mWaitForParts = false;
            mMaxChunkSubmitted = 0;
        }
        return true;
    }
    return false;
}

bool DirectReadSlot::increaseReqsInflight()
{
    if (isRaidedTransfer())
    {
        LOG_verbose << "Increasing counter of total requests inflight: " << mNumReqsInflight << " + 1 = " << (mNumReqsInflight + 1) << " [this = " << this << "]";
        assert(mNumReqsInflight < mReqs.size());
        ++mNumReqsInflight;
        if (mNumReqsInflight == static_cast<unsigned>(mReqs.size()))
        {
            assert(!mWaitForParts);
            LOG_verbose << "Wait for parts set to true" << " [this = " << this << "]";
            mWaitForParts = true;
            resetWatchdogPartialValues();
        }
        return true;
    }
    return false;
}

bool DirectReadSlot::isConnectionDone(const size_t connectionNum) const
{
    return mReqs[connectionNum] &&
           (mReqs[connectionNum]->status == REQ_DONE ||
            (mReqs[connectionNum]->pos ==
             mDr->drbuf.transferSize(static_cast<unsigned>(connectionNum))));
}

std::pair<int, m_off_t> DirectReadSlot::getMinAndMeanSpeed(const dstime dsSinceLastWatch)
{
    // if minstreamingrate is zero, there's no minimum acceptable rate for streaming.
    const m_off_t meanspeed = (10 * mDr->drn->partiallen) / dsSinceLastWatch;
    int minspeed = mDr->drn->client->minstreamingrate;
    if (minspeed < 0)
    {
        LOG_warn << "DirectReadSlot: Watchdog -> Set min speed as MIN_BYTES_PER_SECOND("
                 << MIN_BYTES_PER_SECOND << ") to compare with average speed."
                 << " [this = " << this << "]";
        minspeed = MIN_BYTES_PER_SECOND;
    }

    LOG_debug << "DirectReadSlot: Watchdog -> Mean speed: " << meanspeed
              << " B/s. Min speed: " << minspeed << " B/s [Partial len: " << mDr->drn->partiallen
              << ". Ds: " << dsSinceLastWatch << "]"
              << " [this = " << this << "]";
    return {minspeed, meanspeed};
}

void DirectReadSlot::resetWatchdogPartialValues()
{
    mDr->drn->partiallen = 0;
    mDr->drn->partialstarttime = Waiter::ds;
};

bool DirectReadSlot::watchOverDirectReadPerformance()
{
    auto dsSinceLastWatch = Waiter::ds - mDr->drn->partialstarttime;
    if (dsSinceLastWatch <= MEAN_SPEED_INTERVAL_DS)
    {
        return false;
    }

    const auto [minTransferspeed, transferMeanspeed] = getMinAndMeanSpeed(dsSinceLastWatch);
    if (!mDr->hasValidCallback())
    {
        LOG_err << "DirectReadSlot Watchdog: Transfer is already deleted."
                << (transferMeanspeed >= minTransferspeed ?
                        " Transfer speed too low for streaming, Skipping retry" :
                        "")
                << " [this = " << this << "]";
        mDr->drn->client->sendevent(99472, "DirectRead detected with a null transfer");
        delete mDr;
        return false;
    }

    if (!minTransferspeed)
    {
        // no limits set by client, so no performance check is required
        LOG_verbose << "DirectReadSlot Watchdog: No minTransferspeed";
        resetWatchdogPartialValues();
        return false;
    }

    if (isAnyRaidedPartFailed())
    {
        LOG_warn << "DirectReadSlot Watchdog: a raided part has already reported as failed. "
                    "Skipping watchdog";
        resetWatchdogPartialValues();
        return false;
    }

    const auto [slowConns, slowestConnectionIndex] = searchSlowConnsUnderThreshold();
    LOG_debug << "DirectReadSlot Watchdog: number of detected slow connections = "
              << slowConns.size();
    if (slowConns.empty())
    {
        assert(getMinSpeedPerConnBytesPerSec() &&
               "If no minstrate is set, we should have already exited");

        if (transferMeanspeed < minTransferspeed)
        {
            LOG_debug << "watchOverDirectReadPerformance: slowConns empty and transferMeanspeed: "
                      << transferMeanspeed << " B/s < "
                      << " minTransferspeed: " << minTransferspeed << " B/s";
            assert(false && "slowConns empty and transferMeanspeed < minTransferspeed");
            retryEntireTransfer(API_EAGAIN);
            return true;
        }

        resetWatchdogPartialValues();
        return false;
    }

    if (slowConns.size() <= MAX_SIMULTANEOUS_SLOW_RAIDED_CONNS)
    {
        if (const auto unusedConnNotReusable =
                !unusedConnectionCanBeReused() ||
                maxUnusedConnSwitchesReached(UnusedConn::TRANSFER_OR_CONN_SPEED_UNDER_THRESHOLD);
            unusedConnNotReusable || mNumReqsInflight < EFFECTIVE_RAIDPARTS)
        {
            if (mNumReqsInflight < EFFECTIVE_RAIDPARTS && areAllReqsReadyOrInFlight())
            {
                LOG_warn << "DirectReadSlot Watchdog: Skipping retryEntireTransfer. Wait until "
                            "requests with status REQ_INFLIGHT reach REQ_READY. Also, increase "
                            "UnusedConnSwitches(TRANSFER_OR_CONN_SPEED_UNDER_THRESHOLD) to prevent "
                            "this scenario from repeating indefinitely.";

                increaseUnusedConnSwitches(UnusedConn::TRANSFER_OR_CONN_SPEED_UNDER_THRESHOLD);
                resetWatchdogPartialValues();
                return false;
            }

            LOG_err
                << "DirectReadSlot Watchdog: [conn " << slowestConnectionIndex << "]: "
                << " Cannot replace unused connection by " << slowestConnectionIndex
                << " . Reason: unusedConnection cannot be reused or maxUnusedConnSwitchesReached";
            retryEntireTransfer(API_EAGAIN);
            return true;
        }

        replaceConnectionByUnusedInflight(slowestConnectionIndex,
                                          UnusedConn::TRANSFER_OR_CONN_SPEED_UNDER_THRESHOLD,
                                          UnusedConn::UN_NOT_ERR);

        resetWatchdogPartialValues();
        return false;
    }

    LOG_err << "DirectReadSlot Watchdog: [conn " << slowestConnectionIndex << "]: "
            << " Cannot replace unused connection by " << slowestConnectionIndex
            << " . Reason: too many slow connections detected";
    retryEntireTransfer(API_EAGAIN);
    return true;
}

void DirectReadSlot::resetConnSwitchesCounters(const std::chrono::steady_clock::time_point& now)
{
    mNumConnSwitchesSlowestPart = 0;
    mNumConnSwitchesBelowSpeedThreshold = 0;
    mNumConnDetectedBelowSpeedThreshold.clear();
    mConnectionSwitchesLimitLastReset = now;
}

void DirectReadSlot::resetConnSwitchesCountersIfTimeoutExpired()
{
    if (const auto now = std::chrono::steady_clock::now();
        ((now - mConnectionSwitchesLimitLastReset) > CONNECTION_SWITCHES_LIMIT_RESET_TIME))
    {
        resetConnSwitchesCounters(now);
    }
}

bool DirectReadSlot::isAnyRaidedPartFailed() const
{
    if (!isRaidedTransfer())
    {
        return false;
    }

    return std::any_of(std::begin(mReqs),
                       std::end(mReqs),
                       [](const std::unique_ptr<HttpReq>& req)
                       {
                           return req && req->status == REQ_FAILURE;
                       });
}

bool DirectReadSlot::doio()
{
    const auto isRaid = isRaidedTransfer();
    unsigned minSpeedPerConnBytesPerSec = getMinSpeedPerConnBytesPerSec();
    if (!minSpeedPerConnBytesPerSec)
    {
        minSpeedPerConnBytesPerSec = 1; // No limit (1 B/s)
    }

    if (isRaid)
    {
        // round up to a RAIDSECTOR divisible value
        minSpeedPerConnBytesPerSec = (minSpeedPerConnBytesPerSec + RAIDSECTOR - 1) &
                                     ~(static_cast<unsigned>(RAIDSECTOR) - 1);
    }

    resetConnSwitchesCountersIfTimeoutExpired();

    for (unsigned int connectionNum = static_cast<unsigned int>(mReqs.size()); connectionNum--;)
    {
        std::unique_ptr<HttpReq>& req = mReqs[connectionNum];
        bool isNotUnusedConnection = !isRaid || (connectionNum != mUnusedConn.getNum());
        bool submitCondition = req && isNotUnusedConnection &&
                               (req->status == REQ_INFLIGHT || req->status == REQ_SUCCESS);

        if (submitCondition)
        {
            if (req->in.size())
            {
                unsigned n = static_cast<unsigned>(req->in.size());
                auto lastDataTime = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - req->postStartTime).count();
                m_off_t chunkTime = static_cast<m_off_t>(lastDataTime) - mThroughput[connectionNum].second;

                unsigned minChunkSize;
                m_off_t maxChunkSize, aggregatedThroughput;
                if (req->status == REQ_INFLIGHT)
                {
                    m_off_t updatedThroughput = calcThroughput(mThroughput[connectionNum].first + n, mThroughput[connectionNum].second + chunkTime) * 1000;
                    m_off_t chunkThroughput = calcThroughput(static_cast<m_off_t>(n), chunkTime) * 1000;
                    aggregatedThroughput = (chunkThroughput + updatedThroughput) / 2;
                    maxChunkSize = aggregatedThroughput;
                    // 16KB as min chunk divisible size to submit. If the user's speed is even lower than 16KB/s per connection, then respect the minSpeedPerConnection.
                    // This is to avoid small chunks to be assembled (if raid) and delivered.
                    unsigned minChunkDivisibleSize =
                        maxChunkSize < (16 * 1024) ? minSpeedPerConnBytesPerSec :
                                                     16 * 1024; // 16KB is divisible by RAIDSECTOR:
                                                                // works for RAID and NON-RAID

                    if (mMaxChunkSubmitted && maxChunkSize && ((std::max(static_cast<unsigned>(maxChunkSize), mMaxChunkSubmitted) / std::min(static_cast<unsigned>(maxChunkSize), mMaxChunkSubmitted)) == 1))
                    {
                        // Avoid small chunks due to fragmentation caused by similar (but different) chunk sizes (compared to max submitted chunk size)
                        maxChunkSize = mMaxChunkSubmitted;
                    }
                    minChunkSize = std::max(static_cast<unsigned>(maxChunkSize), minChunkDivisibleSize);
                    n = (n >= minChunkSize) ?
                            (n / minChunkDivisibleSize) * minChunkDivisibleSize :
                            0;
                }
                else
                {
                    minChunkSize = 0;
                    maxChunkSize = n;
                    aggregatedThroughput = 0;
                }

                assert(!isRaidedTransfer() || (req->status == REQ_SUCCESS) ||
                       ((n % RAIDSECTOR) == 0));
                if ((isRaidedTransfer() && (req->status != REQ_SUCCESS) && ((n % RAIDSECTOR) != 0)))
                {
                    LOG_err << "DirectReadSlot [conn " << connectionNum
                            << "] ERROR: (isRaid() && (req->status != REQ_SUCCESS) && ((n % "
                               "RAIDSECTOR) != 0)"
                            << " n = " << n << ", req->in.size = " << req->in.size()
                            << ", req->status = " << req->status.load()
                            << ", adapted maxChunkSize = " << maxChunkSize
                            << ", mMaxChunkSize = " << mMaxChunkSize
                            << ", submitted = " << mThroughput[connectionNum].first
                            << " [this = " << this << "]";
                }

                if (n)
                {
                    mThroughput[connectionNum].first += static_cast<m_off_t>(n);
                    mThroughput[connectionNum].second += chunkTime;
                    LOG_verbose << "DirectReadSlot [conn " << connectionNum << "] ->"
                                << " FilePiece's going to be submitted: n = " << n
                                << ", req->in.size = " << req->in.size()
                                << ", req->in.capacity = " << req->in.capacity()
                                << " [minChunkSize = " << minChunkSize
                                << ", mMaxChunkSize = " << mMaxChunkSize
                                << ", reqs.size = " << mReqs.size() << ", req->status = "
                                << std::string(req->status == REQ_READY    ? "REQ_READY" :
                                               req->status == REQ_INFLIGHT ? "REQ_INFLIGHT" :
                                               req->status == REQ_SUCCESS  ? "REQ_SUCCESS" :
                                                                             "REQ_SOMETHING")
                                << ", req->httpstatus = " << req->httpstatus
                                << ", req->contentlength = " << req->contentlength
                                << ", numReqsInflight = " << mNumReqsInflight
                                << ", unusedRaidConnection = " << mUnusedConn.getNum() << "]"
                                << " [chunk throughput = "
                                << ((calcThroughput(static_cast<m_off_t>(n), chunkTime) * 1000) /
                                    1024)
                                << " KB/s"
                                << ", average throughput = "
                                << (getThroughput(connectionNum) * 1000 / 1024) << " KB/s"
                                << ", aggregated throughput = " << (aggregatedThroughput / 1024)
                                << " KB/s"
                                << ", maxChunkSize = " << (maxChunkSize / 1024) << " KBs]"
                                << ", [req->pos_pre = " << (req->pos)
                                << ", req->pos_now = " << (req->pos + n) << "]"
                                << " [this = " << this << "]";
                    RaidBufferManager::FilePiece* np = new RaidBufferManager::FilePiece(req->pos, n);
                    memcpy(np->buf.datastart(), req->in.c_str(), n);

                    req->in.erase(0, n);
                    req->contentlength -= n;
                    req->bufpos = 0;
                    req->pos += n;

                    unsigned submittingConnection = isRaid ? connectionNum : 0;
                    mDr->drbuf.submitBuffer(submittingConnection, np);

                    if (n > mMaxChunkSubmitted)
                    {
                        mMaxChunkSubmitted = n;
                    }
                }

                if (req->httpio)
                {
                    req->httpio->lastdata = Waiter::ds;
                }
                req->lastdata = Waiter::ds;

                // we might have a raid-reassembled block to write now, or this very block in non-raid
                if (n && !processAnyOutputPieces())
                {
                    LOG_debug << "DirectReadSlot [conn " << connectionNum << "] Transfer is finished after processing pending output pieces. Removing DirectRead" << " [this = " << this << "]";
                    delete mDr;
                    return true;
                }

                mDr->drn->schedule(DirectReadSlot::TEMPURL_TIMEOUT_DS);
            }

            if (req->status == REQ_SUCCESS && !req->in.size())
            {
                decreaseReqsInflight();
                req->status = REQ_READY;
            }
        }

        if (!req || req->status == REQ_READY)
        {
            bool waitForOthers = isRaid ? waitForPartsInFlight() : false;
            if (!waitForOthers)
            {
                if (searchAndDisconnectSlowestConnection(connectionNum))
                {
                    LOG_verbose
                        << "DirectReadSlot [conn " << connectionNum
                        << "] Continue DirectReadSlot loop after disconnecting slow connection "
                        << mUnusedConn.getNum() << " [this = " << this << "]";
                }

                bool newBufferSupplied = false, pauseForRaid = false;
                std::pair<m_off_t, m_off_t> posrange =
                    mDr->drbuf.nextNPosForConnection(connectionNum,
                                                     newBufferSupplied,
                                                     pauseForRaid);
                if (newBufferSupplied)
                {
                    if (static_cast<unsigned>(connectionNum) == mUnusedConn.getNum())
                    {
                        // Count the "unused connection" (restored by parity) as a req inflight, so we avoid to exec this piece of code needlessly
                        increaseReqsInflight();
                        mUnusedConnIncrementedInFlightReqs = true;
                    }
                    // we might have a raid-reassembled block to write, or a previously loaded block, or a skip block to process.
                    if (!processAnyOutputPieces())
                    {
                        LOG_debug << "DirectReadSlot [conn " << connectionNum << "] Transfer is finished after processing pending output pieces (on new buffer supplied). Removing DirectRead" << " [this = " << this << "]";
                        delete mDr;
                        return true;
                    }
                }
                else if (!pauseForRaid)
                {
                    if (posrange.first >= posrange.second)
                    {
                        if (req)
                        {
                            LOG_verbose << "DirectReadSlot [conn " << connectionNum << "] Request status set to DONE" << " [this = " << this << "]";
                            req->status = REQ_DONE;
                        }
                        bool allDone = true;
                        for (size_t i = mReqs.size(); i--;)
                        {
                            if (mReqs[i] && mReqs[i]->status != REQ_DONE)
                            {
                                allDone = false;
                            }
                        }
                        if (allDone)
                        {
                            LOG_debug << "DirectReadSlot [conn " << connectionNum << "] All requests are DONE: Delete read request and direct read slot" << " [this = " << this << "]";

                            // remove and delete completed read request, then remove slot
                            delete mDr;
                            return true;
                        }
                    }
                    else
                    {
                        if (!mDr->hasValidCallback())
                        {
                            LOG_err << "DirectReadSlot [conn " << connectionNum << "] There is a chunk request, but transfer is already deleted. This should never happen. Aborting" << " [this = " << this << "]";
                            mDr->drn->client->sendevent(99472, "DirectRead detected with a null transfer");
                            delete mDr;
                            return true;
                        }

                        if (static_cast<size_t>(connectionNum) == mUnusedConn.getNum())
                        {
                            LOG_err << "DirectReadSlot [conn " << connectionNum
                                    << "] We are processing unused connection";
                            assert(false && "Processing unused connection when we should not!");
                        }

                        if (!req)
                        {
                            mReqs[connectionNum] = std::make_unique<HttpReq>(true);
                        }

                        if (!isRaidedTransfer())
                        {
                            // Chunk size limit for non-raid: MAX_DELIVERY_CHUNK.
                            // If the whole chunk is requested (file size), with the same request all the time,
                            // the throughput could be too low for long periods of time, depending on the actual TCP congestion algorithm.
                            posrange.second =
                                std::min(posrange.second,
                                         posrange.first + DirectReadSlot::MAX_DELIVERY_CHUNK);
                        }

                        char buf[128];
                        snprintf(buf, sizeof(buf), "/%" PRIu64 "-", posrange.first);
                        if (mDr->count)
                        {
                            snprintf(strchr(buf, 0),
                                     sizeof(buf) - strlen(buf),
                                     "%" PRIu64,
                                     posrange.second - 1);
                        }

                        req->pos = posrange.first;
                        req->posturl = adjustURLPort(mDr->drbuf.tempURL(connectionNum));
                        req->posturl.append(buf);
                        LOG_debug << "DirectReadSlot [conn " << connectionNum
                                  << "] Request chunk of size "
                                  << (posrange.second - posrange.first)
                                  << " (request status = " << req->status.load() << ")"
                                  << " [this = " << this << "]";
                        LOG_debug << req->getLogName() << "POST URL: " << req->posturl;

                        mThroughput[connectionNum].first = 0;
                        mThroughput[connectionNum].second = 0;
                        req->in.reserve(mMaxChunkSize + (mMaxChunkSize/2));
                        req->post(mDr->drn->client); // status will go to inflight or fail
                        LOG_verbose << req->getLogName() << "DirectReadSlot [conn " << connectionNum
                                    << "] POST done (new request status = " << req->status.load()
                                    << ")"
                                    << " [this = " << this << "]";

                        mDr->drbuf.transferPos(connectionNum) = posrange.second;
                        increaseReqsInflight();
                    }
                }
            }
        }

        if (req && req->status == REQ_FAILURE)
        {
            LOG_warn << "DirectReadSlot [conn " << connectionNum
                     << "] Request status is FAILURE [Request status = " << req->status.load()
                     << ", HTTP status = " << req->httpstatus << "]"
                     << " [this = " << this << "]";
            onFailure(req, static_cast<size_t>(connectionNum));
            return true;
        }

        if (watchOverDirectReadPerformance())
        {
            LOG_debug << "DirectReadSlot [conn " << connectionNum << "] DirectReadSlot will be retried" << " [this = " << this << "]";
            return true;
        }
    }

    return false;
}

void DirectReadSlot::onFailure(std::unique_ptr<HttpReq>& req, const size_t connectionNum)
{
    if (!mDr->hasValidCallback())
    {
        LOG_err << "DirectReadSlot [conn " << connectionNum
                << "] Request failed, but transfer is already deleted. Aborting"
                << " [this = " << this << "]";
        mDr->drn->client->sendevent(99472, "DirectRead detected with a null transfer");
        delete mDr;
    }
    else
    {
        if (auto isBwOverquotaErr = req->httpstatus == 509; isBwOverquotaErr)
        {
            LOG_warn
                << "DirectReadSlot Bandwidth overquota from storage server for streaming transfer"
                << " [this = " << this << "]";
            dstime backoff = mDr->drn->client->overTransferQuotaBackoff(req.get());
            retryEntireTransfer(API_EOVERQUOTA, backoff);
        }
        else
        {
            retryOnError(connectionNum, req->httpstatus);
        }
    }
}

// abort active read, remove from pending queue
void DirectRead::abort()
{
    delete drs;
    drs = NULL;

    if (drq_it != drn->client->drq.end())
    {
        drn->client->drq.erase(drq_it);
        drq_it = drn->client->drq.end();
    }
}

m_off_t DirectRead::drMaxReqSize() const
{
    m_off_t numParts = drn->tempurls.size() == RAIDPARTS ?
                                    static_cast<m_off_t>(EFFECTIVE_RAIDPARTS) :
                                    static_cast<m_off_t>(drn->tempurls.size());
    return std::max(drn->size / numParts, TransferSlot::MAX_REQ_SIZE);
}

void DirectRead::revokeCallback(void* appData)
{
    assert(callback);
    DirectRead::CallbackParam param{std::in_place_type<DirectRead::Revoke>, appData};
    callback(param);
}

bool DirectRead::onData(byte* buffer,
                        m_off_t len,
                        m_off_t theOffset,
                        m_off_t speed,
                        m_off_t meanSpeed)
{
    assert(callback);
    DirectRead::CallbackParam param{std::in_place_type<DirectRead::Data>,
                                    buffer,
                                    len,
                                    theOffset,
                                    speed,
                                    meanSpeed};
    callback(param);
    return std::get<DirectRead::Data>(param).ret;
}

dstime DirectRead::onFailure(const Error& e, int retry, dstime timeLeft)
{
    assert(callback);
    DirectRead::CallbackParam param{std::in_place_type<DirectRead::Failure>, e, retry, timeLeft};
    callback(param);
    return std::get<DirectRead::Failure>(param).ret;
}

bool DirectRead::hasValidCallback()
{
    assert(callback);
    DirectRead::CallbackParam param{std::in_place_type<DirectRead::IsValid>};
    callback(param);
    return std::get<DirectRead::IsValid>(param).ret;
}

DirectRead::DirectRead(DirectReadNode* cdrn,
                       m_off_t ccount,
                       m_off_t coffset,
                       int creqtag,
                       Callback&& callback):
    drbuf(this),
    callback(std::move(callback))
{
    LOG_debug << "[DirectRead::DirectRead] New DirectRead "
              << "[this = " << this << "]";
    drn = cdrn;

    count = ccount;
    offset = coffset;
    progress = 0;
    reqtag = creqtag;

    drs = NULL;

    reads_it = drn->reads.insert(drn->reads.end(), this);

    if (!drn->tempurls.empty())
    {
        // we already have tempurl(s): queue for immediate fetching
        m_off_t streamingMaxReqSize = drMaxReqSize();
        LOG_debug << "Direct read start -> direct read node size = " << drn->size << ", streaming max request size: " << streamingMaxReqSize;
        drbuf.setIsRaid(drn->tempurls, offset, offset + count, drn->size, streamingMaxReqSize, false);
        drq_it = drn->client->drq.insert(drn->client->drq.end(), this);
    }
    else
    {
        // no tempurl yet or waiting for a retry
        drq_it = drn->client->drq.end();
    }
}

DirectRead::~DirectRead()
{
    LOG_debug << "Deleting DirectRead" << " [this = " << this << "]";
    abort();

    if (reads_it != drn->reads.end())
    {
        drn->reads.erase(reads_it);
    }
}

std::string DirectReadSlot::adjustURLPort(std::string url)
{
    if (Utils::startswith(url, "http:"))
    {
        size_t portendindex = url.find("/", 8);
        size_t portstartindex = url.find(":", 8);

        if (portendindex != string::npos)
        {
            if (portstartindex == string::npos)
            {
                if (mDr->drn->client->usealtdownport)
                {
                    LOG_debug << "Enabling alternative port for streaming transfer";
                    url.insert(portendindex, ":8080");
                }
            }
            else
            {
                if (!mDr->drn->client->usealtdownport)
                {
                    LOG_debug << "Disabling alternative port for streaming transfer";
                    url.erase(portstartindex, portendindex - portstartindex);
                }
            }
        }
    }
    return url;
}

// request DirectRead's range via tempurl
DirectReadSlot::DirectReadSlot(DirectRead* cdr)
{
    LOG_debug << "[DirectReadSlot::DirectReadSlot] New DirectReadSlot [cdr = " << (void*)cdr << "]" << " [this = " << this << "]";
    mDr = cdr;

    mPos = mDr->offset + mDr->progress;
    mDr->nextrequestpos = mPos;

    mSpeed = mMeanSpeed = 0;

    assert(mReqs.empty());
    const auto numReqs = isRaidedTransfer() ? mDr->drbuf.tempUrlVector().size() : 1;
    assert(isRaidedTransfer() ? (numReqs == RAIDPARTS) : 1);
    for (size_t i = numReqs; i--; )
    {
        mReqs.push_back(std::make_unique<HttpReq>(true));
        mReqs.back()->status = REQ_READY;
        mReqs.back()->type = REQ_BINARY;
    }
    LOG_verbose << "[DirectReadSlot::DirectReadSlot] Num requests: " << numReqs
                << " [this = " << this << "]";
    mThroughput.resize(mReqs.size());
    mUnusedConn.clear();

    unsigned auxUnused = static_cast<unsigned>(mReqs.size());
    if (isRaidedTransfer())
    {
        const auto un = mDr->drbuf.getUnusedRaidConnection();
        auxUnused = un < numReqs ? un : DEFAULT_UNUSED_CONN_INDEX;
    }

    LOG_verbose << "[DirectReadSlot::DirectReadSlot] Set initial unused raid connection to "
                << auxUnused << " [this = " << this << "]";
    mDr->drbuf.setUnusedRaidConnection(auxUnused);
    mUnusedConn.setUnused(auxUnused, UnusedConn::UN_NOT_ERR);

    mNumConnDetectedBelowSpeedThreshold.clear();
    mNumConnSwitchesSlowestPart = 0;
    mNumConnSwitchesBelowSpeedThreshold = 0;
    mNumReqsInflight = 0;
    mWaitForParts = false;
    mMaxChunkSubmitted = 0;
    mDrs_it = mDr->drn->client->drss.insert(mDr->drn->client->drss.end(), this);

    mDr->drn->partiallen = 0;
    mDr->drn->partialstarttime = Waiter::ds;
    mMaxChunkSize = static_cast<unsigned>(static_cast<unsigned>(DirectReadSlot::MAX_DELIVERY_CHUNK) / (mReqs.size() == static_cast<unsigned>(RAIDPARTS) ? (static_cast<unsigned>(EFFECTIVE_RAIDPARTS)) : mReqs.size()));
    if (isRaidedTransfer())
    {
        mMaxChunkSize -= mMaxChunkSize % RAIDSECTOR;
    }
    mMinComparableThroughput = DirectReadSlot::DEFAULT_MIN_COMPARABLE_THROUGHPUT;
    mSlotStartTime = std::chrono::steady_clock::now();
    mConnectionSwitchesLimitLastReset = std::chrono::steady_clock::now();
}

DirectReadSlot::~DirectReadSlot()
{
    mDr->drn->client->drss.erase(mDrs_it);
    LOG_debug << "Deleting DirectReadSlot" << " [this = " << this << "]";
}

bool priority_comparator(const LazyEraseTransferPtr& i, const LazyEraseTransferPtr& j)
{
    return (i.transfer ? i.transfer->priority : i.preErasurePriority) < (j.transfer ? j.transfer->priority : j.preErasurePriority);
}

TransferList::TransferList()
{
    currentpriority = PRIORITY_START;
}

void TransferList::addtransfer(Transfer *transfer, TransferDbCommitter& committer, bool startFirst)
{
    if (transfer->state != TRANSFERSTATE_PAUSED)
    {
        transfer->state = TRANSFERSTATE_QUEUED;
    }

    assert(transfer->type == PUT || transfer->type == GET);

    if (!transfer->priority)
    {
        if (startFirst && transfers[transfer->type].size())
        {
            transfer_list::iterator dstit = transfers[transfer->type].begin();
            transfer->priority = dstit->transfer->priority - PRIORITY_STEP;
            prepareIncreasePriority(transfer, transfers[transfer->type].end(), dstit, committer);
            transfers[transfer->type].push_front(transfer);
        }
        else
        {
            currentpriority += PRIORITY_STEP;
            transfer->priority = currentpriority;
            assert(!transfers[transfer->type].size() || transfers[transfer->type][transfers[transfer->type].size() - 1]->priority < transfer->priority);
            transfers[transfer->type].push_back(transfer);
        }

        client->transfercacheadd(transfer, &committer);
    }
    else
    {
        transfer_list::iterator it = std::lower_bound(transfers[transfer->type].begin(), transfers[transfer->type].end(), LazyEraseTransferPtr(transfer), priority_comparator);
        assert(it == transfers[transfer->type].end() || it->transfer->priority != transfer->priority);
        transfers[transfer->type].insert(it, transfer);
    }
}

void TransferList::removetransfer(Transfer *transfer)
{
    transfer_list::iterator it;
    if (getIterator(transfer, it, true))
    {
        transfers[transfer->type].erase(it);
    }
}

void TransferList::movetransfer(Transfer *transfer, Transfer *prevTransfer, TransferDbCommitter& committer)
{
    transfer_list::iterator dstit;
    if (getIterator(prevTransfer, dstit))
    {
        movetransfer(transfer, dstit, committer);
    }
}

void TransferList::movetransfer(Transfer *transfer, unsigned int position, TransferDbCommitter& committer)
{
    transfer_list::iterator dstit;
    if (position >= transfers[transfer->type].size())
    {
        dstit = transfers[transfer->type].end();
    }
    else
    {
        dstit = transfers[transfer->type].begin() + position;
    }

    transfer_list::iterator it;
    if (getIterator(transfer, it))
    {
        movetransfer(it, dstit, committer);
    }
}

void TransferList::movetransfer(Transfer *transfer, transfer_list::iterator dstit, TransferDbCommitter& committer)
{
    transfer_list::iterator it;
    if (getIterator(transfer, it))
    {
        movetransfer(it, dstit, committer);
    }
}

void TransferList::movetransfer(transfer_list::iterator it, transfer_list::iterator dstit, TransferDbCommitter& committer)
{
    if (it == dstit)
    {
        LOG_warn << "Trying to move before the same transfer";
        return;
    }

    if ((it + 1) == dstit)
    {
        LOG_warn << "Trying to move to the same position";
        return;
    }

    Transfer *transfer = (*it);
    assert(transfer->type == PUT || transfer->type == GET);
    if (dstit == transfers[transfer->type].end())
    {
        LOG_debug << "Moving transfer to the last position";
        prepareDecreasePriority(transfer, it, dstit);

        transfers[transfer->type].erase(it);
        currentpriority += PRIORITY_STEP;
        transfer->priority = currentpriority;
        assert(!transfers[transfer->type].size() || transfers[transfer->type][transfers[transfer->type].size() - 1]->priority < transfer->priority);
        transfers[transfer->type].push_back(transfer);
        client->transfercacheadd(transfer, &committer);
        client->app->transfer_update(transfer);
        return;
    }

    int srcindex = int(std::distance(transfers[transfer->type].begin(), it));
    int dstindex = int(std::distance(transfers[transfer->type].begin(), dstit));
    LOG_debug << "Moving transfer from " << srcindex << " to " << dstindex;

    uint64_t prevpriority = 0;
    uint64_t nextpriority = 0;

    nextpriority = dstit->transfer->priority;
    if (dstit != transfers[transfer->type].begin())
    {
        transfer_list::iterator previt = dstit - 1;
        prevpriority = previt->transfer->priority;
    }
    else
    {
        prevpriority = nextpriority - 2 * PRIORITY_STEP;
    }

    uint64_t newpriority = (prevpriority + nextpriority) / 2;
    LOG_debug << "Moving transfer between priority " << prevpriority << " and " << nextpriority << ". New: " << newpriority;
    if (prevpriority == newpriority)
    {
        LOG_warn << "There is no space for the move. Adjusting priorities.";
        int positions = dstindex;
        uint64_t fixedPriority = transfers[transfer->type][0]->priority -
                                 PRIORITY_STEP * (static_cast<uint64_t>(positions) + 1);
        for (int i = 0; i < positions; i++)
        {
            Transfer* t = transfers[transfer->type][static_cast<size_t>(i)];
            LOG_debug << "Adjusting priority of transfer " << i << " to " << fixedPriority;
            t->priority = fixedPriority;
            client->transfercacheadd(t, &committer);
            client->app->transfer_update(t);
            fixedPriority += PRIORITY_STEP;
        }
        newpriority = fixedPriority;
        LOG_debug << "Fixed priority: " << fixedPriority;
    }

    transfer->priority = newpriority;
    if (srcindex > dstindex)
    {
        prepareIncreasePriority(transfer, it, dstit, committer);
    }
    else
    {
        prepareDecreasePriority(transfer, it, dstit);
        dstindex--;
    }

    transfers[transfer->type].erase(it);
    transfer_list::iterator fit = transfers[transfer->type].begin() + dstindex;
    assert(fit == transfers[transfer->type].end() || fit->transfer->priority != transfer->priority);
    transfers[transfer->type].insert(fit, transfer);
    client->transfercacheadd(transfer, &committer);
    client->app->transfer_update(transfer);
}

void TransferList::movetofirst(Transfer *transfer, TransferDbCommitter& committer)
{
    movetransfer(transfer, transfers[transfer->type].begin(), committer);
}

void TransferList::movetofirst(transfer_list::iterator it, TransferDbCommitter& committer)
{
    Transfer *transfer = (*it);
    movetransfer(it, transfers[transfer->type].begin(), committer);
}

void TransferList::movetolast(Transfer *transfer, TransferDbCommitter& committer)
{
    movetransfer(transfer, transfers[transfer->type].end(), committer);
}

void TransferList::movetolast(transfer_list::iterator it, TransferDbCommitter& committer)
{
    Transfer *transfer = (*it);
    movetransfer(it, transfers[transfer->type].end(), committer);
}

void TransferList::moveup(Transfer *transfer, TransferDbCommitter& committer)
{
    transfer_list::iterator it;
    if (getIterator(transfer, it))
    {
        if (it == transfers[transfer->type].begin())
        {
            return;
        }
        transfer_list::iterator dstit = it - 1;
        movetransfer(it, dstit, committer);
    }
}

void TransferList::moveup(transfer_list::iterator it, TransferDbCommitter& committer)
{
    if (it == transfers[it->transfer->type].begin())
    {
        return;
    }

    transfer_list::iterator dstit = it - 1;
    movetransfer(it, dstit, committer);
}

void TransferList::movedown(Transfer *transfer, TransferDbCommitter& committer)
{
    transfer_list::iterator it;
    if (getIterator(transfer, it))
    {

        transfer_list::iterator dstit = it + 1;
        if (dstit == transfers[transfer->type].end())
        {
            return;
        }

        dstit++;
        movetransfer(it, dstit, committer);
    }
}

void TransferList::movedown(transfer_list::iterator it, TransferDbCommitter& committer)
{
    if (it == transfers[it->transfer->type].end())
    {
        return;
    }

    transfer_list::iterator dstit = it + 1;
    movetransfer(it, dstit, committer);
}

error TransferList::pause(Transfer *transfer, bool enable, TransferDbCommitter& committer)
{
    if (!transfer)
    {
        return API_ENOENT;
    }

    if ((enable && transfer->state == TRANSFERSTATE_PAUSED) ||
            (!enable && transfer->state != TRANSFERSTATE_PAUSED))
    {
        return API_OK;
    }

    if (!enable)
    {
        transfer->state = TRANSFERSTATE_QUEUED;

        transfer_list::iterator it;
        if (getIterator(transfer, it))
        {
            prepareIncreasePriority(transfer, it, it, committer);
        }

        client->transfercacheadd(transfer, &committer);
        client->app->transfer_update(transfer);
        return API_OK;
    }

    if (transfer->state == TRANSFERSTATE_ACTIVE
            || transfer->state == TRANSFERSTATE_QUEUED
            || transfer->state == TRANSFERSTATE_RETRYING)
    {
        if (transfer->slot)
        {
            if (transfer->client->ststatus != STORAGE_RED || transfer->type == GET)
            {
                transfer->bt.arm();
            }
            delete transfer->slot;
            transfer->slot = NULL;
        }
        transfer->state = TRANSFERSTATE_PAUSED;
        client->transfercacheadd(transfer, &committer);
        client->app->transfer_update(transfer);
        return API_OK;
    }

    return API_EFAILED;
}

auto TransferList::begin(direction_t direction) -> transfer_list::iterator
{
    return transfers[direction].begin();
}

auto TransferList::end(direction_t direction) -> transfer_list::iterator
{
    return transfers[direction].end();
}

bool TransferList::getIterator(Transfer *transfer, transfer_list::iterator& it, bool canHandleErasedElements)
{
    assert(transfer);
    if (!transfer)
    {
        LOG_err << "Getting iterator of a NULL transfer";
        return false;
    }

    assert(transfer->type == GET || transfer->type == PUT);
    if (transfer->type != GET && transfer->type != PUT)
    {
        LOG_err << "Getting iterator of wrong transfer type " << transfer->type;
        return false;
    }

    it = std::lower_bound(transfers[transfer->type].begin(canHandleErasedElements), transfers[transfer->type].end(canHandleErasedElements), LazyEraseTransferPtr(transfer), priority_comparator);
    if (it != transfers[transfer->type].end(canHandleErasedElements) && it->transfer == transfer)
    {
        return true;
    }
    return false;
}

std::array<vector<Transfer*>, 6> TransferList::nexttransfers(std::function<bool(Transfer*)>& continuefunction,
                                                             std::function<bool(direction_t)>& directionContinuefunction,
                                                             TransferDbCommitter& committer)
{
    std::array<vector<Transfer*>, 6> chosenTransfers;

    static direction_t putget[] = { PUT, GET };

    for (direction_t direction : putget)
    {
        for (Transfer *transfer : transfers[direction])
        {
            if (!transfer->slot)
            {
                // check for cancellation here before we go to the trouble of requesting a download/upload URL
                transfer->removeCancelledTransferFiles(&committer);
                if (transfer->files.empty())
                {
                    transfer->removeAndDeleteSelf(TRANSFERSTATE_CANCELLED);
                    continue;
                }
            }

            // don't traverse the whole list if we already have as many as we are going to get
            if (!directionContinuefunction(direction)) break;

            bool continueLarge = true;
            bool continueSmall = true;

            if ((!transfer->slot && isReady(transfer))
                || (transfer->asyncopencontext
                    && transfer->asyncopencontext->finished))
            {
                TransferCategory tc(transfer);

                if (tc.sizetype == LARGEFILE && continueLarge)
                {
                    continueLarge = continuefunction(transfer);
                    if (continueLarge)
                    {
                        chosenTransfers[tc.index()].push_back(transfer);
                    }
                }
                else if (tc.sizetype == SMALLFILE && continueSmall)
                {
                    continueSmall = continuefunction(transfer);
                    if (continueSmall)
                    {
                        chosenTransfers[tc.index()].push_back(transfer);
                    }
                }
                if (!continueLarge && !continueSmall)
                {
                    break;
                }
            }
        }
    }
    return chosenTransfers;
}

Transfer *TransferList::transferat(direction_t direction, unsigned int position)
{
    if (transfers[direction].size() > position)
    {
        return transfers[direction][position];
    }
    return NULL;
}

void TransferList::prepareIncreasePriority(Transfer *transfer, transfer_list::iterator /*srcit*/, transfer_list::iterator dstit, TransferDbCommitter& committer)
{
    assert(transfer->type == PUT || transfer->type == GET);
    if (dstit == transfers[transfer->type].end())
    {
        return;
    }

    if (!transfer->slot && transfer->state != TRANSFERSTATE_PAUSED)
    {
        Transfer *lastActiveTransfer = NULL;
        for (transferslot_list::iterator it = client->tslots.begin(); it != client->tslots.end(); it++)
        {
            Transfer *t = (*it)->transfer;
            if (t && t->type == transfer->type && t->slot
                    && t->state == TRANSFERSTATE_ACTIVE
                    && t->priority > transfer->priority
                    && (!lastActiveTransfer || t->priority > lastActiveTransfer->priority))
            {
                lastActiveTransfer = t;
            }
        }

        if (lastActiveTransfer)
        {
            if (lastActiveTransfer->client->ststatus != STORAGE_RED || lastActiveTransfer->type == GET)
            {
                lastActiveTransfer->bt.arm();
            }
            delete lastActiveTransfer->slot;
            lastActiveTransfer->slot = NULL;
            lastActiveTransfer->state = TRANSFERSTATE_QUEUED;
            client->transfercacheadd(lastActiveTransfer, &committer);
            client->app->transfer_update(lastActiveTransfer);
        }
    }
}

void TransferList::prepareDecreasePriority(Transfer *transfer, transfer_list::iterator it, transfer_list::iterator dstit)
{
    assert(transfer->type == PUT || transfer->type == GET);
    if (transfer->slot && transfer->state == TRANSFERSTATE_ACTIVE)
    {
        transfer_list::iterator cit = it + 1;
        while (cit != transfers[transfer->type].end())
        {
            if (!cit->transfer->slot && isReady(*cit))
            {
                if (transfer->client->ststatus != STORAGE_RED || transfer->type == GET)
                {
                    transfer->bt.arm();
                }
                delete transfer->slot;
                transfer->slot = NULL;
                transfer->state = TRANSFERSTATE_QUEUED;
                break;
            }

            if (cit == dstit)
            {
                break;
            }

            cit++;
        }
    }
}

bool TransferList::isReady(Transfer *transfer)
{
    return ((transfer->state == TRANSFERSTATE_QUEUED || transfer->state == TRANSFERSTATE_RETRYING)
            && transfer->bt.armed());
}

} // namespace
