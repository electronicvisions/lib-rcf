
//******************************************************************************
// RCF - Remote Call Framework
//
// Copyright (c) 2005 - 2019, Delta V Software. All rights reserved.
// http://www.deltavsoft.com
//
// RCF is distributed under dual licenses - closed source or GPL.
// Consult your particular license for conditions of use.
//
// If you have not purchased a commercial license, you are using RCF 
// under GPL terms.
//
// Version: 3.1
// Contact: support <at> deltavsoft.com 
//
//******************************************************************************

#include <RCF/Schannel.hpp>

#include <RCF/ClientStub.hpp>
#include <RCF/Exception.hpp>
#include <RCF/RcfServer.hpp>
#include <RCF/SspiFilter.hpp>
#include <RCF/ThreadLocalData.hpp>
#include <RCF/Tools.hpp>
#include <RCF/Win32Certificate.hpp>
#include <RCF/Log.hpp>

#include <wincrypt.h>
#include <schnlsp.h>

namespace RCF {

    PSecurityFunctionTable getSft();

    void SspiFilter::encryptWriteBufferSchannel()
    {
        // encrypt the pre buffer to the write buffer

        RCF_ASSERT(mContextState == AuthOkAck);

        SecPkgContext_Sizes sizes;
        getSft()->QueryContextAttributes(
            &mContext,
            SECPKG_ATTR_SIZES,
            &sizes);

        SecPkgContext_StreamSizes streamSizes;
        getSft()->QueryContextAttributes(
            &mContext,
            SECPKG_ATTR_STREAM_SIZES,
            &streamSizes);

        DWORD cbHeader          = streamSizes.cbHeader;
        DWORD cbMsg             = static_cast<DWORD>(mWriteByteBufferOrig.getLength());
        DWORD cbTrailer         = streamSizes.cbTrailer;
        DWORD cbPacket          = cbHeader + cbMsg + cbTrailer;

        resizeWriteBuffer(cbPacket);
        
        memcpy(
            mWriteBuffer+cbHeader,
            mWriteByteBufferOrig.getPtr(),
            mWriteByteBufferOrig.getLength());

        BYTE *pEncryptedMsg     =((BYTE *) mWriteBuffer);

        SecBuffer rgsb[4]       = {0};
        rgsb[0].cbBuffer        = cbHeader;
        rgsb[0].BufferType      = SECBUFFER_STREAM_HEADER;
        rgsb[0].pvBuffer        = pEncryptedMsg;

        rgsb[1].cbBuffer        = cbMsg;
        rgsb[1].BufferType      = SECBUFFER_DATA;
        rgsb[1].pvBuffer        = pEncryptedMsg + cbHeader;

        rgsb[2].cbBuffer        = cbTrailer;
        rgsb[2].BufferType      = SECBUFFER_STREAM_TRAILER;
        rgsb[2].pvBuffer        = pEncryptedMsg + cbHeader + cbMsg;

        rgsb[3].cbBuffer        = 0;
        rgsb[3].BufferType      = SECBUFFER_EMPTY;
        rgsb[3].pvBuffer        = NULL;

        SecBufferDesc sbd       = {0};
        sbd.ulVersion           = SECBUFFER_VERSION;
        sbd.cBuffers            = sizeof(rgsb)/sizeof(*rgsb);
        sbd.pBuffers            = rgsb;

        SECURITY_STATUS status = getSft()->EncryptMessage(
            &mContext,
            0,
            &sbd,
            0);

        RCF_VERIFY(
            status == SEC_E_OK,
            Exception(RcfError_SspiEncrypt, "EncryptMessage()", osError(status)));
            
        RCF_ASSERT(rgsb[0].cbBuffer == cbHeader);
        RCF_ASSERT(rgsb[1].cbBuffer == cbMsg);
        RCF_ASSERT(rgsb[2].cbBuffer <= cbTrailer);

        cbTrailer               = rgsb[2].cbBuffer;
        cbPacket                = cbHeader + cbMsg + cbTrailer;
        resizeWriteBuffer(cbPacket);
    }

