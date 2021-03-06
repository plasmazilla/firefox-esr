/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim:set ts=2 sw=2 sts=2 et cindent: */
/* ***** BEGIN LICENSE BLOCK *****
 * Version: MPL 1.1/GPL 2.0/LGPL 2.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is Mozilla code.
 *
 * The Initial Developer of the Original Code is the Mozilla Corporation.
 * Portions created by the Initial Developer are Copyright (C) 2007
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 *  Chris Double <chris.double@double.co.nz>
 *
 * Alternatively, the contents of this file may be used under the terms of
 * either the GNU General Public License Version 2 or later (the "GPL"), or
 * the GNU Lesser General Public License Version 2.1 or later (the "LGPL"),
 * in which case the provisions of the GPL or the LGPL are applicable instead
 * of those above. If you wish to allow use of your version of this file only
 * under the terms of either the GPL or the LGPL, and not to allow others to
 * use your version of this file under the terms of the MPL, indicate your
 * decision by deleting the provisions above and replace them with the notice
 * and other provisions required by the GPL or the LGPL. If you do not delete
 * the provisions above, a recipient may use your version of this file under
 * the terms of any one of the MPL, the GPL or the LGPL.
 *
 * ***** END LICENSE BLOCK ***** */
#include "nsDebug.h"
#include "nsMediaStream.h"
#include "nsMediaDecoder.h"
#include "nsNetUtil.h"
#include "nsAutoLock.h"
#include "nsThreadUtils.h"
#include "nsIFile.h"
#include "nsIFileChannel.h"
#include "nsIHttpChannel.h"
#include "nsISeekableStream.h"
#include "nsIInputStream.h"
#include "nsIOutputStream.h"
#include "nsIRequestObserver.h"
#include "nsIStreamListener.h"
#include "nsIScriptSecurityManager.h"
#include "nsCrossSiteListenerProxy.h"
#include "nsHTMLMediaElement.h"
#include "nsIDocument.h"
#include "nsDOMError.h"
#include "nsICachingChannel.h"
#include "nsURILoader.h"
#include "ImageErrors.h"

using mozilla::TimeStamp;

#define HTTP_OK_CODE 200
#define HTTP_PARTIAL_RESPONSE_CODE 206

nsMediaChannelStream::nsMediaChannelStream(nsMediaDecoder* aDecoder,
    nsIChannel* aChannel, nsIURI* aURI)
  : nsMediaStream(aDecoder, aChannel, aURI),
    mOffset(0), mSuspendCount(0),
    mReopenOnError(PR_FALSE), mIgnoreClose(PR_FALSE),
    mCacheStream(this),
    mLock(nsAutoLock::NewLock("media.channel.stream")),
    mCacheSuspendCount(0)
{
}

nsMediaChannelStream::~nsMediaChannelStream()
{
  if (mListener) {
    // Kill its reference to us since we're going away
    mListener->Revoke();
  }
  if (mLock) {
    nsAutoLock::DestroyLock(mLock);
  }
}

// nsMediaChannelStream::Listener just observes the channel and
// forwards notifications to the nsMediaChannelStream. We use multiple
// listener objects so that when we open a new stream for a seek we can
// disconnect the old listener from the nsMediaChannelStream and hook up
// a new listener, so notifications from the old channel are discarded
// and don't confuse us.
NS_IMPL_ISUPPORTS4(nsMediaChannelStream::Listener,
                   nsIRequestObserver, nsIStreamListener, nsIChannelEventSink,
                   nsIInterfaceRequestor)

nsresult
nsMediaChannelStream::Listener::OnStartRequest(nsIRequest* aRequest,
                                               nsISupports* aContext)
{
  if (!mStream)
    return NS_OK;
  return mStream->OnStartRequest(aRequest);
}

nsresult
nsMediaChannelStream::Listener::OnStopRequest(nsIRequest* aRequest,
                                              nsISupports* aContext,
                                              nsresult aStatus)
{
  if (!mStream)
    return NS_OK;
  return mStream->OnStopRequest(aRequest, aStatus);
}

nsresult
nsMediaChannelStream::Listener::OnDataAvailable(nsIRequest* aRequest, 
                                                nsISupports* aContext, 
                                                nsIInputStream* aStream,
                                                PRUint32 aOffset,
                                                PRUint32 aCount)
{
  if (!mStream)
    return NS_OK;
  return mStream->OnDataAvailable(aRequest, aStream, aCount);
}