    bool SspiFilter::decryptReadBufferSchannel()
    {
        // decrypt read buffer in place

        RCF_ASSERT(mContextState == AuthOkAck);

        BYTE *pMsg              = ((BYTE *) mReadBuffer);
        DWORD cbMsg             = static_cast<DWORD>(mReadBufferPos);
        SecBuffer rgsb[4]       = {0};
        
        rgsb[0].cbBuffer        = cbMsg;
        rgsb[0].BufferType      = SECBUFFER_DATA;
        rgsb[0].pvBuffer        = pMsg;

        rgsb[1].cbBuffer        = 0;
        rgsb[1].BufferType      = SECBUFFER_EMPTY;
        rgsb[1].pvBuffer        = NULL;

        rgsb[2].cbBuffer        = 0;
        rgsb[2].BufferType      = SECBUFFER_EMPTY;
        rgsb[2].pvBuffer        = NULL;

        rgsb[3].cbBuffer        = 0;
        rgsb[3].BufferType      = SECBUFFER_EMPTY;
        rgsb[3].pvBuffer        = NULL;

        SecBufferDesc sbd       = {0};
        sbd.ulVersion           = SECBUFFER_VERSION;
        sbd.cBuffers            = sizeof(rgsb)/sizeof(*rgsb);
        sbd.pBuffers            = rgsb;
        ULONG qop               = 0;

        SECURITY_STATUS status  = getSft()->DecryptMessage(
            &mContext,
            &sbd,
            0,
            &qop);

        if (status == SEC_E_INCOMPLETE_MESSAGE)
        {
            // Not enough data.
            std::size_t readBufferPos = mReadBufferPos;
            resizeReadBuffer(mReadBufferPos + mReadAheadChunkSize);
            mReadBufferPos = readBufferPos;
            readBuffer();
            return false;
        }
        else
        {
            for (int i=1; i<4; ++i)
            {
                if (rgsb[i].BufferType == SECBUFFER_EXTRA)
                {
                    // Found extra data - set a marker where it begins.
                    char * pRemainingData = (char *) rgsb[i].pvBuffer;
                    mRemainingDataPos = pRemainingData - mReadBuffer;
                    RCF_ASSERT(0 < mRemainingDataPos && mRemainingDataPos < mReadBufferPos);
                    break;
                }
            }            
        }

        trimReadBuffer();

        RCF_VERIFY(
            status == SEC_E_OK,
            Exception(RcfError_SspiDecrypt, "DecryptMessage()", osError(status)));

        RCF_ASSERT(rgsb[0].BufferType == SECBUFFER_STREAM_HEADER);
        RCF_ASSERT(rgsb[1].BufferType == SECBUFFER_DATA);
        RCF_ASSERT(rgsb[2].BufferType == SECBUFFER_STREAM_TRAILER);

        DWORD cbHeader          = rgsb[0].cbBuffer;
        DWORD cbData            = rgsb[1].cbBuffer;
        DWORD cbTrailer         = rgsb[2].cbBuffer;

        RCF_UNUSED_VARIABLE(cbTrailer);

        mReadBufferPos          = cbHeader;
        mReadBufferLen          = cbHeader + cbData;

        return true;
    }