nsresult
nsMediaChannelStream::Listener::OnChannelRedirect(nsIChannel* aOldChannel,
                                                  nsIChannel* aNewChannel,
                                                  PRUint32 aFlags)
{
  if (!mStream)
    return NS_OK;
  return mStream->OnChannelRedirect(aOldChannel, aNewChannel, aFlags);
}

nsresult
nsMediaChannelStream::Listener::GetInterface(const nsIID & aIID, void **aResult)
{
  return QueryInterface(aIID, aResult);
}

nsresult
nsMediaChannelStream::OnStartRequest(nsIRequest* aRequest)
{
  NS_ASSERTION(mChannel.get() == aRequest, "Wrong channel!");

  nsHTMLMediaElement* element = mDecoder->GetMediaElement();
  NS_ENSURE_TRUE(element, NS_ERROR_FAILURE);
  if (element->ShouldCheckAllowOrigin()) {
    // If the request was cancelled by nsCrossSiteListenerProxy due to failing
    // the Access Control check, send an error through to the media element.
    nsresult status;
    nsresult rv = aRequest->GetStatus(&status);
    if (NS_FAILED(rv) || status == NS_ERROR_DOM_BAD_URI) {
      mDecoder->NetworkError();
      return NS_ERROR_DOM_BAD_URI;
    }
  }

  nsCOMPtr<nsIHttpChannel> hc = do_QueryInterface(aRequest);
  PRBool seekable = PR_FALSE;
  if (hc) {
    nsCAutoString ranges;
    hc->GetResponseHeader(NS_LITERAL_CSTRING("Accept-Ranges"),
                          ranges);
    PRBool acceptsRanges = ranges.EqualsLiteral("bytes"); 

    if (mOffset == 0) {
      // Look for duration headers from known Ogg content systems. In the case
      // of multiple options for obtaining the duration the order of precedence is;
      // 1) The Media resource metadata if possible (done by the decoder itself).
      // 2) X-Content-Duration.
      // 3) x-amz-meta-content-duration.
      // 4) Perform a seek in the decoder to find the value.
      nsCAutoString durationText;
      PRInt32 ec = 0;
      nsresult rv = hc->GetResponseHeader(NS_LITERAL_CSTRING("X-Content-Duration"), durationText);
      if (NS_FAILED(rv)) {
        rv = hc->GetResponseHeader(NS_LITERAL_CSTRING("X-AMZ-Meta-Content-Duration"), durationText);
      }

      if (NS_SUCCEEDED(rv)) {
        float duration = durationText.ToFloat(&ec);
        if (ec == NS_OK && duration >= 0) {
          mDecoder->SetDuration(PRInt64(NS_round(duration*1000)));
        }
      }
    }
 
    PRUint32 responseStatus = 0; 
    hc->GetResponseStatus(&responseStatus);
    if (mOffset > 0 && responseStatus == HTTP_OK_CODE) {
      // If we get an OK response but we were seeking, we have to assume
      // that seeking doesn't work. We also need to tell the cache that
      // it's getting data for the start of the stream.
      mCacheStream.NotifyDataStarted(0);
      mOffset = 0;
    } else if (mOffset == 0 && 
               (responseStatus == HTTP_OK_CODE ||
                responseStatus == HTTP_PARTIAL_RESPONSE_CODE)) {
      // We weren't seeking and got a valid response status,
      // set the length of the content.
      PRInt32 cl = -1;
      hc->GetContentLength(&cl);
      if (cl >= 0) {
        mCacheStream.NotifyDataLength(cl);
      }
    }
    // XXX we probably should examine the Content-Range header in case
    // the server gave us a range which is not quite what we asked for

    // If we get an HTTP_OK_CODE response to our byte range request,
    // and the server isn't sending Accept-Ranges:bytes then we don't
    // support seeking.
    seekable =
      responseStatus == HTTP_PARTIAL_RESPONSE_CODE || acceptsRanges;
  }
  mDecoder->SetSeekable(seekable);
  mCacheStream.SetSeekable(seekable);

  nsCOMPtr<nsICachingChannel> cc = do_QueryInterface(aRequest);
  if (cc) {
    PRBool fromCache = PR_FALSE;
    nsresult rv = cc->IsFromCache(&fromCache);
    if (NS_SUCCEEDED(rv) && !fromCache) {
      cc->SetCacheAsFile(PR_TRUE);
    }
  }

  {
    nsAutoLock lock(mLock);
    mChannelStatistics.Start(TimeStamp::Now());
  }

  mReopenOnError = PR_FALSE;
  mIgnoreClose = PR_FALSE;
  if (mSuspendCount > 0) {
    // Re-suspend the channel if it needs to be suspended
    mChannel->Suspend();
  }

  // Fires an initial progress event and sets up the stall counter so stall events
  // fire if no download occurs within the required time frame.
  mDecoder->Progress(PR_FALSE);

  return NS_OK;
}