    bool SspiServerFilter::doHandshakeSchannel()
    {
        // Use the block in the read buffer to proceed through the handshake procedure.

        // Lazy acquiring of implicit credentials.
        if (!mHaveCredentials)
        {
            acquireCredentials();
        }

        DWORD cbPacket          = mPkgInfo.cbMaxToken;

        SecBuffer ob            = {0};
        ob.BufferType           = SECBUFFER_TOKEN;
        ob.cbBuffer             = 0;
        ob.pvBuffer             = NULL;

        SecBufferDesc obd       = {0};
        obd.cBuffers            = 1;
        obd.ulVersion           = SECBUFFER_VERSION;
        obd.pBuffers            = &ob;
        
        SecBuffer ib[2]         = {0};
        ib[0].BufferType        = SECBUFFER_TOKEN;
        ib[0].cbBuffer          = static_cast<DWORD>(mReadBufferPos);
        ib[0].pvBuffer          = mReadBuffer;
        ib[1].BufferType        = SECBUFFER_EMPTY;
        ib[1].cbBuffer          = 0;
        ib[1].pvBuffer          = NULL;

        SecBufferDesc ibd       = {0};
        ibd.cBuffers            = 2;
        ibd.ulVersion           = SECBUFFER_VERSION;
        ibd.pBuffers            = ib;

        DWORD contextRequirements = mContextRequirements;
        if ( mCertValidationCallback || mAutoCertValidation.size() > 0 )
        {
            // Need this to get the client to send the server a certificate.
            contextRequirements |= ASC_REQ_MUTUAL_AUTH;
        }

        DWORD   CtxtAttr        = 0;
        TimeStamp Expiration    = {0};
        SECURITY_STATUS status  = getSft()->AcceptSecurityContext(
            &mCredentials,
            mHaveContext ? &mContext : NULL,
            &ibd,
            contextRequirements,
            SECURITY_NATIVE_DREP,
            &mContext,
            &obd,
            &CtxtAttr,
            &Expiration);

        switch (status)
        {
        case SEC_E_OK:
        case SEC_I_CONTINUE_NEEDED:
        case SEC_I_COMPLETE_NEEDED:
        case SEC_I_COMPLETE_AND_CONTINUE:
            mHaveContext = true;
            break;
        default:
            break;
        }

        cbPacket = ob.cbBuffer;

        RCF_ASSERT(
            status != SEC_I_COMPLETE_AND_CONTINUE &&
            status != SEC_I_COMPLETE_NEEDED)
            (status);

        if (status == SEC_E_INCOMPLETE_MESSAGE)
        {
            // Not enough data.
            std::size_t readBufferPos = mReadBufferPos;
            resizeReadBuffer(mReadBufferPos + mReadAheadChunkSize);
            mReadBufferPos = readBufferPos;
            readBuffer();
            return false;
        }
        else if (ib[1].BufferType == SECBUFFER_EXTRA)
        {
            // We consider any extra data at this stage to be a protocol error.
            Exception e(RcfError_SspiHandshakeExtraData);
            RCF_THROW(e);
        }

        trimReadBuffer();
        
        if (status == SEC_I_CONTINUE_NEEDED)
        {
            // Authorization ok so far, copy outbound data to write buffer.
            resizeWriteBuffer(ob.cbBuffer);
            memcpy(mWriteBuffer, ob.pvBuffer, ob.cbBuffer);
            getSft()->FreeContextBuffer(ob.pvBuffer);
        }
        else if (status == SEC_E_OK)
        {
            // Authorization ok, send last handshake block to the client.
            mContextState = AuthOk;
            RCF_ASSERT(cbPacket > 0);
            resizeWriteBuffer(cbPacket);
            memcpy(mWriteBuffer, ob.pvBuffer, ob.cbBuffer);
            getSft()->FreeContextBuffer(ob.pvBuffer);

            // Extract the peer certificate.
            PCCERT_CONTEXT pRemoteCertContext = NULL;

            status = getSft()->QueryContextAttributes(
                &mContext,
                SECPKG_ATTR_REMOTE_CERT_CONTEXT,
                (PVOID)&pRemoteCertContext);

            if (pRemoteCertContext)
            {
                mRemoteCertPtr.reset( new Win32Certificate(pRemoteCertContext) );
            }

            // If we have a custom validation callback, call it.
            if ( mCertValidationCallback )
            {
                bool validated = mCertValidationCallback(mRemoteCertPtr.get());
                if ( !validated )
                {
                    Exception e(RcfError_SslCertVerificationCustom);
                    RCF_THROW(e);
                }
            }       
        }
        else
        {
            std::string errorMsg;

            // First check if the client has gotten its port numbers mixed up and sent us a HTTP message by mistake.
            std::string msg(mReadBuffer, mReadBufferPos);
            std::size_t linePos = msg.find('\n');
            std::size_t httpPos = msg.find("HTTP");
            if ( httpPos < linePos )
            {
                std::string body = "<html><body>Invalid protocol (use HTTPS).</body></html>";
                std::string bodyLen = std::to_string(body.length());
                std::string response =
                    "HTTP/1.0 400 Bad Request\r\n"
                    "X-RCFError: Invalid protocol (use HTTPS)\r\n"
                    "Content-Type: text/html\r\n"
                    "Content-Length: " + bodyLen + "\r\n"
                    "Connection: Closed\r\n\r\n"
                    + body;

                errorMsg = response;
            }
            else
            {
                // Authorization failed.
                RCF_LOG_2() << "Schannel SSL handshake failed. Error: " + RCF::osError(status);

                errorMsg = "Schannel SSL handshake failed. Error: " + RCF::osError(status);
            }

            resizeWriteBuffer(errorMsg.length());
            memcpy(mWriteBuffer, errorMsg.c_str(), errorMsg.length());
        }

        return true;
    }