nsresult
nsMediaChannelStream::OnStopRequest(nsIRequest* aRequest, nsresult aStatus)
{
  NS_ASSERTION(mChannel.get() == aRequest, "Wrong channel!");
  NS_ASSERTION(mSuspendCount == 0,
               "How can OnStopRequest fire while we're suspended?");

  {
    nsAutoLock lock(mLock);
    mChannelStatistics.Stop(TimeStamp::Now());
  }

  if (NS_FAILED(aStatus) && aStatus != NS_IMAGELIB_ERROR_LOAD_ABORTED &&
      mReopenOnError) {
    nsresult rv = CacheClientSeek(mOffset, PR_FALSE);
    if (NS_SUCCEEDED(rv))
      return rv;
    // If the reopen/reseek fails, just fall through and treat this
    // error as fatal.
  }

  if (!mIgnoreClose) {
    mCacheStream.NotifyDataEnded(aStatus);
    if (mDecoder) {
      mDecoder->NotifyDownloadEnded(aStatus);
    }
  }

  return NS_OK;
}

nsresult
nsMediaChannelStream::OnChannelRedirect(nsIChannel* aOld, nsIChannel* aNew,
                                        PRUint32 aFlags)
{
  mChannel = aNew;
  SetupChannelHeaders();
  return NS_OK;
}

struct CopySegmentClosure {
  nsCOMPtr<nsIPrincipal> mPrincipal;
  nsMediaChannelStream*  mStream;
};

NS_METHOD
nsMediaChannelStream::CopySegmentToCache(nsIInputStream *aInStream,
                                         void *aClosure,
                                         const char *aFromSegment,
                                         PRUint32 aToOffset,
                                         PRUint32 aCount,
                                         PRUint32 *aWriteCount)
{
  CopySegmentClosure* closure = static_cast<CopySegmentClosure*>(aClosure);
  // Keep track of where we're up to
  closure->mStream->mOffset += aCount;
  closure->mStream->mCacheStream.NotifyDataReceived(aCount, aFromSegment,
                                                    closure->mPrincipal);
  *aWriteCount = aCount;
  return NS_OK;
}

nsresult
nsMediaChannelStream::OnDataAvailable(nsIRequest* aRequest,
                                      nsIInputStream* aStream,
                                      PRUint32 aCount)
{
  NS_ASSERTION(mChannel.get() == aRequest, "Wrong channel!");

  {
    nsAutoLock lock(mLock);
    mChannelStatistics.AddBytes(aCount);
  }

  CopySegmentClosure closure;
  nsIScriptSecurityManager* secMan = nsContentUtils::GetSecurityManager();
  if (secMan && mChannel) {
    secMan->GetChannelPrincipal(mChannel, getter_AddRefs(closure.mPrincipal));
  }
  closure.mStream = this;

  PRUint32 count = aCount;
  while (count > 0) {
    PRUint32 read;
    nsresult rv = aStream->ReadSegments(CopySegmentToCache, &closure, count, 
                                        &read);
    if (NS_FAILED(rv))
      return rv;
    NS_ASSERTION(read > 0, "Read 0 bytes while data was available?");
    count -= read;
  }
  mDecoder->NotifyBytesDownloaded();

  // Fire a progress events according to the time and byte constraints outlined
  // in the spec.
  mDecoder->Progress(PR_FALSE);
  return NS_OK;
}

nsresult nsMediaChannelStream::Open(nsIStreamListener **aStreamListener)
{
  NS_ASSERTION(NS_IsMainThread(), "Only call on main thread");

  if (!mLock)
    return NS_ERROR_OUT_OF_MEMORY;
  nsresult rv = mCacheStream.Init();
  if (NS_FAILED(rv))
    return rv;
  NS_ASSERTION(mOffset == 0, "Who set mOffset already?");
  return OpenChannel(aStreamListener);
}