    bool SspiClientFilter::doHandshakeSchannel()
    {
        // use the block in the read buffer to proceed through the handshake procedure

        // lazy acquiring of implicit credentials
        if (mImplicitCredentials && !mHaveCredentials)
        {
            acquireCredentials();
        }
     
        SecBuffer ob            = {0};
        ob.BufferType           = SECBUFFER_TOKEN;
        ob.cbBuffer             = 0;
        ob.pvBuffer             = NULL;

        SecBufferDesc obd       = {0};
        obd.cBuffers            = 1;
        obd.ulVersion           = SECBUFFER_VERSION;
        obd.pBuffers            = &ob;

        SecBuffer ib[2]         = {0};
        
        ib[0].BufferType        = SECBUFFER_TOKEN;
        ib[0].cbBuffer          = static_cast<DWORD>(mReadBufferPos);
        ib[0].pvBuffer          = mReadBuffer;

        ib[1].BufferType        = SECBUFFER_EMPTY;
        ib[1].cbBuffer          = 0;
        ib[1].pvBuffer          = NULL;

        SecBufferDesc ibd       = {0};
        ibd.cBuffers            = 2;
        ibd.ulVersion           = SECBUFFER_VERSION;
        ibd.pBuffers            = ib;

        tstring strTarget       = mAutoCertValidation;
        const TCHAR *target     = strTarget.empty() ? RCF_T("") : strTarget.c_str();
        DWORD CtxtAttr          = 0;
        TimeStamp Expiration    = {0};

        DWORD contextRequirements = mContextRequirements;
        if (mLocalCertPtr && mLocalCertPtr->getWin32Context())
        {
            contextRequirements |= ISC_REQ_USE_SUPPLIED_CREDS;
        }

        SECURITY_STATUS status  = getSft()->InitializeSecurityContext(
            &mCredentials,
            mHaveContext ? &mContext : NULL,
            (TCHAR *) target,
            mContextRequirements,
            0,
            SECURITY_NATIVE_DREP,
            mHaveContext ? &ibd : NULL,
            0,
            &mContext,
            &obd,
            &CtxtAttr,
            &Expiration);

        if ( status == SEC_I_INCOMPLETE_CREDENTIALS )
        {
            // We're in here if the server has requested a client certificate but the client hasn't provided one.
            // We proceed anyway, and let the server-side application code decide what to do.

            contextRequirements |= ISC_REQ_USE_SUPPLIED_CREDS;

            status = getSft()->InitializeSecurityContext(
                &mCredentials,
                mHaveContext ? &mContext : NULL,
                (TCHAR *)target,
                mContextRequirements,
                0,
                SECURITY_NATIVE_DREP,
                mHaveContext ? &ibd : NULL,
                0,
                &mContext,
                &obd,
                &CtxtAttr,
                &Expiration);
        }

        if ( status == SEC_E_INVALID_TOKEN )
        {
            // The data may be a plain text error message, so display it to the user.
            std::size_t displayLen = RCF_MIN(std::size_t(1024), mReadBufferPos);
            std::string msg(mReadBuffer, displayLen);
            RCF::Exception e(RcfError_NonSSLResponse, msg);
            throw e;
        }

        switch (status)
        {
        case SEC_E_OK:
        case SEC_I_CONTINUE_NEEDED:
        case SEC_I_COMPLETE_NEEDED:
        case SEC_I_COMPLETE_AND_CONTINUE:
        case SEC_I_INCOMPLETE_CREDENTIALS:
            mHaveContext = true;
            break;
        default:
            break;
        }

        RCF_ASSERT(
            status != SEC_I_COMPLETE_NEEDED &&
            status != SEC_I_COMPLETE_AND_CONTINUE)
            (status);

        if (status == SEC_E_INCOMPLETE_MESSAGE)
        {
            // Not enough data.
            std::size_t readBufferPos = mReadBufferPos;            
            resizeReadBuffer(mReadBufferPos + mReadAheadChunkSize);
            mReadBufferPos = readBufferPos;
            readBuffer();
            return false;
        }
        else if (ib[1].BufferType == SECBUFFER_EXTRA)
        {
            // We consider any extra data at this stage to be a protocol error.
            Exception e(RcfError_SspiHandshakeExtraData);
            RCF_THROW(e);
        }

        trimReadBuffer();
            
        if (status == SEC_I_CONTINUE_NEEDED)
        {
            // Handshake OK so far.

            RCF_ASSERT(ob.cbBuffer);
            mContextState = AuthContinue;
            resizeWriteBuffer(ob.cbBuffer);
            memcpy(mWriteBuffer, ob.pvBuffer, ob.cbBuffer);
            getSft()->FreeContextBuffer(ob.pvBuffer);
            return true;
        }
        else if (status == SEC_E_OK)
        {
            // Handshake OK.

            // Extract the peer certificate.
            PCCERT_CONTEXT pRemoteCertContext = NULL;

            status = getSft()->QueryContextAttributes(
                &mContext,
                SECPKG_ATTR_REMOTE_CERT_CONTEXT,
                (PVOID)&pRemoteCertContext);

            mRemoteCertPtr.reset( new Win32Certificate(pRemoteCertContext) );

            // If we have a custom validation callback, call it.
            if (mCertValidationCallback)
            {
                bool ok = mCertValidationCallback(mRemoteCertPtr.get());
                if (!ok)
                {
                    Exception e( RcfError_SslCertVerificationCustom );
                    RCF_THROW(e);
                }
            }

            // And now back to business.
            mContextState = AuthOkAck;
            resumeUserIo();
            return false;
        }
        else
        {
            Exception e(RcfError_SspiAuthFailClient, osError(status));
            RCF_THROW(e);
            return false;
        }
    }