nsresult nsMediaChannelStream::OpenChannel(nsIStreamListener** aStreamListener)
{
  NS_ASSERTION(NS_IsMainThread(), "Only call on main thread");
  NS_ENSURE_TRUE(mChannel, NS_ERROR_NULL_POINTER);
  NS_ASSERTION(!mListener, "Listener should have been removed by now");

  if (aStreamListener) {
    *aStreamListener = nsnull;
  }

  mListener = new Listener(this);
  NS_ENSURE_TRUE(mListener, NS_ERROR_OUT_OF_MEMORY);

  if (aStreamListener) {
    *aStreamListener = mListener;
    NS_ADDREF(*aStreamListener);
  } else {
    mChannel->SetNotificationCallbacks(mListener.get());

    nsCOMPtr<nsIStreamListener> listener = mListener.get();

    // Ensure that if we're loading cross domain, that the server is sending
    // an authorizing Access-Control header.
    nsHTMLMediaElement* element = mDecoder->GetMediaElement();
    NS_ENSURE_TRUE(element, NS_ERROR_FAILURE);
    if (element->ShouldCheckAllowOrigin()) {
      nsresult rv;
      listener = new nsCrossSiteListenerProxy(mListener,
                                              element->NodePrincipal(),
                                              mChannel,
                                              PR_FALSE,
                                              &rv);
      NS_ENSURE_TRUE(listener, NS_ERROR_OUT_OF_MEMORY);
      NS_ENSURE_SUCCESS(rv, rv);
    } else {
      nsresult rv = nsContentUtils::GetSecurityManager()->
        CheckLoadURIWithPrincipal(element->NodePrincipal(),
                                  mURI,
                                  nsIScriptSecurityManager::STANDARD);
      NS_ENSURE_SUCCESS(rv, rv);
    }

    SetupChannelHeaders();
 
    nsresult rv = mChannel->AsyncOpen(listener, nsnull);
    NS_ENSURE_SUCCESS(rv, rv);
  }

  return NS_OK;
}

void nsMediaChannelStream::SetupChannelHeaders()
{
  // Always use a byte range request even if we're reading from the start
  // of the resource.
  // This enables us to detect if the stream supports byte range
  // requests, and therefore seeking, early.
  nsCOMPtr<nsIHttpChannel> hc = do_QueryInterface(mChannel);
  if (hc) {
    nsCAutoString rangeString("bytes=");
    rangeString.AppendInt(mOffset);
    rangeString.Append("-");
    hc->SetRequestHeader(NS_LITERAL_CSTRING("Range"), rangeString, PR_FALSE);
  } else {
    NS_ASSERTION(mOffset == 0, "Don't know how to seek on this channel type");
  }
}

nsresult nsMediaChannelStream::Close()
{
  NS_ASSERTION(NS_IsMainThread(), "Only call on main thread");

  mCacheStream.Close();
  CloseChannel();
  return NS_OK;
}

already_AddRefed<nsIPrincipal> nsMediaChannelStream::GetCurrentPrincipal()
{
  NS_ASSERTION(NS_IsMainThread(), "Only call on main thread");

  nsCOMPtr<nsIPrincipal> principal = mCacheStream.GetCurrentPrincipal();
  return principal.forget();
}

void nsMediaChannelStream::CloseChannel()
{
  NS_ASSERTION(NS_IsMainThread(), "Only call on main thread");

  {
    nsAutoLock lock(mLock);
    mChannelStatistics.Stop(TimeStamp::Now());
  }

  if (mListener) {
    mListener->Revoke();
    mListener = nsnull;
  }

  if (mChannel) {
    if (mSuspendCount > 0) {
      // Resume the channel before we cancel it
      mChannel->Resume();
    }
    // The status we use here won't be passed to the decoder, since
    // we've already revoked the listener. It can however be passed
    // to DocumentViewerImpl::LoadComplete if our channel is the one
    // that kicked off creation of a video document. We don't want that
    // document load to think there was an error.
    // NS_IMAGELIB_ERROR_LOAD_ABORTED is the best thing we have for that
    // at the moment.
    mChannel->Cancel(NS_IMAGELIB_ERROR_LOAD_ABORTED);
    mChannel = nsnull;
  }
}

nsresult nsMediaChannelStream::Read(char* aBuffer, PRUint32 aCount, PRUint32* aBytes)
{
  NS_ASSERTION(!NS_IsMainThread(), "Don't call on main thread");

  return mCacheStream.Read(aBuffer, aCount, aBytes);
}

nsresult nsMediaChannelStream::Seek(PRInt32 aWhence, PRInt64 aOffset) 
{
  NS_ASSERTION(!NS_IsMainThread(), "Don't call on main thread");

  return mCacheStream.Seek(aWhence, aOffset);
}

PRInt64 nsMediaChannelStream::Tell()
{
  NS_ASSERTION(!NS_IsMainThread(), "Don't call on main thread");

  return mCacheStream.Tell();
}

void nsMediaChannelStream::Suspend(PRBool aCloseImmediately)
{
  NS_ASSERTION(NS_IsMainThread(), "Don't call on main thread");

  nsHTMLMediaElement* element = mDecoder->GetMediaElement();
  if (!element) {
    // Shutting down; do nothing.
    return;
  }

  if (mChannel) {
    if (aCloseImmediately && mCacheStream.IsSeekable()) {
      // Kill off our channel right now, but don't tell anyone about it.
      mIgnoreClose = PR_TRUE;
      CloseChannel();
      element->DownloadSuspended();
    } else if (mSuspendCount == 0) {
      {
        nsAutoLock lock(mLock);
        mChannelStatistics.Stop(TimeStamp::Now());
      }
      mChannel->Suspend();
      element->DownloadSuspended();
    }
  }

  ++mSuspendCount;
}

void nsMediaChannelStream::Resume()
{
  NS_ASSERTION(NS_IsMainThread(), "Don't call on main thread");
  NS_ASSERTION(mSuspendCount > 0, "Too many resumes!");

  nsHTMLMediaElement* element = mDecoder->GetMediaElement();
  if (!element) {
    // Shutting down; do nothing.
    return;
  }

  --mSuspendCount;
  if (mSuspendCount == 0) {
    if (mChannel) {
      // Just wake up our existing channel
      {
        nsAutoLock lock(mLock);
        mChannelStatistics.Start(TimeStamp::Now());
      }
      // if an error occurs after Resume, assume it's because the server
      // timed out the connection and we should reopen it.
      mReopenOnError = PR_TRUE;
      mChannel->Resume();
      element->DownloadResumed();
    } else {
      // Need to recreate the channel
      CacheClientSeek(mOffset, PR_FALSE);
      element->DownloadResumed();
    }
  }
}

nsresult
nsMediaChannelStream::CacheClientSeek(PRInt64 aOffset, PRBool aResume)
{
  NS_ASSERTION(NS_IsMainThread(), "Don't call on main thread");

  nsLoadFlags loadFlags =
    nsICachingChannel::LOAD_BYPASS_LOCAL_CACHE_IF_BUSY |
    (mLoadInBackground ? nsIRequest::LOAD_BACKGROUND : 0);

  CloseChannel();

  nsHTMLMediaElement* element = mDecoder->GetMediaElement();
  if (!element) {
    // The decoder is being shut down, so don't bother opening a new channel
    return NS_OK;
  }
  nsCOMPtr<nsILoadGroup> loadGroup = element->GetDocumentLoadGroup();
  NS_ENSURE_TRUE(loadGroup, NS_ERROR_NULL_POINTER);

  if (aResume) {
    NS_ASSERTION(mSuspendCount > 0, "Too many resumes!");
    // No need to mess with the channel, since we're making a new one
    --mSuspendCount;
  }

  nsresult rv =
    NS_NewChannel(getter_AddRefs(mChannel),
                  mURI,
                  nsnull,
                  loadGroup,
                  nsnull,
                  loadFlags);
  if (NS_FAILED(rv))
    return rv;
  mOffset = aOffset;
  return OpenChannel(nsnull);
}

class SuspendedStatusChanged : public nsRunnable 
{
public:
  SuspendedStatusChanged(nsMediaDecoder* aDecoder) :
    mDecoder(aDecoder)
  {
    MOZ_COUNT_CTOR(SuspendedStatusChanged);
  }
  ~SuspendedStatusChanged()
  {
    MOZ_COUNT_DTOR(SuspendedStatusChanged);
  }

  NS_IMETHOD Run() {
    mDecoder->NotifySuspendedStatusChanged();
    return NS_OK;
  }

private:
  nsRefPtr<nsMediaDecoder> mDecoder;
};

nsresult
nsMediaChannelStream::CacheClientSuspend()
{
  {
    nsAutoLock lock(mLock);
    ++mCacheSuspendCount;
  }
  Suspend(PR_FALSE);

  // We have to spawn an event here since we're being called back from
  // a sensitive place in nsMediaCache, which doesn't want us to reenter
  // the decoder and cause deadlocks or other unpleasantness
  nsCOMPtr<nsIRunnable> event = new SuspendedStatusChanged(mDecoder);
  NS_DispatchToMainThread(event, NS_DISPATCH_NORMAL);
  return NS_OK;
}