    void SspiFilter::setupCredentialsSchannel()
    {
        SCHANNEL_CRED schannelCred          = {0};       
        schannelCred.dwVersion              = SCHANNEL_CRED_VERSION;
        PCCERT_CONTEXT pCertContext         = NULL;
        if(mLocalCertPtr)
        {
            pCertContext                    = mLocalCertPtr->getWin32Context();
            schannelCred.cCreds             = 1;
            schannelCred.paCred             = &pCertContext;
        }

        schannelCred.grbitEnabledProtocols  = mEnabledProtocols;

        if (mServer)
        {
            if (mCertValidationCallback)
            {
                // Server side manual validation.
                schannelCred.dwFlags            = SCH_CRED_MANUAL_CRED_VALIDATION;
            }
            else
            {
                // Server side auto validation.
                schannelCred.dwFlags            = 0;
            }
        }
        else
        {
            if (mCertValidationCallback)
            {
                // Client side manual validation.
                schannelCred.dwFlags            = SCH_CRED_NO_DEFAULT_CREDS | SCH_CRED_MANUAL_CRED_VALIDATION;
            }
            else
            {
                // Client side auto validation.
                schannelCred.dwFlags            = SCH_CRED_NO_DEFAULT_CREDS | SCH_CRED_AUTO_CRED_VALIDATION;
            }
        }

        SECURITY_STATUS status = getSft()->AcquireCredentialsHandle(
            NULL,
            UNISP_NAME,
            mServer ? SECPKG_CRED_INBOUND : SECPKG_CRED_OUTBOUND ,
            NULL,
            &schannelCred,
            NULL, 
            NULL,
            &mCredentials,
            NULL);

        if (status != SEC_E_OK)
        {
            Exception e(RcfError_Sspi, "AcquireCredentialsHandle()", osError(status));
            RCF_THROW(e);
        }

        mHaveCredentials = true;
    }

    SchannelServerFilter::SchannelServerFilter(
        RcfServer & server,
        DWORD enabledProtocols,
        ULONG contextRequirements) :
            SspiServerFilter(UNISP_NAME, RCF_T(""), BoolSchannel)
    {
        CertificatePtr certificatePtr = server.getCertificate();
        Win32CertificatePtr certificateBasePtr = std::dynamic_pointer_cast<Win32Certificate>(certificatePtr);
        if (certificateBasePtr)
        {
            mLocalCertPtr = certificateBasePtr;
        }

        mCertValidationCallback = server.getCertificateValidationCallback();
        mAutoCertValidation = server.getEnableSchannelCertificateValidation();

        mContextRequirements = contextRequirements;
        mEnabledProtocols = enabledProtocols;
    }

    static const ULONG DefaultSchannelContextRequirements =
            ASC_REQ_SEQUENCE_DETECT 
        |   ASC_REQ_REPLAY_DETECT 
        |   ASC_REQ_CONFIDENTIALITY 
        |   ASC_REQ_EXTENDED_ERROR 
        |   ASC_REQ_ALLOCATE_MEMORY 
        |   ASC_REQ_STREAM;

    static const DWORD DefaultSchannelServerProtocols = 
            SP_PROT_TLS1_2_SERVER 
        /*| SP_PROT_TLS1_1_SERVER 
        |   SP_PROT_TLS1_0_SERVER*/;

    static const DWORD DefaultSchannelClientProtocols = 
            SP_PROT_TLS1_2_CLIENT 
        |   SP_PROT_TLS1_1_CLIENT 
        |   SP_PROT_TLS1_0_CLIENT;

    SchannelFilterFactory::SchannelFilterFactory()
    {
    }

    FilterPtr SchannelFilterFactory::createFilter(RcfServer & server)
    {
        DWORD dwProtocols = server.getSchannelEnabledProtocols();
        if (dwProtocols == 0)
        {
            dwProtocols = DefaultSchannelServerProtocols;
        }

        ULONG contextRequirements = server.getSchannelContextRequirements();
        if (contextRequirements == 0)
        {
            contextRequirements = DefaultSchannelContextRequirements;
        }

        std::shared_ptr<SchannelServerFilter> filterPtr(
            new SchannelServerFilter(server, dwProtocols, contextRequirements));

        return filterPtr;
    }

    SchannelClientFilter::SchannelClientFilter(ClientStub * pClientStub) :
            SspiClientFilter(
                pClientStub,
                Smp_Encryption,
                0, 
                UNISP_NAME, 
                RCF_T(""),
                BoolSchannel)
    {
        mEnabledProtocols = pClientStub->getSchannelEnabledProtocols();
        if (mEnabledProtocols == 0)
        {
            mEnabledProtocols = DefaultSchannelClientProtocols;
        }

        mContextRequirements = pClientStub->getSchannelContextRequirements();
        if (mContextRequirements == 0)
        {
            mContextRequirements = DefaultSchannelContextRequirements;
        }

        CertificatePtr certificatePtr = pClientStub->getCertificate();
        Win32CertificatePtr certificateBasePtr = std::dynamic_pointer_cast<Win32Certificate>(certificatePtr);
        if (certificateBasePtr)
        {
            mLocalCertPtr = certificateBasePtr;
        }

        mCertValidationCallback = pClientStub->getCertificateValidationCallback();
        mAutoCertValidation = pClientStub->getEnableSchannelCertificateValidation();
    }

    Win32CertificatePtr SspiFilter::getPeerCertificate()
    {
        return mRemoteCertPtr;
    }

    PCtxtHandle SspiFilter::getSecurityContext() const
    {
        return (PCtxtHandle) &mContext;
    }

} // namespace RCF