nsresult
nsMediaChannelStream::CacheClientResume()
{
  Resume();
  {
    nsAutoLock lock(mLock);
    --mCacheSuspendCount;
  }

  // We have to spawn an event here since we're being called back from
  // a sensitive place in nsMediaCache, which doesn't want us to reenter
  // the decoder and cause deadlocks or other unpleasantness
  nsCOMPtr<nsIRunnable> event = new SuspendedStatusChanged(mDecoder);
  NS_DispatchToMainThread(event, NS_DISPATCH_NORMAL);
  return NS_OK;
}

PRInt64
nsMediaChannelStream::GetNextCachedData(PRInt64 aOffset)
{
  return mCacheStream.GetNextCachedData(aOffset);
}

PRInt64
nsMediaChannelStream::GetCachedDataEnd(PRInt64 aOffset)
{
  return mCacheStream.GetCachedDataEnd(aOffset);
}

PRBool
nsMediaChannelStream::IsDataCachedToEndOfStream(PRInt64 aOffset)
{
  return mCacheStream.IsDataCachedToEndOfStream(aOffset);
}

PRBool
nsMediaChannelStream::IsSuspendedByCache()
{
  nsAutoLock lock(mLock);
  return mCacheSuspendCount > 0;
}

void
nsMediaChannelStream::SetReadMode(nsMediaCacheStream::ReadMode aMode)
{
  mCacheStream.SetReadMode(aMode);
}

void
nsMediaChannelStream::SetPlaybackRate(PRUint32 aBytesPerSecond)
{
  mCacheStream.SetPlaybackRate(aBytesPerSecond);
}

void
nsMediaChannelStream::Pin()
{
  mCacheStream.Pin();
}

void
nsMediaChannelStream::Unpin()
{
  mCacheStream.Unpin();
}

double
nsMediaChannelStream::GetDownloadRate(PRPackedBool* aIsReliable)
{
  nsAutoLock lock(mLock);
  return mChannelStatistics.GetRate(TimeStamp::Now(), aIsReliable);
}

PRInt64
nsMediaChannelStream::GetLength()
{
  return mCacheStream.GetLength();
}

class nsMediaFileStream : public nsMediaStream
{
public:
  nsMediaFileStream(nsMediaDecoder* aDecoder, nsIChannel* aChannel, nsIURI* aURI) :
    nsMediaStream(aDecoder, aChannel, aURI), mSize(-1),
    mLock(nsAutoLock::NewLock("media.file.stream"))
  {
  }
  ~nsMediaFileStream()
  {
    if (mLock) {
      nsAutoLock::DestroyLock(mLock);
    }
  }

  // Main thread
  virtual nsresult Open(nsIStreamListener** aStreamListener);
  virtual nsresult Close();
  virtual void     Suspend(PRBool aCloseImmediately) {}
  virtual void     Resume() {}
  virtual already_AddRefed<nsIPrincipal> GetCurrentPrincipal();

  // These methods are called off the main thread.

  // Other thread
  virtual void     SetReadMode(nsMediaCacheStream::ReadMode aMode) {}
  virtual void     SetPlaybackRate(PRUint32 aBytesPerSecond) {}
  virtual nsresult Read(char* aBuffer, PRUint32 aCount, PRUint32* aBytes);
  virtual nsresult Seek(PRInt32 aWhence, PRInt64 aOffset);
  virtual PRInt64  Tell();

  // Any thread
  virtual void    Pin() {}
  virtual void    Unpin() {}
  virtual double  GetDownloadRate(PRPackedBool* aIsReliable)
  {
    // The data's all already here
    *aIsReliable = PR_TRUE;
    return 100*1024*1024; // arbitray, use 100MB/s
  }
  virtual PRInt64 GetLength() { return mSize; }
  virtual PRInt64 GetNextCachedData(PRInt64 aOffset)
  {
    return (aOffset < mSize) ? aOffset : -1;
  }
  virtual PRInt64 GetCachedDataEnd(PRInt64 aOffset) { return PR_MAX(aOffset, mSize); }
  virtual PRBool  IsDataCachedToEndOfStream(PRInt64 aOffset) { return PR_TRUE; }
  virtual PRBool  IsSuspendedByCache() { return PR_FALSE; }

private:
  // The file size, or -1 if not known. Immutable after Open().
  PRInt64 mSize;

  // This lock handles synchronisation between calls to Close() and
  // the Read, Seek, etc calls. Close must not be called while a
  // Read or Seek is in progress since it resets various internal
  // values to null.
  // This lock protects mSeekable and mInput.
  PRLock* mLock;

  // Seekable stream interface to file. This can be used from any
  // thread.
  nsCOMPtr<nsISeekableStream> mSeekable;

  // Input stream for the media data. This can be used from any
  // thread.
  nsCOMPtr<nsIInputStream>  mInput;
};

class LoadedEvent : public nsRunnable 
{
public:
  LoadedEvent(nsMediaDecoder* aDecoder) :
    mDecoder(aDecoder)
  {
    MOZ_COUNT_CTOR(LoadedEvent);
  }
  ~LoadedEvent()
  {
    MOZ_COUNT_DTOR(LoadedEvent);
  }

  NS_IMETHOD Run() {
    mDecoder->NotifyDownloadEnded(NS_OK);
    return NS_OK;
  }

private:
  nsRefPtr<nsMediaDecoder> mDecoder;
};

nsresult nsMediaFileStream::Open(nsIStreamListener** aStreamListener)
{
  NS_ASSERTION(NS_IsMainThread(), "Only call on main thread");

  if (aStreamListener) {
    *aStreamListener = nsnull;
  }

  nsresult rv;
  if (aStreamListener) {
    // The channel is already open. We need a synchronous stream that
    // implements nsISeekableStream, so we have to find the underlying
    // file and reopen it
    nsCOMPtr<nsIFileChannel> fc(do_QueryInterface(mChannel));
    if (!fc)
      return NS_ERROR_UNEXPECTED;

    nsCOMPtr<nsIFile> file; 
    rv = fc->GetFile(getter_AddRefs(file));
    NS_ENSURE_SUCCESS(rv, rv);

    rv = NS_NewLocalFileInputStream(getter_AddRefs(mInput), file);
  } else {
    // Ensure that we never load a local file from some page on a 
    // web server.
    nsHTMLMediaElement* element = mDecoder->GetMediaElement();
    NS_ENSURE_TRUE(element, NS_ERROR_FAILURE);

    rv = nsContentUtils::GetSecurityManager()->
           CheckLoadURIWithPrincipal(element->NodePrincipal(),
                                     mURI,
                                     nsIScriptSecurityManager::STANDARD);
    NS_ENSURE_SUCCESS(rv, rv);

    rv = mChannel->Open(getter_AddRefs(mInput));
  }
  NS_ENSURE_SUCCESS(rv, rv);

  mSeekable = do_QueryInterface(mInput);
  if (!mSeekable) {
    // XXX The file may just be a .url or similar
    // shortcut that points to a Web site. We need to fix this by
    // doing an async open and waiting until we locate the real resource,
    // then using that (if it's still a file!).
    return NS_ERROR_FAILURE;
  }

  // Get the file size and inform the decoder. Only files up to 4GB are
  // supported here.
  PRUint32 size;
  rv = mInput->Available(&size);
  if (NS_SUCCEEDED(rv)) {
    mSize = size;
  }

  nsCOMPtr<nsIRunnable> event = new LoadedEvent(mDecoder);
  NS_DispatchToMainThread(event, NS_DISPATCH_NORMAL);
  return NS_OK;
}

nsresult nsMediaFileStream::Close()
{
  NS_ASSERTION(NS_IsMainThread(), "Only call on main thread");

  nsAutoLock lock(mLock);
  if (mChannel) {
    mChannel->Cancel(NS_IMAGELIB_ERROR_LOAD_ABORTED);
    mChannel = nsnull;
    mInput = nsnull;
    mSeekable = nsnull;
  }

  return NS_OK;
}

already_AddRefed<nsIPrincipal> nsMediaFileStream::GetCurrentPrincipal()
{
  NS_ASSERTION(NS_IsMainThread(), "Only call on main thread");

  nsCOMPtr<nsIPrincipal> principal;
  nsIScriptSecurityManager* secMan = nsContentUtils::GetSecurityManager();
  if (!secMan || !mChannel)
    return nsnull;
  secMan->GetChannelPrincipal(mChannel, getter_AddRefs(principal));
  return principal.forget();
}

nsresult nsMediaFileStream::Read(char* aBuffer, PRUint32 aCount, PRUint32* aBytes)
{
  nsAutoLock lock(mLock);
  if (!mInput)
    return NS_ERROR_FAILURE;
  return mInput->Read(aBuffer, aCount, aBytes);
}

nsresult nsMediaFileStream::Seek(PRInt32 aWhence, PRInt64 aOffset) 
{
  NS_ASSERTION(!NS_IsMainThread(), "Don't call on main thread");

  nsAutoLock lock(mLock);
  if (!mSeekable)
    return NS_ERROR_FAILURE;
  return mSeekable->Seek(aWhence, aOffset);
}

PRInt64 nsMediaFileStream::Tell()
{
  NS_ASSERTION(!NS_IsMainThread(), "Don't call on main thread");

  nsAutoLock lock(mLock);
  if (!mSeekable)
    return 0;

  PRInt64 offset = 0;
  mSeekable->Tell(&offset);
  return offset;
}

nsresult
nsMediaStream::Open(nsMediaDecoder* aDecoder, nsIURI* aURI,
                    nsIChannel* aChannel, nsMediaStream** aStream,
                    nsIStreamListener** aListener)
{
  NS_ASSERTION(NS_IsMainThread(), 
	             "nsMediaStream::Open called on non-main thread");

  *aStream = nsnull;

  nsCOMPtr<nsIChannel> channel;
  if (aChannel) {
    channel = aChannel;
  } else {
    nsHTMLMediaElement* element = aDecoder->GetMediaElement();
    NS_ENSURE_TRUE(element, NS_ERROR_NULL_POINTER);
    nsCOMPtr<nsILoadGroup> loadGroup = element->GetDocumentLoadGroup();
    nsresult rv = NS_NewChannel(getter_AddRefs(channel), 
                                aURI, 
                                nsnull,
                                loadGroup,
                                nsnull,
                                nsICachingChannel::LOAD_BYPASS_LOCAL_CACHE_IF_BUSY);
    NS_ENSURE_SUCCESS(rv, rv);
  }

  nsMediaStream* stream;
  nsCOMPtr<nsIFileChannel> fc = do_QueryInterface(channel);
  if (fc) {
    stream = new nsMediaFileStream(aDecoder, channel, aURI);
  } else {
    stream = new nsMediaChannelStream(aDecoder, channel, aURI);
  }
  if (!stream)
    return NS_ERROR_OUT_OF_MEMORY;

  nsresult rv = stream->Open(aListener);
  NS_ENSURE_SUCCESS(rv, rv);

  *aStream = stream;
  return NS_OK;
}

void nsMediaStream::MoveLoadsToBackground() {
  NS_ASSERTION(!mLoadInBackground, "Why are you calling this more than once?");
  mLoadInBackground = PR_TRUE;
  if (!mChannel) {
    // No channel, resource is probably already loaded.
    return;
  }

  nsresult rv;
  nsHTMLMediaElement* element = mDecoder->GetMediaElement();
  if (!element) {
    NS_WARNING("Null element in nsMediaStream::MoveLoadsToBackground()");
    return;
  }
  nsCOMPtr<nsILoadGroup> loadGroup;
  rv = mChannel->GetLoadGroup(getter_AddRefs(loadGroup));
  NS_ASSERTION(NS_SUCCEEDED(rv), "GetLoadGroup() failed!");
  nsresult status;
  mChannel->GetStatus(&status);
  // Note: if (NS_FAILED(status)), the channel won't be in the load group.
  PRBool isPending = PR_FALSE;
  if (loadGroup &&
      NS_SUCCEEDED(status) &&
      NS_SUCCEEDED(mChannel->IsPending(&isPending)) &&
      isPending) {
    rv = loadGroup->RemoveRequest(mChannel, nsnull, status);
    NS_ASSERTION(NS_SUCCEEDED(rv), "RemoveRequest() failed!");

    nsLoadFlags loadFlags;
    rv = mChannel->GetLoadFlags(&loadFlags);
    NS_ASSERTION(NS_SUCCEEDED(rv), "GetLoadFlags() failed!");

    loadFlags |= nsIRequest::LOAD_BACKGROUND;
    rv = mChannel->SetLoadFlags(loadFlags);
    NS_ASSERTION(NS_SUCCEEDED(rv), "SetLoadFlags() failed!");

    rv = loadGroup->AddRequest(mChannel, nsnull);
    NS_ASSERTION(NS_SUCCEEDED(rv), "AddRequest() failed!");
  }
}
